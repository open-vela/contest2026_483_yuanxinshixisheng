/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "session_ab_probe.h"
#include <errno.h>
#include <string.h>
static uint32_t crc_byte(uint32_t crc, unsigned char value)
{
  crc ^= value;
  for (unsigned i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  return crc;
}
static uint32_t crc_sample(uint32_t crc, int16_t value)
{ return crc_byte(crc_byte(crc, (unsigned char)value), (unsigned char)((uint16_t)value >> 8)); }
uint32_t k7_session_ab_crc(const unsigned char *data, unsigned bytes)
{
  uint32_t crc = UINT32_MAX;
  for (unsigned i = 0; i < bytes; i++) crc = crc_byte(crc, data[i]);
  return ~crc;
}
static int16_t sample(const struct k7_ab_config *c, unsigned at)
{ return (int16_t)((uint16_t)c->pcm[at * 2] | ((uint16_t)c->pcm[at * 2 + 1] << 8)); }
static int drain(struct k7_voice_session *s, const struct k7_ab_config *c,
                 int16_t *rx, unsigned cap, struct k7_ab_result *r)
{
  struct k7_voice_frame f;
  int got;
  while ((got = k7_voice_session_take_kws(s, &f)) == 1) {
    if (!r->rx_samples) r->rx_first = f.first_sample;
    if (f.epoch != 1 || f.first_sample != r->rx_first + r->rx_samples) {
      r->rx_gaps++; return -EILSEQ;
    }
    if (f.samples > cap - r->rx_samples) { r->capture_truncated = true; return -ENOSPC; }
    for (unsigned i = 0; i < f.samples; i++) {
      rx[r->rx_samples++] = f.pcm[i]; r->rx_crc = crc_sample(r->rx_crc, f.pcm[i]);
    }
  }
  if (got) { r->rx_gaps++; return got; }
  while ((got = k7_voice_session_take_render(s, &f)) == 1) {
    if (f.epoch < 1 || f.epoch > 2) return -EILSEQ;
    struct k7_ab_pass *p = &r->pass[f.epoch - 1];
    if (f.first_sample != p->rendered || f.samples > c->samples - p->rendered) return -EILSEQ;
    for (unsigned i = 0; i < f.samples; i++) {
      if (f.pcm[i] != sample(c, p->rendered)) return -EILSEQ;
      p->crc = crc_sample(p->crc, f.pcm[i]); p->rendered++;
    }
  }
  if (got) return got;
  struct k7_voice_anchor a;
  for (;;) {
    got = k7_voice_session_take_anchor(s, &a);
    if (got == -EPIPE) { r->event_gaps++; continue; }
    if (got <= 0) break;
    if (a.flags & (K7_VOICE_ANCHOR_IO_ERROR | K7_VOICE_ANCHOR_CLOCK_INVALID | K7_VOICE_ANCHOR_COUNT_INVALID)) return -EIO;
    if (a.event != K7_VOICE_ANCHOR_RX_FRAME && a.event != K7_VOICE_ANCHOR_TX_FRAME) {
      if (r->events >= K7_AB_EVENTS) return -ENOSPC;
      r->event[r->events++] = a;
    }
  }
  return got;
}
int k7_session_ab_run(struct k7_voice_session *s, const struct k7_ab_config *c,
                      const struct k7_ab_io *io, int16_t *rx, unsigned cap,
                      struct k7_ab_result *r)
{
  if (!r) return -EINVAL;
  memset(r, 0, sizeof(*r));
  if (!s || !c || !c->pcm || !rx || !cap || !io || !io->ms || !io->yield || !io->cancelled ||
      !c->samples || c->samples > 160000 || !c->cut[0] || c->cut[0] >= c->cut[1] ||
      c->cut[1] >= c->samples || !c->gap_ms || c->gap_ms > 1500 || !c->timeout_ms || c->timeout_ms > 30000)
    return r->result = -EINVAL;
  r->source_crc = k7_session_ab_crc(c->pcm, c->samples * 2);
  r->rx_crc = r->pass[0].crc = r->pass[1].crc = UINT32_MAX;
  uint64_t began = io->ms(io->arg), gap_began = 0, next = began, done_at = 0;
  if (began == UINT64_MAX) return r->result = -ETIMEDOUT;
  unsigned pass = 0, cut = 0; bool active = false, ended = false, waiting = false;
  int rc = 0;
  for (;;) {
    uint64_t now = io->ms(io->arg);
    if (io->cancelled(io->arg)) { rc = -ECANCELED; break; }
    if (now == UINT64_MAX || now < began || now - began >= c->timeout_ms) { rc = -ETIMEDOUT; break; }
    struct k7_voice_session_status status; k7_voice_session_status(s, &status);
    if (status.finished || !status.ready || !status.running || status.stop_requested) {
      rc = status.result ? status.result : -ENOTCONN; break;
    }
    if (status.input_epoch != 1 || status.kws_overflows || status.render_overflows || status.anchor_overflows) {
      rc = -EOVERFLOW; break;
    }
    rc = drain(s, c, rx, cap, r); if (rc) break;
    if (pass == 2) {
      if (now >= done_at && now - done_at >= 200) { r->capture_complete = true; break; }
      io->yield(io->arg); continue;
    }
    struct k7_ab_pass *p = &r->pass[pass];
    if (!active && now >= next) {
      rc = k7_voice_session_tx_begin_stream(s, pass + 1, c->prebuffer, c->rebuffer, 5000);
      if (rc) break;
      p->start_rx = status.microphone_samples; active = true; ended = false; cut = 0;
    }
    if (active) {
      if (pass == 1 && cut < 2 && p->submitted == c->cut[cut]) {
        if (!waiting && status.tx_state == K7_VOICE_TX_PAUSED && status.tx_pauses == cut + 1) {
          waiting = true; gap_began = now;
        }
        if (waiting && now >= gap_began && now - gap_began >= c->gap_ms) { cut++; waiting = false; }
      }
      unsigned limit = pass == 1 && cut < 2 ? c->cut[cut] : c->samples;
      /* RAM source only. Keep filling until queue pressure; never block RX or
       * consume another caller's queues. B's only deliberate gaps are cuts. */
      while (p->submitted < limit) {
        int16_t pcm[320]; unsigned count = limit - p->submitted;
        if (count > 320) count = 320;
        for (unsigned i = 0; i < count; i++) pcm[i] = sample(c, p->submitted + i);
        rc = k7_voice_session_tx_write(s, pass + 1, pcm, count);
        if (rc == -EAGAIN) { rc = 0; break; }
        if (rc) break;
        p->submitted += count;
      }
      if (rc) break;
      if (p->submitted == c->samples && !ended) {
        rc = k7_voice_session_tx_end(s, pass + 1); if (rc) break; ended = true;
      }
      struct k7_voice_tx_result out;
      int got = k7_voice_session_tx_result(s, pass + 1, &out);
      if (!got) {
        p->tx_result = out.result; p->queued = out.queued_samples;
        p->discarded = out.discarded_words; p->pauses = out.pauses;
        p->end_rx = status.microphone_samples;
        rc = drain(s, c, rx, cap, r); if (rc) break;
        if (out.result || !ended || out.discarded_words || out.queued_samples != c->samples ||
            p->rendered != c->samples || ~p->crc != r->source_crc || out.pauses != (pass ? 2u : 0u)) {
          rc = out.result ? out.result : -EILSEQ; break;
        }
        pass++; active = false; next = now + 300; done_at = now;
      } else if (got != -EAGAIN) { rc = got; break; }
    }
    io->yield(io->arg);
  }
  if (rc && active) (void)k7_voice_session_tx_cancel(s, pass + 1);
  r->rx_crc = ~r->rx_crc;
  r->pass[0].crc = ~r->pass[0].crc; r->pass[1].crc = ~r->pass[1].crc;
  r->result = rc; return rc;
}
