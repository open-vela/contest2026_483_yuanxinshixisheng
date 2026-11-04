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
static int group_ready(uint32_t v)
{ return (v & 63u) && ((v >> 6) & 63u) && ((v >> 12) & 63u) && ((v >> 18) & 63u); }
/* Optional diagnostics only. The same monotonic microsecond source brackets
 * sequential FIFO reads and, for start/stop, the corresponding operation.
 * No presumed DATA_CNT meaning, DAC timestamp or acoustic-delay estimate. */
static void anchor(const struct pio_port *p, struct k7_voice_session *s,
                   const struct k7_duplex_session_result *r, unsigned event,
                   uint32_t generation, unsigned queued, unsigned discarded,
                   int result, uint64_t before)
{
  if (!k7_voice_session_anchors_enabled(s)) return;
  struct k7_voice_anchor a = {.before_us = before == UINT64_MAX ? p->us(p->ctx) : before,
    .rx_samples = r->captured, .generation = generation, .tx_queued_samples = queued,
    .discarded_words = discarded, .event = event, .result = result};
  uint32_t rx = 0, tx = 0;
  if (rd(p, SAI_RXFIFOLR, &rx)) a.flags |= K7_VOICE_ANCHOR_IO_ERROR;
  if (rd(p, SAI_TXFIFOLR, &tx)) a.flags |= K7_VOICE_ANCHOR_IO_ERROR;
  a.after_us = p->us(p->ctx); a.rx_fifo_words = count(rx); a.tx_fifo_words = count(tx);
  (void)k7_voice_session_record_anchor(s, &a); /* loss never blocks the pump */
}
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
  uint32_t v, raw, left = 0; unsigned half = 0;
  enum { OFF, PLAYING, STOP_IDLE, STOP_CLEAR, PAUSED, EOF_TAIL } tx = OFF;
  unsigned queued = 0, discarded = 0; bool eof = false;
  struct fifo_tags fifo = {0};
  bool stream_policy = false, amp_enabled = false, discard_recorded = false;
  bool reference_drained = false;
  const bool anchors = k7_voice_session_anchors_enabled(s);
  uint32_t generation = 0;
  int turn_rc = 0, rc = -EIO;
  uint64_t previous, last_rx, stop_began = 0, pause_began = 0, tail_began = 0;
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
      complete_turn(p, s, r, generation, queued, discarded, turn_rc, reference_drained);
      queued = discarded = 0; generation = 0; tx = OFF; action = K7_VOICE_TX_WAIT;
    }
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
        if (anchors) anchor(p, s, r, K7_VOICE_ANCHOR_TX_START, generation, queued, 0, 0, began);
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
        }
      }
    }
    if (group_ready(raw)) {
      uint32_t words[8];
      for (unsigned i = 0; i < 8; ++i) if (rd(p, SAI_RXDR, &words[i])) { rc = -EIO; goto done; }
      for (unsigned i = 2; i < 8; i += 2) if (words[i] != words[0]) r->grouped_mismatches++;
      if (r->grouped_mismatches) { rc = -EILSEQ; goto done; }
      last_rx = now;
      if (!half) { left = words[0]; half = 1; }
      else {
        half = 0;
        rc = k7_voice_session_microphone(s, (int16_t)(left >> 16), r->captured);
        if (rc) goto done;
        r->captured++;
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
  return r->result;
}
