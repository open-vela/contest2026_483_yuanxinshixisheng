/* SPDX-License-Identifier: GPL-2.0-or-later
 * (C) Copyright 2008-2018 Fuzhou Rockchip Electronics Co., Ltd
 * Author: Guochun Huang <hero.huang@rock-chips.com>
 * Derived from Rockchip U-Boot samsung_mipi_dcphy.c, GPL-2.0+.
 * Fixed RK3576 4-lane DPHY TX at 560 Mbps; see DSI2_SOURCE.md.
 */
#include "rk3576_dcphy.h"
#include <errno.h>
#include <string.h>

static int wr(struct k7_dcphy *p, uint32_t r, uint32_t v)
{ return p->io.write(p->io.arg, K7_DCPHY, r, v); }
static int update(struct k7_dcphy *p, uint32_t r, uint32_t m, uint32_t v)
{
  uint32_t old; int ret = p->io.read(p->io.arg, K7_DCPHY, r, &old);
  return ret < 0 ? ret : wr(p, r, (old & ~m) | (v & m));
}
int k7_dcphy_init(struct k7_dcphy *p, const struct k7_display_io *io)
{
  if (!p || !io || !io->read || !io->write || !io->delay_us ||
      !io->m_phy_reset) return -EINVAL;
  memset(p, 0, sizeof(*p)); p->io = *io; return 0;
}
int k7_dcphy_power_off(struct k7_dcphy *p)
{
  unsigned i; int ret, first = 0;
  if (!p || !p->io.write) return -EINVAL;
  if (!p->touched) return 0;
  p->ready = false;
  /* Best-effort shutdown still attempts every lane and PLL on a write error. */
  for (i = 0; i < 5; ++i) {
    ret = update(p, 0x300 + i * 0x100, 1, 0);
    if (ret < 0 && !first) first = ret;
  }
  ret = update(p, 0x100, 1u << 12, 0);
  if (ret < 0 && !first) first = ret;
  ret = p->io.m_phy_reset(p->io.arg, true);
  if (ret < 0 && !first) first = ret;
  if (first < 0 && !p->cleanup_error) p->cleanup_error = first;
  if (!first) p->touched = false;
  return first;
}
int k7_dcphy_power_on(struct k7_dcphy *p, uint32_t ref_hz,
                      bool verified_resources)
{
  unsigned i, elapsed; uint32_t status; int ret;
  if (!p || !p->io.write) return -EINVAL;
  if (!verified_resources) return -EACCES;
  if (ref_hz != 24000000u) return -ENOSYS;
  if (p->ready || p->touched) return -EBUSY;
  p->touched = true; p->cleanup_error = 0;
#define TRY(x) do { ret = (x); if (ret < 0) goto fail; } while (0)
  /* Clear only master CPHY mode using GRF high-word mask. */
  TRY(p->io.write(p->io.arg, K7_DCPHY_GRF, 0, 1u << 16));
  TRY(p->io.m_phy_reset(p->io.arg, true));
  TRY(wr(p, 0x000, 0x0010)); TRY(wr(p, 0x004, 0x0110));
  TRY(wr(p, 0x008, 0x3223));
  /* Fout=(M*2*24MHz)/(P*2^S), M=280 P=3 S=3 -> 560MHz.
   * VCO=4480MHz, Fref=8MHz, both inside vendor limits. No fractional/SSC. */
  TRY(update(p, 0x100, 0x073f | (1u << 12), 0x0303));
  TRY(wr(p, 0x104, 0)); TRY(update(p, 0x108, 0x3ff, 280));
  TRY(update(p, 0x110, 1u << 11, 0));
  TRY(wr(p, 0x114, 0x0500));
  TRY(wr(p, 0x11c, 0xf000)); TRY(wr(p, 0x120, 0xf000));
  /* Vendor table row 560: clk_prepare=22,clk_zero=7,clk_post=1,
   * clk_trail=45,hs_prepare=23,hs_zero=2,hs_trail=38,lpx=16,hs_exit=2. */
  TRY(wr(p, 0x300, 0xf000));
  TRY(wr(p, 0x308, 0x7133)); /* clock 52/52 ohm */
  TRY(wr(p, 0x30c, 0));
  TRY(wr(p, 0x330, 0x1100)); TRY(wr(p, 0x334, 0x0716));
  TRY(wr(p, 0x338, 0x022d)); TRY(wr(p, 0x33c, 1));
  TRY(wr(p, 0x340, 0x1f4)); TRY(wr(p, 0x350, 0));
  for (i = 0; i < 4; ++i) {
    uint32_t b = 0x400 + i * 0x100;
    TRY(update(p, b, 1, 0));
    TRY(wr(p, b + 8, 0x71ee)); /* data 39/39 ohm */
    TRY(wr(p, b + 12, 0));
    TRY(wr(p, b + 0x30, 0x1100)); TRY(wr(p, b + 0x34, 0x0217));
    TRY(wr(p, b + 0x38, 0x0226)); TRY(wr(p, b + 0x3c, 0x30));
    TRY(wr(p, b + 0x40, 0x1f4));
  }
  TRY(update(p, 0x100, 1u << 12, 1u << 12));
  for (elapsed = 0; ; elapsed += 10) {
    TRY(p->io.read(p->io.arg, K7_DCPHY, 0x140, &status));
    p->pll_status = status; p->pll_status_valid = true;
    if (status & 1) break;
    if (elapsed >= 20000) { ret = -ETIMEDOUT; goto fail; }
    TRY(p->io.delay_us(p->io.arg, 10));
  }
  for (i = 0; i < 5; ++i) {
    uint32_t b = 0x300 + i * 0x100;
    TRY(wr(p, b + 4, 0x2000)); TRY(update(p, b, 1, 1));
  }
  TRY(p->io.m_phy_reset(p->io.arg, false));
  TRY(p->io.delay_us(p->io.arg, 100));
  p->ready = true;
  return 0;
fail:
  k7_dcphy_power_off(p);
  return ret;
#undef TRY
}
