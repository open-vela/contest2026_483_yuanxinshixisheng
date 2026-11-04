/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_RK3576_DCPHY_H
#define K7_RK3576_DCPHY_H
#include <stdint.h>
#include <stdbool.h>

enum k7_display_block { K7_DSI2, K7_DCPHY, K7_DSI_GRF, K7_DCPHY_GRF };
/* Callbacks use byte offsets, not physical addresses. Caller provides ordered
 * MMIO, mapped regions, exclusive ownership and a real microsecond delay.
 * All callbacks return 0 or negative errno. No callback may silently retry. */
struct k7_display_io {
  int (*read)(void *, enum k7_display_block, uint32_t, uint32_t *);
  int (*write)(void *, enum k7_display_block, uint32_t, uint32_t);
  int (*delay_us)(void *, unsigned int);
  int (*m_phy_reset)(void *, bool asserted);
  void *arg;
};
struct k7_dcphy {
  struct k7_display_io io;
  bool ready;
  bool touched;
  int cleanup_error;
  uint32_t pll_status;
  bool pll_status_valid;
};
#define K7_DCPHY_LANE_RATE_HZ 560000000u
/* Caller has enabled PCLK_MIPI_DCPHY, CLK_PHY_REF_SRC=24MHz and released
 * APB/GRF resets. DCPHY must be exclusively in DPHY TX use, never shared CSI.
 * verified_resources is attestation by platform preflight, not a bypass. */
int k7_dcphy_init(struct k7_dcphy *, const struct k7_display_io *);
int k7_dcphy_power_on(struct k7_dcphy *, uint32_t ref_hz,
                      bool verified_resources);
int k7_dcphy_power_off(struct k7_dcphy *);
#endif
