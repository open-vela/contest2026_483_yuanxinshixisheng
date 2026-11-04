/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "duplex_session.h"
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#include "k7_apm21_live_wiring.h"
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
#include "k7_apm21_idle_events.h"
#endif
#endif
#include <errno.h>
#include <string.h>
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#include <stdio.h>
struct rx_start_snapshot {
  uint64_t captured,before_us,after_us;
  uint32_t generation,half,poll_rx,prefill_tx,anchor_rx,anchor_tx,anchor_flags;
  unsigned valid;
};
struct rx_group_fault {
  struct rx_start_snapshot start;
  uint64_t captured;
  uint32_t words[8],poll_rx,half,tx,generation,queued;
  uint32_t xfer,status,rx_fifo,tx_fifo,read_failures;
  unsigned valid;
};
#endif
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
#include "session_capture.h"
#endif
#define BIT(n) (UINT32_C(1) << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#include "input/rockchip_sai.h"
/* Conservative software guard pending physical ES8388/loaded-amplifier
 * measurement; this is NOT a guaranteed codec/DAC latency specification.
 * RX and serial clocks continue during the guard, with no fabricated PCM. */
#define STREAM_TAIL_US 30000u
static int rd(const struct pio_port *p, uint32_t o, uint32_t *v)
{ return p->read(p->ctx, o, v) ? -EIO : 0; }
static int wr(const struct pio_port *p, uint32_t o, uint32_t v)
{ return p->write(p->ctx, o, v) ? -EIO : 0; }
static int mod(const struct pio_port *p, uint32_t o, uint32_t m, uint32_t v)
{ uint32_t old; return rd(p, o, &old) ? -EIO : wr(p, o, (old & ~m) | (v & m)); }
static unsigned count(uint32_t v)
{ return (v & 63u) + ((v >> 6) & 63u) + ((v >> 12) & 63u) + ((v >> 18) & 63u); }
/* RXDR visits bank0,0,1,1,2,2,3,3. Each read consumes one 32-bit
 * entry: wait for both L/R entries in every bank before the fixed 8 reads.
 * A nonzero bank alone would make its second read access an empty FIFO. */
static int group_ready(uint32_t v)
{ return (v & 63u) >= 2 && ((v >> 6) & 63u) >= 2 && ((v >> 12) & 63u) >= 2 && ((v >> 18) & 63u) >= 2; }
/* Optional diagnostics only. The same monotonic microsecond source brackets
 * sequential FIFO reads and, for start/stop, the corresponding operation.
 * No presumed DATA_CNT meaning, DAC timestamp or acoustic-delay estimate. */
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
static void anchor_observed(const struct pio_port *p, struct k7_voice_session *s,
                   const struct k7_duplex_session_result *r, unsigned event,
                   uint32_t generation, unsigned queued, unsigned discarded,
                   int result, uint64_t before,struct rx_start_snapshot *seen)
#else
static void anchor(const struct pio_port *p, struct k7_voice_session *s,
                   const struct k7_duplex_session_result *r, unsigned event,
                   uint32_t generation, unsigned queued, unsigned discarded,
                   int result, uint64_t before)
#endif
{
  if (!k7_voice_session_anchors_enabled(s)) return;
  struct k7_voice_anchor a = {.before_us = before == UINT64_MAX ? p->us(p->ctx) : before,
    .rx_samples = r->captured, .generation = generation, .tx_queued_samples = queued,
    .discarded_words = discarded, .event = event, .result = result};
  uint32_t rx = 0, tx = 0;
  if (rd(p, SAI_RXFIFOLR, &rx)) a.flags |= K7_VOICE_ANCHOR_IO_ERROR;
  if (rd(p, SAI_TXFIFOLR, &tx)) a.flags |= K7_VOICE_ANCHOR_IO_ERROR;
  a.after_us = p->us(p->ctx); a.rx_fifo_words = count(rx); a.tx_fifo_words = count(tx);
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
  /* Reuse these pre-existing TX_START anchor reads. No additional healthy
   * PIO MMIO or timestamp read is introduced by the diagnostic snapshot. */
  if(seen){seen->before_us=a.before_us;seen->after_us=a.after_us;
    seen->anchor_rx=rx;seen->anchor_tx=tx;seen->anchor_flags=a.flags;seen->valid=1;}
#endif
  (void)k7_voice_session_record_anchor(s, &a); /* loss never blocks the pump */
}
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#define anchor(...) anchor_observed(__VA_ARGS__,NULL)
#endif
static int low(const struct pio_port *p)
{
  uint64_t before = p->us(p->ctx); int rc = p->amp_off_fast(p->ctx);
  uint64_t after = p->us(p->ctx);
  return rc ? -EIO : after < before || after - before >= 100 ? -ETIMEDOUT : 0;
}
/* Control completion observation only. AMP_OFF result/interval does not
 * establish codec output zeros or acoustic-tail completion. Default anchors
 * off performs the same low() calls; diagnostics add no TXDR writes. */
static int mute(const struct pio_port *p,struct k7_voice_session *s,
                const struct k7_duplex_session_result *r,uint32_t generation,
                unsigned queued,unsigned discarded)
{
  bool enabled=k7_voice_session_anchors_enabled(s);
  uint64_t before=enabled?p->us(p->ctx):0;
  int rc=low(p);
  if(enabled)anchor(p,s,r,K7_VOICE_ANCHOR_AMP_OFF,generation,queued,discarded,rc,before);
  return rc;
}
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
/* A successful transition is emitted once per actual AMP_ON. The eager
 * cancellation path may already have muted before a terminal-state branch.
 * I/O failure and owner cleanup still perform low() even if already muted;
 * they do not fabricate a second successful AMP_OFF transition. A failed
 * low leaves enabled true so final cleanup retries the physical operation. */
