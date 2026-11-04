/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rk3576_dsi2.h"
#include "f050008m01.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct mock {
  uint32_t reg[4][512];
  unsigned writes, delay, headers, words, bad_read, fail_at;
  unsigned stuck_pll, stuck_mode, busy, tx_busy_after_header;
  unsigned reset_asserted;
  uint32_t header, payload[64];
};
static int rd(void *a, enum k7_display_block b, uint32_t r, uint32_t *v)
{
  struct mock *m = a;
  if (m->bad_read) return -EIO;
  assert(r % 4 == 0 && r / 4 < 512);
  if (b == K7_DCPHY && r == 0x140) *v = !m->stuck_pll;
  else if (b == K7_DSI2 && r == 0x20)
    *v = (m->busy || (m->tx_busy_after_header && m->headers)) ? 1u << 16 : 0;
  else *v = m->reg[b][r / 4];
  return 0;
}
static int wr(void *a, enum k7_display_block b, uint32_t r, uint32_t v)
{
  struct mock *m = a;
  assert(r % 4 == 0 && r / 4 < 512);
  ++m->writes;
  if (m->fail_at && m->writes == m->fail_at) return -EIO;
  m->reg[b][r / 4] = v;
  if (b == K7_DSI2 && r == 0x18 && !m->stuck_mode) m->reg[b][0x1c / 4] = v;
  if (b == K7_DSI2 && r == 0x2c0) { m->header = v; ++m->headers; }
  if (b == K7_DSI2 && r == 0x2c4) {
    if (m->words < 64) m->payload[m->words] = v;
    ++m->words;
  }
  return 0;
}
static int delay(void *a, unsigned us) { ((struct mock *)a)->delay += us; return 0; }
static int reset(void *a, bool asserted)
{ ((struct mock *)a)->reset_asserted = asserted; return 0; }
static int panel_delay(void *a, unsigned ms) { (void)a; (void)ms; return 0; }
static void fresh(struct k7_dsi2 *d, struct mock *m)
{
  struct k7_display_io io = {rd, wr, delay, reset, m};
  memset(m, 0, sizeof(*m)); assert(k7_dsi2_init(d, &io) == 0);
}
int main(void)
{
  struct k7_dsi2 d; struct mock m;
  unsigned n, i; unsigned char data[] = {0xff, 0x87, 0x56, 0x01, 0xa5};
  fresh(&d, &m);
  assert(k7_dsi2_prepare(&d, 100000000, 24000000, false) == -EACCES);
  assert(k7_dsi2_prepare(&d, 100000000, 26000000, true) == -ENOSYS);
  assert(k7_dsi2_prepare(&d, 0, 24000000, true) == -EINVAL);
  assert(k7_dsi2_prepare(&d, 1000000, 24000000, true) == -ERANGE);
  assert(m.writes == 0);
  assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == 0);
  assert(d.phy.ready && d.prepared && !m.reset_asserted);
  assert(m.reg[K7_DCPHY][0x100 / 4] == 0x1303);
  assert(m.reg[K7_DCPHY][0x108 / 4] == 280);
  assert(m.reg[K7_DSI2][0x104 / 4] == 0x301);
  n = m.writes;
  assert(k7_dsi2_panel_tx(&d, 0x05, data, 2) == -EINVAL);
  assert(k7_dsi2_panel_tx(&d, 0x06, data, 1) == -EINVAL);
  assert(m.writes == n);
  assert(k7_dsi2_panel_tx(&d, 0x39, data, 5) == 5);
  assert(m.header == 0x01000539 && m.words == 2);
  assert(m.payload[0] == 0x015687ff && m.payload[1] == 0xa5);
  assert(k7_dsi2_panel_tx(&d, 0x15, data, 2) == 2);
  assert(m.header == 0x0187ff15);
  { struct k7panel_io panel = {k7_dsi2_panel_tx, panel_delay, &d};
    n = m.headers; assert(k7panel_run(k7panel_init, k7panel_init_size, &panel) == 0);
    assert(m.headers - n == 171); }
  n = m.writes;
  assert(k7_dsi2_video_enable(&d, false) == -EACCES && m.writes == n);
  assert(k7_dsi2_video_enable(&d, true) == 0 && d.video);
  assert(m.reg[K7_DSI2][0x304 / 4] == 409600); /* 15*35/84*65536 */
  assert(m.reg[K7_DSI2][0x314 / 4] == 19660800);
  assert(m.reg[K7_DSI2][0x31c / 4] == 21818027);
  assert(k7_dsi2_video_disable(&d) == 0 && !d.video && d.prepared);
  assert(m.reg[K7_DSI2][0x1c / 4] == 2);
  assert(k7_dsi2_shutdown(&d) == 0 && !d.phy.ready && !d.touched);
  assert(m.reset_asserted && !(m.reg[K7_DCPHY][0x100 / 4] & 0x1000));
  fresh(&d, &m); m.stuck_pll = 1;
  assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == -ETIMEDOUT);
  assert(m.delay == 20100 && d.faulted && !d.prepared && m.reset_asserted);
  assert(d.phy.pll_status_valid && !(d.phy.pll_status & 1));
  assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == -EBUSY);
  fresh(&d, &m); m.stuck_mode = 1;
  /* Mode 3 has bit 1 set but is not command mode 2: exact status required. */
  m.reg[K7_DSI2][0x1c / 4] = 3;
  assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == -ETIMEDOUT);
  assert(d.failed_poll_reg == 0x1c && d.failed_poll_value == 3 &&
         d.failed_poll_error == -ETIMEDOUT);
  fresh(&d, &m); assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == 0);
  m.busy = 1;
  assert(k7_dsi2_panel_tx(&d, 5, data, 1) == -ETIMEDOUT && m.headers == 0);
  m.busy = 0; assert(k7_dsi2_panel_tx(&d, 5, data, 1) == -EPIPE);
  fresh(&d, &m); assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == 0);
  m.tx_busy_after_header = 1;
  assert(k7_dsi2_panel_tx(&d, 5, data, 1) == -ETIMEDOUT && m.headers == 1);
  assert(k7_dsi2_panel_tx(&d, 5, data, 1) == -EPIPE && m.headers == 1);
  fresh(&d, &m); assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == 0);
  m.reg[K7_DSI2][0x420 / 4] = 1;
  assert(k7_dsi2_panel_tx(&d, 5, data, 1) == -EIO && m.headers == 0);
  fresh(&d, &m); assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == 0);
  m.fail_at = m.writes + 1;
  assert(k7_dsi2_shutdown(&d) == -EIO && d.touched && d.cleanup_error == -EIO);
  assert(k7_dsi2_shutdown(&d) == 0 && !d.touched && d.cleanup_error == -EIO);
  /* Each individual preparation write failure propagates and clears readiness. */
  fresh(&d, &m); assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == 0);
  n = m.writes;
  for (i = 1; i <= n; ++i) {
    fresh(&d, &m); m.fail_at = i;
    assert(k7_dsi2_prepare(&d, 100000000, 24000000, true) == -EIO);
    assert(!d.prepared && !d.phy.ready && d.faulted);
  }
  puts("PASS: fixed PLL, panel 171 packets, RGB888 timing, fail-closed preflight,");
  puts("timeouts, exact mode, no uncertain-TX retry, all preparation write faults");
  return 0;
}
