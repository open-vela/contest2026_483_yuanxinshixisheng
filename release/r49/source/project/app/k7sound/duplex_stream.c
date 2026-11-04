/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "duplex_stream.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>
#define BIT(n) (UINT32_C(1) << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#include "input/rockchip_sai.h"

static int rd(const struct pio_port *p, uint32_t o, uint32_t *v)
{ return p->read(p->ctx, o, v) ? -EIO : 0; }
static int wr(const struct pio_port *p, uint32_t o, uint32_t v)
{ return p->write(p->ctx, o, v) ? -EIO : 0; }
static int mod(const struct pio_port *p, uint32_t o, uint32_t mask, uint32_t v)
{ uint32_t old; return rd(p, o, &old) ? -EIO : wr(p, o, (old & ~mask) | (v & mask)); }
static unsigned count(uint32_t v)
{ return (v & 63u) + ((v >> 6) & 63u) + ((v >> 12) & 63u) + ((v >> 18) & 63u); }
static int ready_group(uint32_t v)
{ return (v & 63u) && ((v >> 6) & 63u) && ((v >> 12) & 63u) && ((v >> 18) & 63u); }
static int amp_low(const struct pio_port *p)
{
  uint64_t before = p->us(p->ctx);
  int rc = p->amp_off_fast(p->ctx);
  uint64_t after = p->us(p->ctx);
  return rc ? -EIO : after < before || after - before >= 100 ? -ETIMEDOUT : 0;
}
static int wait_bits(const struct pio_port *p, uint32_t reg, uint32_t mask,
                      uint32_t expected, uint64_t start, unsigned budget)
{
  for (unsigned i = 0; i < 10000; ++i) {
    uint32_t v; if (rd(p, reg, &v)) return -EIO;
    uint64_t now = p->us(p->ctx);
    if (now < start || now - start >= budget) return -ETIMEDOUT;
    if ((v & mask) == expected) return 0;
  }
  return -ETIMEDOUT;
}
/* Called only at final exit, after RX is no longer required. Never used for
 * ordinary TX cancellation, which is advanced while servicing RX below. */