static int mute_transition(const struct pio_port*p,struct k7_voice_session*s,
 const struct k7_duplex_session_result*r,uint32_t generation,unsigned queued,
 unsigned discarded,bool*enabled,bool force_low){
 if(!*enabled)return force_low?low(p):0;
 int rc=mute(p,s,r,generation,queued,discarded);if(!rc)*enabled=false;return rc;
}
#define MUTE_NORMAL(...) mute_transition(__VA_ARGS__,&amp_enabled,false)
#define MUTE_SAFE(...) mute_transition(__VA_ARGS__,&amp_enabled,true)
#endif
static int wait_bits(const struct pio_port *p, uint32_t reg, uint32_t mask, uint32_t want,
                      uint64_t start, unsigned budget)
{
  for (unsigned i = 0; i < 10000; ++i) {
    uint32_t v; if (rd(p, reg, &v)) return -EIO;
    uint64_t now = p->us(p->ctx);
    if (now < start || now - start >= budget) return -ETIMEDOUT;
    if ((v & mask) == want) return 0;
  }
  return -ETIMEDOUT;
}
/* Tags follow FIFO order, not PCM value (real speech may itself contain 0).
 * bit 0 is the oldest unobserved word. Hardware consumption only removes a
 * prefix. An impossible growing count is an error, conservatively all source. */
struct fifo_tags { unsigned words; uint32_t source; };
static int observe(struct fifo_tags *f, unsigned words)
{
  if (words > 16) return -EPROTO;
  if (words > f->words) { f->words = words; f->source = (1u << words) - 1u; return -EIO; }
  f->source >>= f->words - words; f->words = words; return 0;
}
static unsigned source_words(const struct fifo_tags *f)
{
  unsigned n = 0; uint32_t bits = f->source;
  while (bits) { n += bits & 1u; bits >>= 1; }
  return n;
}
static int put_word(const struct pio_port *p, struct fifo_tags *f, uint32_t value, bool source)
{
  if (f->words >= 16) return -EPROTO;
  if (wr(p, SAI_TXDR, value)) return -EIO;
  if (source) f->source |= 1u << f->words;
  f->words++; return 0;
}
/* The optional reference sees completed stereo pairs including gap zeros.
 * Failed half writes revoke all qualification; it cannot change pump results. */
static int put_pair(const struct pio_port *p, struct k7_voice_session *s,
                    struct fifo_tags *f, int16_t sample, bool source,
                    uint64_t source_index, uint32_t generation)
{
  bool enabled=k7_voice_session_fifo_enabled(s);
  uint64_t before=enabled?p->us(p->ctx):0;
  uint32_t word=(uint32_t)((int32_t)sample*65536);
  int rc=put_word(p,f,word,source);
  if(!rc)rc=put_word(p,f,word,source);
  if(enabled){
    uint64_t after=p->us(p->ctx);
    if(rc)k7_voice_session_fifo_revoke(s,K7_FIFO_REF_HALF_WRITE|K7_FIFO_REF_IO);
    else k7_voice_session_fifo_pair(s,generation,sample,!source,source_index,before,after);
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
    if(rc)k7_apm21_tap_reference_observe(k7_voice_session_apm21_tap(s),generation,k7_voice_session_input_epoch(s),k7_voice_session_anchor_epoch(s),f->words,rc,after,0);
#endif
    if(rc)k7_apm21_tap_gap(k7_voice_session_apm21_tap(s),rc);
    else k7_apm21_tap_pair(k7_voice_session_apm21_tap(s),sample,!source,before,after);
#endif
  }
  return rc;
}
static int stop_all(const struct pio_port *p, struct fifo_tags *f, unsigned *discarded,
                    struct k7_duplex_session_result *r, bool discard_recorded)
{
  uint64_t start = p->us(p->ctx); uint32_t v;
  int rc = mod(p, SAI_XFER, 12, 0);
  if (!rc) rc = wait_bits(p, SAI_STATUS, 12, 12, start, 1000);
  if (!rc) {
    rc = rd(p, SAI_TXFIFOLR, &v);
    if (!rc) {
      if (observe(f, count(v))) r->tx_provenance_errors++;
      unsigned valid = source_words(f);
      if (!discard_recorded) {
        *discarded += valid; r->tx_discarded_silence_words += f->words - valid;
      }
    }
  }
  if (!rc) rc = mod(p, SAI_CLR, 3, 3);
  if (!rc) rc = wait_bits(p, SAI_CLR, 3, 0, start, 1000);
  if (!rc) rc = mod(p, SAI_XFER, 3, 0);
  if (!rc) rc = wait_bits(p, SAI_STATUS, SAI_STATUS_FS_IDLE, SAI_STATUS_FS_IDLE, start, 2000);
  if (!rc && (rd(p, SAI_XFER, &v) || (v & 15))) rc = -EIO;
  if (!rc && (rd(p, SAI_TXFIFOLR, &v) || count(v))) rc = -EIO;
  if (!rc && (rd(p, SAI_RXFIFOLR, &v) || count(v))) rc = -EIO;
  return rc;
}
/* Keep the serial transmitter alive during an opted-in refill. These words
 * are an explicit physical gap, not source PCM: no cursor/render/completion
 * accounting is advanced. Timing anchors invalidate mapping across the gap. */
