/* SPDX-License-Identifier: GPL-2.0-or-later
 * (C) Copyright 2008-2017 Fuzhou Rockchip Electronics Co., Ltd
 * Author: Guochun Huang <hero.huang@rock-chips.com>
 * Fixed-mode adaptation of U-Boot dw_mipi_dsi2.c. See DSI2_SOURCE.md.
 */
#include "rk3576_dsi2.h"
#include <errno.h>
#include <string.h>

static int wr(struct k7_dsi2 *d, uint32_t r, uint32_t v)
{ return d->io.write(d->io.arg, K7_DSI2, r, v); }
static int update(struct k7_dsi2 *d, uint32_t r, uint32_t mask, uint32_t v)
{
  uint32_t old; int ret = d->io.read(d->io.arg, K7_DSI2, r, &old);
  return ret < 0 ? ret : wr(d, r, (old & ~mask) | (v & mask));
}
static int poll(struct k7_dsi2 *d, uint32_t r, uint32_t mask,
                uint32_t expected, unsigned limit)
{
  unsigned elapsed; uint32_t v = 0; int ret;
  for (elapsed = 0; ; elapsed += 10) {
    ret = d->io.read(d->io.arg, K7_DSI2, r, &v);
    if (ret < 0) break;
    if ((v & mask) == expected) return 0;
    if (elapsed >= limit) { ret = -ETIMEDOUT; break; }
    ret = d->io.delay_us(d->io.arg, 10);
    if (ret < 0) break;
  }
  if (!d->failed_poll_error) {
    d->failed_poll_reg = r; d->failed_poll_value = v;
    d->failed_poll_error = ret;
  }
  return ret;
}
static uint32_t rounded(uint64_t num, uint32_t den)
{ return (uint32_t)((num + den / 2) / den); }
static int tx_health(struct k7_dsi2 *d)
{
  static const uint32_t offsets[] = {0x400, 0x410, 0x420, 0x440, 0x460};
  unsigned i; uint32_t v; int ret;
  for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    ret = d->io.read(d->io.arg, K7_DSI2, offsets[i], &v);
    if (ret < 0) return ret;
    if (v) return -EIO;
  }
  return 0;
}
int k7_dsi2_init(struct k7_dsi2 *d, const struct k7_display_io *io)
{
  int ret;
  if (!d) return -EINVAL;
  memset(d, 0, sizeof(*d));
  ret = k7_dcphy_init(&d->phy, io);
  if (ret < 0) return ret;
  d->io = *io; return 0;
}
int k7_dsi2_shutdown(struct k7_dsi2 *d)
{
  int a = 0, b, current;
  if (!d || !d->io.write) return -EINVAL;
  d->prepared = false; d->video = false;
  if (d->touched) a = wr(d, 0x000c, 0);
  b = k7_dcphy_power_off(&d->phy);
  current = a < 0 ? a : b;
  /* Historical evidence and this cleanup attempt's completion are separate. */
  if (!d->cleanup_error && d->phy.cleanup_error < 0)
    d->cleanup_error = d->phy.cleanup_error;
  if (!d->cleanup_error && current < 0) d->cleanup_error = current;
  if (current == 0) d->touched = false;
  return current;
}
int k7_dsi2_prepare(struct k7_dsi2 *d, uint32_t sys_hz, uint32_t ref_hz,
                    bool verified_resources)
{
  unsigned i; uint32_t esc, ratio; int ret;
  /* 560Mbps lane -> 35MHz internal PHY HS clock, IPI clock 21MHz.
   * Reject unrepresentable clock ratios instead of truncating register fields. */
  if (!d || !d->io.write) return -EINVAL;
  if (!verified_resources) return -EACCES;
  if (ref_hz != 24000000u) return -ENOSYS;
  if (!sys_hz) return -EINVAL;
  esc = (uint32_t)(((uint64_t)sys_hz + 39999999u) / 40000000u);
  ratio = rounded((uint64_t)35000000u << 16, sys_hz);
  if (esc < 1 || esc > 31 || ratio > 0x1ffff ||
      sys_hz / (2 * esc) < 1000000u) return -ERANGE;
  if (d->prepared || d->touched || d->faulted) return -EBUSY;
  d->touched = true; d->cleanup_error = 0;
#define TRY(x) do { ret = (x); if (ret < 0) goto fail; } while (0)
  TRY(wr(d, 0x0010, 0)); TRY(d->io.delay_us(d->io.arg, 100));
  TRY(wr(d, 0x0010, 7)); TRY(wr(d, 0x000c, 0));
  TRY(wr(d, 0x0024, 1)); /* manual timing mode */
  TRY(wr(d, 0x0100, 0x130)); /* 16bit PPI, four DPHY lanes */
  TRY(wr(d, 0x0104, (esc << 8) | 1)); /* noncontinuous until PHY ready */
  TRY(wr(d, 0x0134, rounded((uint64_t)35000000u << 16, 21000000u)));
  TRY(wr(d, 0x013c, ratio));
  /* U-Boot DPHY defaults in ps: UI=ceil(1e12/560e6)=1786.
   * LP2HS=50000+(40000+4UI)+(105000+6UI); HS2LP=60000+16UI+100000.
   * Internal PHY period=ceil(1e12/35e6)=28572ps. */
  TRY(wr(d, 0x010c, rounded((uint64_t)212860u << 16, 28572u)));
  TRY(wr(d, 0x0114, rounded((uint64_t)188576u << 16, 28572u)));
  TRY(wr(d, 0x0200, 2)); /* BTA on; EOT off, as panel DTS */
  TRY(wr(d, 0x0204, 0)); TRY(wr(d, 0x0208, 0));
  for (i = 0; i < 7; ++i) TRY(wr(d, 0x0404 + i * 0x10, 0));
  TRY(k7_dcphy_power_on(&d->phy, ref_hz, verified_resources));
  TRY(wr(d, 0x000c, 1)); TRY(wr(d, 0x0018, 2));
  TRY(poll(d, 0x001c, 7, 2, 20000));
  d->prepared = true;
  return 0;
fail:
  d->faulted = true; k7_dsi2_shutdown(d); return ret;
#undef TRY
}
int k7_dsi2_panel_tx(void *arg, unsigned char type,
                     const unsigned char *data, size_t size)
{
  struct k7_dsi2 *d = arg;
  uint32_t hdr; size_t i, j; int ret;
  if (!d || !data || !size || size > 255) return -EINVAL;
  if (!((type == 5 && size == 1) || (type == 0x15 && size == 2) ||
        (type == 0x39 && size >= 3))) return -EINVAL;
  if (!d->prepared || !d->phy.ready || d->faulted) return -EPIPE;
#define TRY(x) do { ret = (x); if (ret < 0) goto fail; } while (0)
  TRY(update(d, 0x0104, 1, 0)); /* panel uses continuous clock */
  TRY(update(d, 0x020c, 1u << 20, 1u << 20)); /* LP commands */
  TRY(poll(d, 0x0020, 3u << 16, 0, 1000));
  TRY(tx_health(d));
  hdr = type | (1u << 24);
  if (type == 0x39) {
    hdr |= (uint32_t)size << 8;
    for (i = 0; i < size; i += 4) {
      uint32_t word = 0;
      for (j = 0; j < 4 && i + j < size; ++j)
        word |= (uint32_t)data[i + j] << (8 * j);
      TRY(wr(d, 0x02c4, word));
    }
  } else {
    hdr |= (uint32_t)data[0] << 8;
    if (size == 2) hdr |= (uint32_t)data[1] << 16;
  }
  /* Hardware adds packet ECC/CRC, matching vendor CRI interface. */
  TRY(wr(d, 0x02c0, hdr));
  TRY(poll(d, 0x0020, 3u << 16, 0, 1000));
  TRY(tx_health(d));
  return (int)size;
fail:
  d->faulted = true; /* uncertain TX is never retried */
  return ret;
#undef TRY
}
int k7_dsi2_video_enable(struct k7_dsi2 *d, bool panel_and_vop_ready)
{
  int ret;
  if (!d || !d->prepared || d->faulted || !d->phy.ready) return -EPIPE;
  if (!panel_and_vop_ready) return -EACCES;
  if (d->video) return -EBUSY;
#define TRY(x) do { ret = (x); if (ret < 0) goto fail; } while (0)
  TRY(update(d, 0x0104, 1, 0));
  TRY(wr(d, 0x0344, 720)); TRY(wr(d, 0x0300, 0x50));
  TRY(d->io.write(d->io.arg, K7_DSI_GRF, 0x28, 0x0ff00500));
  TRY(wr(d, 0x0304, rounded((uint64_t)15 * 35000000u << 16, 84000000u)));
  TRY(wr(d, 0x030c, rounded((uint64_t)32 * 35000000u << 16, 84000000u)));
  TRY(wr(d, 0x0314, rounded((uint64_t)720 * 35000000u << 16, 84000000u)));
  TRY(wr(d, 0x031c, rounded((uint64_t)799 * 35000000u << 16, 84000000u)));
  TRY(wr(d, 0x0324, 15)); TRY(wr(d, 0x032c, 12));
  TRY(wr(d, 0x0334, 1280)); TRY(wr(d, 0x033c, 21));
  TRY(wr(d, 0x020c, 2)); /* burst mode, blanking HS disabled */
  TRY(wr(d, 0x0018, 3)); TRY(poll(d, 0x001c, 7, 3, 20000));
  d->video = true; return 0;
fail:
  d->faulted = true; k7_dsi2_shutdown(d); return ret;
#undef TRY
}
int k7_dsi2_video_disable(struct k7_dsi2 *d)
{
  int ret;
  if (!d || !d->io.write) return -EINVAL;
  if (!d->prepared) return 0;
  /* Even a faulted transaction may have left video live. This explicit
   * safety transition remains available and never clears the fault latch. */
  ret = wr(d, 0x0344, 0);
  if (ret == 0) ret = wr(d, 0x0018, 2);
  if (ret == 0) ret = poll(d, 0x001c, 7, 2, 20000);
  if (ret < 0) {
    d->faulted = true;
    if (!d->cleanup_error) d->cleanup_error = ret;
    return ret;
  }
  d->video = false;
  return 0;
}