static int stop_both(const struct pio_port *p, unsigned *discarded)
{
  uint64_t began = p->us(p->ctx); uint32_t v;
  int rc = mod(p, SAI_XFER, SAI_XFER_TXS_MASK | SAI_XFER_RXS_MASK, 0);
  if (!rc) rc = wait_bits(p, SAI_STATUS, SAI_STATUS_TX_IDLE | SAI_STATUS_RX_IDLE,
                          SAI_STATUS_TX_IDLE | SAI_STATUS_RX_IDLE, began, 1000);
  if (!rc) {
    rc = rd(p, SAI_TXFIFOLR, &v);
    if (!rc) *discarded += count(v);
  }
  if (!rc) rc = mod(p, SAI_CLR, SAI_CLR_TXC | SAI_CLR_RXC, SAI_CLR_TXC | SAI_CLR_RXC);
  if (!rc) rc = wait_bits(p, SAI_CLR, SAI_CLR_TXC | SAI_CLR_RXC, 0, began, 1000);
  if (!rc) rc = mod(p, SAI_XFER, SAI_XFER_CLK_MASK | SAI_XFER_FSS_MASK, 0);
  if (!rc) rc = wait_bits(p, SAI_STATUS, SAI_STATUS_FS_IDLE, SAI_STATUS_FS_IDLE, began, 2000);
  if (!rc && (rd(p, SAI_XFER, &v) || (v & 15u))) rc = -EIO;
  if (!rc && (rd(p, SAI_TXFIFOLR, &v) || count(v))) rc = -EIO;
  if (!rc && (rd(p, SAI_RXFIFOLR, &v) || count(v))) rc = -EIO;
  return rc;
}
static int enqueue(const struct pio_port *p, const struct k7_duplex_stream_config *c,
                     struct k7_duplex_stream_result *r)
{
  int16_t sample; int got = c->playback(c->playback_arg, &sample);
  if (got < 0) return got;
  if (got > 1) return -EPROTO;
  if (!got) { r->tx_eof = 1; return 0; }
  uint32_t word = (uint32_t)((int32_t)sample * 65536);
  if (wr(p, SAI_TXDR, word) || wr(p, SAI_TXDR, word)) return -EIO;
  unsigned index = r->tx_queued++;
  if (c->render) {
    int rc = c->render(c->render_arg, sample, index);
    if (rc) return rc < 0 ? rc : -EPROTO;
  }
  return 1;
}
int pio_duplex_stream16(const struct pio_port *p, int prepared, uint32_t mclk,
                        const struct k7_duplex_stream_config *c,
                        struct k7_duplex_stream_result *r)
{
  if (!r) return -EINVAL;
  memset(r, 0, sizeof(*r));
  if (!p || !c || !p->read || !p->write || !p->us || !p->clocks_started ||
      !p->streams_started || !p->amp_off_fast || !c->microphone ||
      !c->playback || !c->stop_all || c->max_capture_frames < 320 ||
      c->max_capture_frames > K7_DUPLEX_MAX_CAPTURE_FRAMES || c->max_capture_frames % 320)
    return r->result = -EINVAL;
  if (!prepared || mclk != 4096000u) return r->result = -ENOTSUP;
  if (c->stop_all(c->stop_arg)) return r->result = -ECANCELED;
  uint32_t v, raw, tx_count, left = 0;
  unsigned half = 0, tx_state = 0; /* 0=running,1=idle-wait,2=clear-wait,3=off */
  uint64_t previous, tx_stop_began = 0, last_rx;
  int rc = -EIO;
  /* A refused/unknown initial state is not proof that clocks may be gated. */
  r->held = 1;
  if (rd(p, SAI_VERSION, &v)) return r->result = -EIO;
  if (v != SAI_VER_2307) return r->result = -ENOTSUP;
  if (rd(p, SAI_XFER, &v) || (v & 15u) || rd(p, SAI_STATUS, &v) || (v & 14u) != 14u ||
      rd(p, SAI_DMACR, &v) || (v & (SAI_DMACR_RDE_MASK | SAI_DMACR_TDE_MASK)) ||
      rd(p, SAI_MONO_CR, &v) || v || rd(p, SAI_TXFIFOLR, &v) || count(v) ||
      rd(p, SAI_RXFIFOLR, &v) || count(v)) return r->result = -EBUSY;
  if (rd(p, SAI_PATH_SEL, &v)) return r->result = -EIO;
  if (v & 0x30000u) return r->result = -ENOTSUP; /* no internal loopback */
  const uint32_t mask = SAI_XCR_START_SEL_MASK | SAI_XCR_EDGE_SHIFT_MASK |
    SAI_XCR_CSR_MASK | SAI_XCR_SJM_MASK | SAI_XCR_FBM_MASK | SAI_XCR_SNB_MASK |
    SAI_XCR_VDJ_MASK | SAI_XCR_SBW_MASK | SAI_XCR_VDW_MASK;
  const uint32_t common = SAI_XCR_EDGE_SHIFT_1 | SAI_XCR_SNB(2) |
    SAI_XCR_VDJ_L | SAI_XCR_SBW(32) | SAI_XCR_VDW(32);
  if (mod(p, SAI_TXCR, mask, common | SAI_XCR_CSR(1)) ||
      mod(p, SAI_RXCR, mask, common | SAI_XCR_CSR(4)) ||
      mod(p, SAI_TX_SHIFT, SAI_XSHIFT_RIGHT_MASK, 2) ||
      mod(p, SAI_RX_SHIFT, SAI_XSHIFT_RIGHT_MASK, 2) ||
      mod(p, SAI_FSCR, SAI_FSCR_EDGE_MASK | SAI_FSCR_FW_MASK | SAI_FSCR_FPW_MASK,
          SAI_FSCR_EDGE_DUAL | SAI_FSCR_FW(64) | SAI_FSCR_FPW(32)) ||
      mod(p, SAI_CKR, SAI_CKR_MDIV_MASK | SAI_CKR_MSS_MASK | SAI_CKR_CKP_MASK | SAI_CKR_FSP_MASK,
          SAI_CKR_MDIV(4)) || mod(p, SAI_PATH_SEL, 0xff03u, 0) ||
      mod(p, SAI_INTCR, SAI_INTCR_TXUIE_MASK | SAI_INTCR_RXOIE_MASK, 0) ||
      mod(p, SAI_INTCR, SAI_INTCR_TXUIC | SAI_INTCR_RXOIC,
          SAI_INTCR_TXUIC | SAI_INTCR_RXOIC)) goto done;
  if (mod(p, SAI_XFER, SAI_XFER_CLK_MASK | SAI_XFER_FSS_MASK,
          SAI_XFER_CLK_EN | SAI_XFER_FSS_EN)) goto done;
  previous = p->us(p->ctx);
  rc = p->clocks_started(p->ctx);
  v = 0;
  uint64_t now = p->us(p->ctx);
  if (rc || now < previous || now - previous >= 100000) {
    rc = rc ? rc : -ETIMEDOUT; goto done;
  }
  if (c->stop_all(c->stop_arg)) { rc = -ECANCELED; goto done; }
  r->tx_cancelled = c->cancel_tx && c->cancel_tx(c->cancel_tx_arg);
  if (!r->tx_cancelled) {
    for (unsigned i = 0; i < 8; ++i) {
      rc = enqueue(p, c, r); if (rc <= 0) break;
    }
    if (rc < 0) goto done;
    if (!r->tx_queued) { rc = -ENODATA; goto done; }
  }
  if (rd(p, SAI_TXFIFOLR, &v) || count(v) != 2 * r->tx_queued) { rc = -EIO; goto done; }
  r->max_tx_fifo = count(v);
  if (mod(p, SAI_XFER, SAI_XFER_TXS_MASK | SAI_XFER_RXS_MASK,
          SAI_XFER_RXS_EN | (r->tx_queued ? SAI_XFER_TXS_EN : 0))) { rc = -EIO; goto done; }
  if (r->tx_queued) {
    previous = p->us(p->ctx); rc = p->streams_started(p->ctx); now = p->us(p->ctx);
    if (rc || now < previous || now - previous >= 100) { rc = rc ? rc : -ETIMEDOUT; goto done; }
  } else tx_state = 3;
  r->start_us = previous = last_rx = p->us(p->ctx);
  if (tx_state == 3) r->tx_stop_us = r->start_us;
  const uint64_t budget = (uint64_t)c->max_capture_frames * 1000000 / 16000 + 100000;
  for (r->polls = 0; r->polls < c->max_capture_frames * 4096u; ++r->polls) {
    now = p->us(p->ctx);
    if (now < previous || now - r->start_us >= budget || now - last_rx >= 100000) { rc = -ETIMEDOUT; goto done; }
    previous = now;
    if (c->stop_all(c->stop_arg)) { rc = -ECANCELED; goto done; }
    if (rd(p, SAI_INTSR, &v)) { rc = -EIO; goto done; }
    if (v & (SAI_INTSR_RXOI_ACT | SAI_INTSR_TXUI_ACT)) {
      r->rx_overflows += !!(v & SAI_INTSR_RXOI_ACT);
      r->tx_underflows += !!(v & SAI_INTSR_TXUI_ACT);
      rc = -EOVERFLOW; goto done;
    }
    if (rd(p, SAI_RXFIFOLR, &raw) || rd(p, SAI_TXFIFOLR, &v)) { rc = -EIO; goto done; }
    tx_count = count(v); unsigned rx_count = count(raw);
    if (tx_count > r->max_tx_fifo) r->max_tx_fifo = tx_count;
    if (rx_count > r->max_rx_fifo) r->max_rx_fifo = rx_count;
    if (tx_count > 16 || rx_count > 128) { rc = -EPROTO; goto done; }
    if (tx_state == 0) {
      if (c->cancel_tx && c->cancel_tx(c->cancel_tx_arg)) r->tx_cancelled = 1;
      if (r->tx_cancelled || (r->tx_eof && !tx_count)) {
        rc = amp_low(p); if (rc) goto done;
        if (mod(p, SAI_XFER, SAI_XFER_TXS_MASK, 0)) { rc = -EIO; goto done; }
        tx_state = 1; tx_stop_began = now;
      } else if (!r->tx_eof && tx_count <= 14) {
        rc = enqueue(p, c, r); if (rc < 0) goto done;
      }
    }
    if (tx_state == 1 || tx_state == 2) {
      if (now < tx_stop_began || now - tx_stop_began >= 1000) { rc = -ETIMEDOUT; goto done; }
      if (tx_state == 1) {
        if (rd(p, SAI_STATUS, &v)) { rc = -EIO; goto done; }
        if (v & SAI_STATUS_TX_IDLE) {
          if (rd(p, SAI_TXFIFOLR, &v)) { rc = -EIO; goto done; }
          r->tx_discarded_words = count(v);
          if (mod(p, SAI_CLR, SAI_CLR_TXC, SAI_CLR_TXC)) { rc = -EIO; goto done; }
          tx_state = 2;
        }
      } else {
        if (rd(p, SAI_CLR, &v)) { rc = -EIO; goto done; }
        if (!(v & SAI_CLR_TXC)) {
          if (rd(p, SAI_TXFIFOLR, &v) || count(v)) { rc = -EIO; goto done; }
          tx_state = 3; r->tx_stop_us = now;
        }
      }
    }
    /* Service real RX during TX drain/idle/clear. Two complete hardware
     * eight-word cycles form one stereo sample, as in the accepted RX path. */
    if (ready_group(raw)) {
      uint32_t words[8];
      for (unsigned i = 0; i < 8; ++i) if (rd(p, SAI_RXDR, &words[i])) { rc = -EIO; goto done; }
      r->grouped_words += 8;
      for (unsigned i = 2; i < 8; i += 2) if (words[i] != words[0]) r->grouped_mismatches++;
      if (r->grouped_mismatches) { rc = -EILSEQ; goto done; }
      last_rx = now;
      if (!half) { left = words[0]; half = 1; }
      else {
        half = 0;
        rc = c->microphone(c->microphone_arg, (int16_t)(left >> 16), r->captured);
        if (rc) { if (rc > 0) rc = -EPROTO; goto done; }
        r->captured++; if (tx_state == 3) r->rx_after_tx_stop++;
        if (r->captured == c->max_capture_frames) { rc = 0; goto done; }
      }
    }
  }
  rc = -ETIMEDOUT;
done:
  r->end_us = p->us(p->ctx);
  r->amp_off_result = amp_low(p);
  r->stop_result = stop_both(p, &r->tx_discarded_words);
  if (!r->stop_result && !r->amp_off_result) r->held = 0;
  r->result = rc ? rc : r->amp_off_result ? r->amp_off_result : r->stop_result;
  return r->result;
}