static int silence(const struct pio_port *p, struct k7_voice_session *s, struct fifo_tags *f, struct k7_duplex_session_result *r)
{
  if (put_pair(p,s,f,0,false,UINT64_MAX,k7_voice_session_tx_generation(s))) return -EIO;
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  k7_session_capture_fifo_zero(s);
#else
  (void)s;
#endif
  r->tx_silence_samples++; return 0;
}
static int enqueue(const struct pio_port *p, struct k7_voice_session *s, unsigned *queued,
                    const struct k7_duplex_session_result *r, uint32_t generation, struct fifo_tags *f)
{
  int16_t sample; uint64_t index;
  int got = k7_voice_session_tx_pull(s, &sample, &index); if (got <= 0) return got;
  if (put_pair(p,s,f,sample,true,index,generation)) return -EIO;
  (*queued)++;
  int rc = k7_voice_session_render(s, sample, index);
  if (k7_voice_session_anchors_enabled(s) && !(*queued % K7_VOICE_FRAME_SAMPLES))
    anchor(p, s, r, K7_VOICE_ANCHOR_TX_FRAME, generation, *queued, 0, rc, UINT64_MAX);
  return rc ? rc : 1;
}
static void complete_turn(const struct pio_port *p, struct k7_voice_session *s,
                          struct k7_duplex_session_result *r, uint32_t generation,
                          unsigned queued, unsigned discarded, int result, bool reference_drained)
{
  if (k7_voice_session_anchors_enabled(s))
    anchor(p, s, r, K7_VOICE_ANCHOR_TX_COMPLETE, generation, queued, discarded, result, UINT64_MAX);
  k7_voice_session_fifo_end(s,result,discarded,reference_drained);
  k7_voice_session_tx_complete(s, result, queued, discarded);
  r->turns++; r->queued += queued;
  if (result == -ECANCELED) r->cancelled_turns++;
  else if (result) r->failed_turns++;
}
int pio_duplex_session(const struct pio_port *p, int prepared, uint32_t mclk,
                        struct k7_voice_session *s, struct k7_duplex_session_result *r)
{
  if (!r) return -EINVAL;
  memset(r, 0, sizeof(*r));
  if (!p || !s || !p->read || !p->write || !p->us || !p->clocks_started ||
      !p->streams_started || !p->amp_off_fast) return r->result = -EINVAL;
  if (!prepared || mclk != 4096000u) return r->result = -ENOTSUP;
  if (k7_voice_session_stop_requested(s)) return r->result = -ECANCELED;
  uint32_t v, raw;
  enum { OFF, PLAYING, STOP_IDLE, STOP_CLEAR, PAUSED, EOF_TAIL, ZERO_DRAIN
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION && defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
    , STOP_RX
#endif
  } tx = OFF;
  unsigned queued = 0, discarded = 0; bool eof = false;
  struct fifo_tags fifo = {0};
  bool stream_policy = false, amp_enabled = false, discard_recorded = false;
  bool reference_drained = false;
  const bool anchors = k7_voice_session_anchors_enabled(s);
  uint32_t generation = 0;
  int turn_rc = 0, rc = -EIO;
  uint64_t previous, last_rx, stop_began = 0, pause_began = 0, tail_began = 0;
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE && defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
  uint64_t zero_stop_began = 0;
#endif
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
  uint64_t round_wait_began=0,round_wait_mic=0;
  uint32_t round_wait_generation=0,eof_quiet_generation=0;
  bool round_waiting=false,eof_quiet_pending=false;
  struct rx_group_fault rx_fault={0};
#endif
  r->held = 1;
  if (rd(p, SAI_VERSION, &v)) return r->result = -EIO;
  if (v != SAI_VER_2307) return r->result = -ENOTSUP;
  if (rd(p, SAI_XFER, &v) || (v & 15) || rd(p, SAI_STATUS, &v) || (v & 14) != 14 ||
      rd(p, SAI_DMACR, &v) || (v & (SAI_DMACR_RDE_MASK | SAI_DMACR_TDE_MASK)) ||
      rd(p, SAI_MONO_CR, &v) || v || rd(p, SAI_TXFIFOLR, &v) || count(v) ||
      rd(p, SAI_RXFIFOLR, &v) || count(v)) return r->result = -EBUSY;
  if (rd(p, SAI_PATH_SEL, &v)) return r->result = -EIO;
  if (v & 0x30000u) return r->result = -ENOTSUP;
  const uint32_t mask = SAI_XCR_START_SEL_MASK | SAI_XCR_EDGE_SHIFT_MASK | SAI_XCR_CSR_MASK |
    SAI_XCR_SJM_MASK | SAI_XCR_FBM_MASK | SAI_XCR_SNB_MASK | SAI_XCR_VDJ_MASK | SAI_XCR_SBW_MASK | SAI_XCR_VDW_MASK;
  const uint32_t base = SAI_XCR_EDGE_SHIFT_1 | SAI_XCR_SNB(2) | SAI_XCR_VDJ_L | SAI_XCR_SBW(32) | SAI_XCR_VDW(32);
  if (mod(p, SAI_TXCR, mask, base | SAI_XCR_CSR(1)) || mod(p, SAI_RXCR, mask, base | SAI_XCR_CSR(4)) ||
      mod(p, SAI_TX_SHIFT, SAI_XSHIFT_RIGHT_MASK, 2) || mod(p, SAI_RX_SHIFT, SAI_XSHIFT_RIGHT_MASK, 2) ||
      mod(p, SAI_FSCR, SAI_FSCR_EDGE_MASK | SAI_FSCR_FW_MASK | SAI_FSCR_FPW_MASK,
          SAI_FSCR_EDGE_DUAL | SAI_FSCR_FW(64) | SAI_FSCR_FPW(32)) ||
      mod(p, SAI_CKR, SAI_CKR_MDIV_MASK | SAI_CKR_MSS_MASK | SAI_CKR_CKP_MASK | SAI_CKR_FSP_MASK, SAI_CKR_MDIV(4)) ||
      mod(p, SAI_PATH_SEL, 0xff03u, 0) || mod(p, SAI_INTCR, SAI_INTCR_TXUIE_MASK | SAI_INTCR_RXOIE_MASK, 0) ||
      mod(p, SAI_INTCR, SAI_INTCR_TXUIC | SAI_INTCR_RXOIC, SAI_INTCR_TXUIC | SAI_INTCR_RXOIC) ||
      mod(p, SAI_XFER, 3, 3)) goto done;
  previous = p->us(p->ctx); rc = p->clocks_started(p->ctx);
  uint64_t now = p->us(p->ctx);
  if (rc || now < previous || now - previous >= 100000) { rc = rc ? rc : -ETIMEDOUT; goto done; }
  if (mod(p, SAI_XFER, SAI_XFER_RXS_MASK, SAI_XFER_RXS_EN)) { rc = -EIO; goto done; }
  previous = last_rx = p->us(p->ctx); k7_voice_session_ready(s, true);
  for (;;) {
    now = p->us(p->ctx);
    if (now < previous || now - last_rx >= 100000) { rc = -ETIMEDOUT; goto done; }
    previous = now;
    if (k7_voice_session_stop_requested(s)) { rc = -ECANCELED; goto done; }
    if (rd(p, SAI_INTSR, &v)) { rc = -EIO; goto done; }
    if (v & SAI_INTSR_RXOI_ACT) { r->rx_overflows++; rc = -EOVERFLOW; goto done; }
    bool tx_fault = (v & SAI_INTSR_TXUI_ACT) != 0;
    if(tx_fault)k7_voice_session_fifo_revoke(s,K7_FIFO_REF_COUNT|K7_FIFO_REF_IO);
    if (tx_fault && (tx == OFF || tx == EOF_TAIL)) { rc = -EIO; r->tx_underflows++; goto done; }
    /* A late hardware fault during drain/stop is never a successful pause. */
    if (tx_fault && (tx == STOP_IDLE || tx == STOP_CLEAR) && !turn_rc) {
      turn_rc = -EPIPE; r->tx_underflows++;
      rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_SAFE(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false;
    }
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION && defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
    if (tx == STOP_RX) {
      if (tx_fault) { rc = -EPIPE; r->tx_underflows++; goto done; }
      if (now < stop_began || now - stop_began >= 1000u) { rc = -ETIMEDOUT; goto done; }
    }
#endif
    if (rd(p, SAI_RXFIFOLR, &raw) || rd(p, SAI_TXFIFOLR, &v)) { rc = -EIO; goto done; }
    unsigned rx_count = count(raw), tx_count = count(v);
    if (rx_count > r->max_rx_fifo) r->max_rx_fifo = rx_count;
    if (tx_count > r->max_tx_fifo) r->max_tx_fifo = tx_count;
    if (rx_count > 128 || tx_count > 16) { k7_voice_session_fifo_revoke(s,K7_FIFO_REF_COUNT); rc = -EPROTO; goto done; }
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
    int stage_observed=observe(&fifo,tx_count);
    k7_apm21_tap_reference_observe(k7_voice_session_apm21_tap(s),generation,k7_voice_session_input_epoch(s),k7_voice_session_anchor_epoch(s),tx_count,stage_observed,p->us(p->ctx),0);
    if(stage_observed){
#else
    if (observe(&fifo, tx_count)) {
#endif
      k7_voice_session_fifo_revoke(s,K7_FIFO_REF_COUNT);
      r->tx_provenance_errors++;
      if (tx == OFF || tx == EOF_TAIL) { rc = -EIO; goto done; }
      turn_rc = -EIO;
    }
    enum k7_voice_tx_action action = k7_voice_session_tx_action(s);
    /* Cancellation must also interrupt EOF drain/clear/tail, not just PLAYING
     * or a software refill. Never leave a held amplifier on until its timer. */
    if (tx != OFF && action == K7_VOICE_TX_CANCEL) {
      if (!turn_rc) turn_rc = -ECANCELED;
      if (amp_enabled) { rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_NORMAL(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false; }
    }
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE && defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
    /* Cancellation while refilling can observe an odd number of physical
     * filler words. Once muted, drain only a FIFO proven entirely explicit
     * zero, with RX/clocks still serviced, before taking the stereo cut.
     * Source/unknown words keep the original immediate-stop policy. */
    if (tx == ZERO_DRAIN) {
      if (tx_fault) { rc = -EPIPE; r->tx_underflows++; goto done; }
      if (now < zero_stop_began || now - zero_stop_began >= 1000u) { rc = -ETIMEDOUT; goto done; }
      if (!tx_count) tx = PAUSED;
    }
#endif
    if (tx == PAUSED && (turn_rc || tx_fault || action == K7_VOICE_TX_CANCEL || now < pause_began ||
        now - pause_began >= (uint64_t)k7_voice_session_tx_pause_timeout_ms(s) * 1000u)) {
      if (tx_fault) { turn_rc = -EPIPE; r->tx_underflows++; }
      else if (!turn_rc) turn_rc = action == K7_VOICE_TX_CANCEL ? -ECANCELED : -ETIMEDOUT;
      rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_NORMAL(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false;
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE && defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
      if (!tx_fault && turn_rc == -ECANCELED && tx_count && !source_words(&fifo)) {
        zero_stop_began = now; tx = ZERO_DRAIN;
      } else
#endif
      {
      uint64_t before = anchors ? p->us(p->ctx) : 0;
      #if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
k7_apm21_tap_stop_pending(k7_voice_session_apm21_tap(s),turn_rc);
#else
k7_apm21_tap_gap(k7_voice_session_apm21_tap(s),-EPIPE);
#endif
#endif
      if (mod(p, SAI_XFER, SAI_XFER_TXS_MASK, 0)) { rc = -EIO; goto done; }
      if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_STOP, generation, queued, 0, turn_rc, before);
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
      if(anchors&&!amp_enabled){
       uint64_t db=p->us(p->ctx);uint32_t dx=0;int dr=rd(p,SAI_XFER,&dx);uint64_t da=p->us(p->ctx);
       k7_voice_session_apm21_idle_disabled(s,generation,r->captured,db,da,dr,dx);
      }
#endif
      tx = STOP_IDLE; stop_began = now;
      }
    }
    if (tx == EOF_TAIL && (turn_rc || now < tail_began || now - tail_began >= STREAM_TAIL_US)) {
      if (tx_count) { rc = -EIO; goto done; }
      if (!turn_rc && now < tail_began) turn_rc = -ETIMEDOUT;
      rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_NORMAL(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
      /* EOF muted only now, after the genuine drain/clear/tail. Take a fresh
       * TXS-disabled observation after this AMP_OFF, not the earlier stop. */
      if(anchors){uint64_t db=p->us(p->ctx);uint32_t dx=0;
       int dr=rd(p,SAI_XFER,&dx);uint64_t da=p->us(p->ctx);
       k7_voice_session_apm21_idle_disabled(s,generation,r->captured,db,da,dr,dx);
       eof_quiet_pending=true;eof_quiet_generation=generation;
      }
#endif
      complete_turn(p, s, r, generation, queued, discarded, turn_rc, reference_drained);
      queued = discarded = 0; generation = 0; tx = OFF; action = K7_VOICE_TX_WAIT;
    }
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
    if(tx==OFF&&action==K7_VOICE_TX_START){
      struct k7_apm21_pio_tap *tap=k7_voice_session_apm21_tap(s);
      int gate=k7_apm21_tap_start_gate(tap,r->captured);
      bool ticket=k7_apm21_tap_round_waiting(tap);
      if(gate<0){rc=gate;goto done;}
      if(ticket){uint32_t next_generation=k7_voice_session_tx_generation(s);
       if(!round_waiting){round_waiting=true;round_wait_began=now;round_wait_mic=r->captured;round_wait_generation=next_generation;}
       if(next_generation!=round_wait_generation||now<round_wait_began||now-round_wait_began>=20000u||
          r->captured<round_wait_mic||r->captured-round_wait_mic>160u){rc=-ETIMEDOUT;goto done;}
       if(!gate||eof_quiet_pending)action=K7_VOICE_TX_WAIT;
       else round_waiting=false;
      }
    }else round_waiting=false;
#endif
    if (tx == OFF && action == K7_VOICE_TX_CANCEL) {
      if (anchors) {
        uint32_t cancelled = k7_voice_session_tx_generation(s);
        anchor(p, s, r, K7_VOICE_ANCHOR_TX_STOP, cancelled, 0, 0, -ECANCELED, UINT64_MAX);
        anchor(p, s, r, K7_VOICE_ANCHOR_TX_COMPLETE, cancelled, 0, 0, -ECANCELED, UINT64_MAX);
      }
      k7_voice_session_tx_complete(s, -ECANCELED, 0, 0); r->cancelled_turns++; r->turns++;
    } else if (tx == OFF && action == K7_VOICE_TX_START) {
      queued = discarded = 0; discard_recorded = false; reference_drained = false;
      stream_policy = k7_voice_session_tx_pause_timeout_ms(s) != 0;
      eof = false; turn_rc = 0;
      generation = k7_voice_session_tx_generation(s);
      if (tx_count) { k7_voice_session_fifo_revoke(s,K7_FIFO_REF_COUNT); rc = -EIO; goto done; }
      k7_voice_session_fifo_begin(s,generation);
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
      k7_apm21_tap_prepare(k7_voice_session_apm21_tap(s),r->captured);
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
      k7_apm21_tap_reference_begin(k7_voice_session_apm21_tap(s),generation,k7_voice_session_input_epoch(s),k7_voice_session_anchor_epoch(s),p->us(p->ctx));
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
      struct k7_fifo_ref_status cut_source={0};int cut_rc=k7_voice_session_fifo_status(s,&cut_source);
      k7_apm21_tap_reference_identity(k7_voice_session_apm21_tap(s),cut_source.instance,cut_source.epoch,cut_rc?cut_rc:cut_source.faults?-EPIPE:0);
#endif
#endif
#endif
      for (unsigned i = 0; i < 8; ++i) {
        int got = enqueue(p, s, &queued, r, generation, &fifo);
        if (got < 0) { turn_rc = got; break; }
        if (!got) { eof = true; break; }
      }
      if (turn_rc || !queued) {
        if (!turn_rc) turn_rc = -ENODATA;
        uint64_t before = anchors ? p->us(p->ctx) : 0;
        rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_SAFE(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false;
        if(anchors)before=p->us(p->ctx);
        #if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
k7_apm21_tap_stop_pending(k7_voice_session_apm21_tap(s),turn_rc);
#else
k7_apm21_tap_gap(k7_voice_session_apm21_tap(s),-EPIPE);
#endif
#endif
        if (mod(p, SAI_XFER, SAI_XFER_TXS_MASK, 0)) { rc = -EIO; goto done; }
        if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_STOP, generation, queued, 0, turn_rc, before);
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
      if(anchors&&!amp_enabled){
       uint64_t db=p->us(p->ctx);uint32_t dx=0;int dr=rd(p,SAI_XFER,&dx);uint64_t da=p->us(p->ctx);
       k7_voice_session_apm21_idle_disabled(s,generation,r->captured,db,da,dr,dx);
      }
#endif
        tx = STOP_IDLE; stop_began = now;
      } else {
        if (rd(p, SAI_TXFIFOLR, &v) || count(v) != 2 * queued) { rc = -EIO; goto done; }
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
        rx_fault.start=(struct rx_start_snapshot){.captured=r->captured,
          .generation=generation,.half=0,.poll_rx=raw,.prefill_tx=v};
#endif
        uint64_t began = anchors ? p->us(p->ctx) : 0;
        if (mod(p, SAI_XFER, SAI_XFER_TXS_MASK, SAI_XFER_TXS_EN)) { rc = -EIO; goto done; }
        uint64_t before = p->us(p->ctx); rc = p->streams_started(p->ctx);
        uint64_t after = p->us(p->ctx);
        if (rc || after < before || after - before >= 100) { rc = rc ? rc : -ETIMEDOUT; goto done; }
        amp_enabled = true; tx_count = 2 * queued;
        rc = k7_voice_session_tx_started(s); if (rc) goto done;
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
        k7_apm21_tap_on(k7_voice_session_apm21_tap(s),r->captured,before,after);
#endif
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
        if (anchors) anchor_observed(p,s,r,K7_VOICE_ANCHOR_TX_START,generation,queued,0,0,began,&rx_fault.start);
#else
        if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_START, generation, queued, 0, 0, began);
#endif
        tx = PLAYING;
      }
    } else if (tx == PAUSED) {
      if (!amp_enabled) { rc = -EIO; goto done; }
      if (tx_count <= 14) {
        if (action == K7_VOICE_TX_START) {
          /* Preserve TXS, clocks and FIFO contents. Source data follows any
           * queued gap zeros in order; there is no transmitter restart. */
          int got = enqueue(p, s, &queued, r, generation, &fifo);
          if (got < 0) turn_rc = got;
          else if (!got) eof = true;
          else {
            rc = k7_voice_session_tx_started(s); if (rc) goto done;
            r->tx_resumes++;
            if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_RESUME, generation, queued, 0, 0, UINT64_MAX);
          }
          tx = PLAYING;
        } else {
          rc = silence(p, s, &fifo, r); if (rc) goto done;
        }
      }
    } else if (tx == PLAYING) {
      if (tx_fault) { turn_rc = -EPIPE; r->tx_underflows++; }
      else if (action == K7_VOICE_TX_CANCEL) turn_rc = -ECANCELED;
      else if (!turn_rc && !eof && tx_count <= 14) {
        int got = enqueue(p, s, &queued, r, generation, &fifo);
        if (got == -EAGAIN && stream_policy) {
          turn_rc = k7_voice_session_tx_draining(s);
          if (!turn_rc) turn_rc = k7_voice_session_tx_paused(s);
          if (!turn_rc) {
            /* Never drain/stop/clear on a refill: that would restart the
             * codec's serial input on resume despite complete source CRCs. */
            rc = silence(p, s, &fifo, r); if (rc) goto done;
            if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_PAUSE, generation, queued, 0, 0, UINT64_MAX);
            r->tx_pauses++; pause_began = now; tx = PAUSED;
          }
        } else if (got < 0) { turn_rc = got; if (got == -EAGAIN) r->tx_underflows++; }
        else if (!got) eof = true;
      }
    }
    if (tx == PLAYING && (turn_rc || (eof && !tx_count))) {
      uint64_t before = anchors ? p->us(p->ctx) : 0;
      /* Normal EOF retains the established tail guard. Cancel/error still
       * mutes before TX stop, without waiting for source or inserted zeros. */
      if (turn_rc || !stream_policy) {
        rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_NORMAL(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false;
      }
      if(anchors)before=p->us(p->ctx);
      #if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
k7_apm21_tap_stop_pending(k7_voice_session_apm21_tap(s),turn_rc);
#else
k7_apm21_tap_gap(k7_voice_session_apm21_tap(s),-EPIPE);
#endif
#endif
      if (mod(p, SAI_XFER, SAI_XFER_TXS_MASK, 0)) { rc = -EIO; goto done; }
      if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_STOP, generation, queued, 0, turn_rc, before);
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
      if(anchors&&!amp_enabled){
       uint64_t db=p->us(p->ctx);uint32_t dx=0;int dr=rd(p,SAI_XFER,&dx);uint64_t da=p->us(p->ctx);
       k7_voice_session_apm21_idle_disabled(s,generation,r->captured,db,da,dr,dx);
      }
#endif
      tx = STOP_IDLE; stop_began = now;
    }
    if (tx == STOP_IDLE || tx == STOP_CLEAR) {
      if (now < stop_began || now - stop_began >= 1000) { rc = -ETIMEDOUT; goto done; }
      if (tx == STOP_IDLE) {
        if (rd(p, SAI_STATUS, &v)) { rc = -EIO; goto done; }
        if (v & SAI_STATUS_TX_IDLE) {
          if (rd(p, SAI_TXFIFOLR, &v)) { rc = -EIO; goto done; }
          unsigned remaining = count(v);
          int observed=observe(&fifo, remaining);
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
          k7_apm21_tap_reference_observe(k7_voice_session_apm21_tap(s),generation,k7_voice_session_input_epoch(s),k7_voice_session_anchor_epoch(s),remaining,observed,p->us(p->ctx),1);
#endif
          reference_drained=!observed&&!remaining;
          if (observed) { k7_voice_session_fifo_revoke(s,K7_FIFO_REF_COUNT); r->tx_provenance_errors++; turn_rc = -EIO; }
          discarded = source_words(&fifo);
          r->tx_discarded_silence_words += fifo.words - discarded;
          discard_recorded = true;
          /* Successful EOF must not discard even one physical word. Error
           * results distinguish source words from explicit gap filler. */
          if ((stream_policy && !turn_rc && remaining) || turn_rc == -EIO) {
            turn_rc = -EIO;
            rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_SAFE(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false;
          }
          if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_CLEAR, generation, queued, discarded, turn_rc, UINT64_MAX);
          k7_voice_session_fifo_revoke(s,K7_FIFO_REF_CLEAR);
          if (mod(p, SAI_CLR, SAI_CLR_TXC, SAI_CLR_TXC)) { rc = -EIO; goto done; }
          tx = STOP_CLEAR;
        }
      } else {
        if (rd(p, SAI_CLR, &v)) { rc = -EIO; goto done; }
        if (!(v & SAI_CLR_TXC)) {
          if (rd(p, SAI_TXFIFOLR, &v) || count(v) ||
              mod(p, SAI_INTCR, SAI_INTCR_TXUIC, SAI_INTCR_TXUIC)) { rc = -EIO; goto done; }
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION && defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
          /* Clear completion can precede the next real RX sample after mute.
           * Keep the canceled turn owned until that successor proves QUIET;
           * SOURCE_END must follow QUIET, never the same-RX OFF boundary. */
          if (anchors && !amp_enabled) tx = STOP_RX;
          else {
#endif
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
          /* This exact STOP_CLEAR path already observed successful TXS-off,
           * TX_IDLE, cleared TXC and empty FIFO. Only a completed mute can
           * certify the canceled-turn digital boundary. Extra read failure
           * revokes these optional events only, never changes audio control. */
          if(anchors&&!amp_enabled){
            uint64_t qb=p->us(p->ctx);uint32_t qx=0;
            int qr=rd(p,SAI_XFER,&qx);uint64_t qa=p->us(p->ctx);
            unsigned qc=!qr&&!(qx&SAI_XFER_TXS_MASK)&&
              (qx&SAI_XFER_RXS_MASK)==SAI_XFER_RXS_EN&&(qx&3u)==3u?K7_APM21_IDLE_QUIET_CHECKS:0;
            k7_voice_session_apm21_idle_quiet(s,generation,r->captured,qb,qa,qc,qx);
          }
#endif
          if (stream_policy && !turn_rc) {
            tail_began = now; tx = EOF_TAIL;
          } else {
            if (amp_enabled) { rc = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_NORMAL(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
; if (rc) goto done; amp_enabled = false; }
            complete_turn(p, s, r, generation, queued, discarded, turn_rc, reference_drained);
            queued = discarded = 0; generation = 0; tx = OFF;
          }
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION && defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
          }
#endif
        }
      }
    }
    if (group_ready(raw)) {
      uint32_t words[8];
      for (unsigned i = 0; i < 8; ++i) if (rd(p, SAI_RXDR, &words[i])) { rc = -EIO; goto done; }
      for (unsigned i = 2; i < 8; i += 2) {
        if (words[i] != words[0]) r->grouped_mismatches++;
        if (words[i + 1] != words[1]) r->grouped_mismatches++;
      }
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
      if (r->grouped_mismatches) {
        /* First failure only. Preserve all original words and errors. These
         * four extra read-only observations occur only after mismatch. */
        if(!rx_fault.valid){
          rx_fault.valid=1;memcpy(rx_fault.words,words,sizeof(words));
          rx_fault.captured=r->captured;rx_fault.poll_rx=raw;rx_fault.half=0;
          rx_fault.tx=(uint32_t)tx;rx_fault.generation=generation;rx_fault.queued=queued;
          if(rd(p,SAI_XFER,&rx_fault.xfer))rx_fault.read_failures|=1u;
          if(rd(p,SAI_STATUS,&rx_fault.status))rx_fault.read_failures|=2u;
          if(rd(p,SAI_RXFIFOLR,&rx_fault.rx_fifo))rx_fault.read_failures|=4u;
          if(rd(p,SAI_TXFIFOLR,&rx_fault.tx_fifo))rx_fault.read_failures|=8u;
        }
        rc = -EILSEQ; goto done;
      }
#else
      if (r->grouped_mismatches) { rc = -EILSEQ; goto done; }
#endif
      last_rx = now;
      /* This complete 8-word cycle contains four copies of one stereo
       * frame. Select its actual left slot once: two such cycles would
       * discard every other 16 kHz frame, not complete a missing half. */
      {
        rc = k7_voice_session_microphone(s, (int16_t)(words[0] >> 16), r->captured);
        if (rc) goto done;
        r->captured++;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION && defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
        if(tx == STOP_RX){
          /* Genuine microphone successor, muted amplifier and fresh physical
           * TXS-off/empty/idle/clear observations. No fabricated RX or PCM. */
          uint64_t qb=p->us(p->ctx);uint32_t qx=0,qf=0,qs=0,qc=0;
          int qr=rd(p,SAI_XFER,&qx)||rd(p,SAI_TXFIFOLR,&qf)||rd(p,SAI_STATUS,&qs)||rd(p,SAI_CLR,&qc);
          uint64_t qa=p->us(p->ctx);
          if(qr||amp_enabled||(qx&SAI_XFER_TXS_MASK)||(qx&SAI_XFER_RXS_MASK)!=SAI_XFER_RXS_EN||
             (qx&3u)!=3u||count(qf)||!(qs&SAI_STATUS_TX_IDLE)||(qc&SAI_CLR_TXC)) { rc=-EIO;goto done; }
          k7_voice_session_apm21_idle_quiet(s,generation,r->captured,qb,qa,K7_APM21_IDLE_QUIET_CHECKS,qx);
          complete_turn(p,s,r,generation,queued,discarded,turn_rc,reference_drained);
          queued=discarded=0;generation=0;tx=OFF;
        }
#endif
        if(eof_quiet_pending){
          /* One genuine RX successor after mute; independently reread the
           * stopped TX/empty FIFO/clear status. This is digital idle only. */
          uint64_t qb=p->us(p->ctx);uint32_t qx=0,qf=0,qs=0,qc=0;
          int qr=rd(p,SAI_XFER,&qx)||rd(p,SAI_TXFIFOLR,&qf)||rd(p,SAI_STATUS,&qs)||rd(p,SAI_CLR,&qc);
          uint64_t qa=p->us(p->ctx);
          unsigned checks=!qr&&!(qx&SAI_XFER_TXS_MASK)&&(qx&SAI_XFER_RXS_MASK)==SAI_XFER_RXS_EN&&
            (qx&3u)==3u&&!count(qf)&&(qs&SAI_STATUS_TX_IDLE)&&!(qc&SAI_CLR_TXC)?K7_APM21_IDLE_QUIET_CHECKS:0;
          k7_voice_session_apm21_idle_quiet(s,eof_quiet_generation,r->captured,qb,qa,checks,qx);
          eof_quiet_pending=false;
        }
#endif
        if (anchors && !(r->captured % K7_VOICE_FRAME_SAMPLES))
          anchor(p, s, r, K7_VOICE_ANCHOR_RX_FRAME, generation, queued, discarded, 0, UINT64_MAX);
      }
    }
  }
done:
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
  k7_apm21_tap_gap(k7_voice_session_apm21_tap(s),rc?rc:-EPIPE);
#endif
  k7_voice_session_fifo_revoke(s,K7_FIFO_REF_STOP|(rc&&rc!=-ECANCELED?K7_FIFO_REF_IO:0));
  k7_voice_session_ready(s, false);
  uint64_t cleanup_began = anchors ? p->us(p->ctx) : 0;
  r->amp_off_result = 
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
MUTE_SAFE(p,s,r,generation,queued,discarded)
#else
mute(p,s,r,generation,queued,discarded)
#endif
;
  if(anchors)cleanup_began=p->us(p->ctx);
  r->stop_result = stop_all(p, &fifo, &discarded, r, discard_recorded);
  if (anchors) {
    if (!generation) {
      struct k7_voice_session_status status; k7_voice_session_status(s, &status);
      if (status.tx_state != K7_VOICE_TX_IDLE) generation = status.tx_generation;
    }
    int cleanup = r->amp_off_result ? r->amp_off_result : r->stop_result ? r->stop_result : rc;
    anchor(p, s, r, K7_VOICE_ANCHOR_SESSION_STOP, generation, queued, discarded, cleanup, cleanup_began);
  }
  /* A reference may already contain prefill pairs while TX is still OFF. */
  k7_voice_session_fifo_end(s,rc?rc:-ECANCELED,discarded,false);
  if (tx != OFF) {
    k7_voice_session_tx_complete(s, rc ? rc : -ECANCELED, queued, discarded);
    r->turns++; r->queued += queued;
  }
  if (!r->amp_off_result && !r->stop_result) r->held = 0;
  /* Do not hide a failed cleanup behind an otherwise expected stop request. */
  r->result = r->amp_off_result ? r->amp_off_result : r->stop_result ? r->stop_result : rc;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
  /* Logging follows the original mute/stop/clear and owner completion path. */
  if(rx_fault.valid)printf("RX_GROUP_FIRSTFAULT captured=%llu half=%u tx=%u generation=%u queued=%u poll_rx=%08x xfer=%08x status=%08x rx_fifo=%08x tx_fifo=%08x read_failures=%u words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x start_valid=%u start_captured=%llu start_half=%u start_generation=%u start_poll_rx=%08x start_prefill_tx=%08x start_anchor_rx=%08x start_anchor_tx=%08x start_anchor_flags=%u start_before_us=%llu start_after_us=%llu amp_off=%d stop=%d held=%d result=%d\n",
    (unsigned long long)rx_fault.captured,rx_fault.half,rx_fault.tx,rx_fault.generation,rx_fault.queued,
    rx_fault.poll_rx,rx_fault.xfer,rx_fault.status,rx_fault.rx_fifo,rx_fault.tx_fifo,rx_fault.read_failures,
    rx_fault.words[0],rx_fault.words[1],rx_fault.words[2],rx_fault.words[3],rx_fault.words[4],rx_fault.words[5],rx_fault.words[6],rx_fault.words[7],
    rx_fault.start.valid,(unsigned long long)rx_fault.start.captured,rx_fault.start.half,rx_fault.start.generation,
    rx_fault.start.poll_rx,rx_fault.start.prefill_tx,rx_fault.start.anchor_rx,rx_fault.start.anchor_tx,rx_fault.start.anchor_flags,
    (unsigned long long)rx_fault.start.before_us,(unsigned long long)rx_fault.start.after_us,r->amp_off_result,r->stop_result,r->held,r->result);
#endif
  return r->result;
}
