/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RK3576_DISPLAY_RESOURCES_H
#define RK3576_DISPLAY_RESOURCES_H
#include <stdint.h>

/* Read-only preflight. Never powers domains, releases bus idle, changes PLLs,
 * clocks or resets, and never accesses potentially unpowered VOP/DSI/DCPHY.
 * Missing these prerequisites requires a separate audited transition owner.
 * Maps required: CRU 27200000/PMU_CRU 27220000/PMU 27380000/
 * PMU0_GRF 26024000. Serialize clock/power changes while sampling.
 * BUS_PARENTS is externally proven live upstream clocks (including PMUPHY,
 * VO0VOP interconnect and VOP/VO0 bus parent PLLs); this module checks their
 * downstream gates/resets but does not certify every upstream PLL topology.
 * XIN24M proves the board's reference is 24MHz; NOMMU proves caller's VOP
 * DMA address/translation plan. No proof may be inferred from a boot banner.
 */
#define K7DR_MAPS        1u
#define K7DR_BUS_PARENTS 2u
#define K7DR_XIN24M      4u
#define K7DR_NOMMU       8u
#define K7DR_STABLE      16u
#define K7DR_ALL_PROOFS  31u
enum k7dr_reg {
  K7DR_PMU_REPAIR, K7DR_PMU_IDLE, K7DR_PMU_ACK, K7DR_PMU_REQ,
  K7DR_GATE_CHANNEL, K7DR_GATE_VOP61, K7DR_GATE_VOP62,
  K7DR_GATE_VO63, K7DR_GATE_DSI64, K7DR_GATE_PHY0,
  K7DR_GATE_PMUPHY5, K7DR_GATE_PMU7,
  K7DR_RESET_CHANNEL, K7DR_RESET_VOP61, K7DR_RESET_VOP62,
  K7DR_RESET_VO63, K7DR_RESET_DSI64, K7DR_RESET_PHY0, K7DR_RESET_PHY1,
  K7DR_SEL_VP1, K7DR_SEL_FINAL, K7DR_SEL_DSI,
  K7DR_PHY_REF, K7DR_PLL_MODE,
  K7DR_VPLL0, K7DR_VPLL1, K7DR_VPLL2, K7DR_VPLL6,
  K7DR_GPLL0, K7DR_GPLL1, K7DR_GPLL2, K7DR_GPLL6,
  K7DR_CPLL0, K7DR_CPLL1, K7DR_CPLL2, K7DR_CPLL6,
  K7DR_REG_COUNT
};
#define K7DR_M_PROOFS       (1u<<0)
#define K7DR_M_READ         (1u<<1)
#define K7DR_M_POWER        (1u<<2)
#define K7DR_M_IDLE         (1u<<3)
#define K7DR_M_GATES        (1u<<4)
#define K7DR_M_RESETS       (1u<<5)
#define K7DR_M_PHYREF       (1u<<6)
#define K7DR_M_VP1_SOURCE   (1u<<7)
#define K7DR_M_VPLL         (1u<<8)
#define K7DR_M_PIXEL_RATE   (1u<<9)
#define K7DR_M_DSI_RATE     (1u<<10)
struct k7dr_io {
  void *arg;
  int (*read)(void *, uintptr_t, uint32_t *); /* 0 or negative errno */
};
struct k7dr_report {
  uint32_t raw[K7DR_REG_COUNT];
  unsigned sampled, proofs, missing, ready;
  uint64_t vpll_hz, pixel_hz, dsi_sys_hz;
  int read_error;
};
uintptr_t k7dr_address(enum k7dr_reg);
/* expected_sys_hz is the independently selected DSI host system-clock rate.
 * No default is silently assumed. Pixel target=84MHz, PHY ref=24MHz.
 * The narrowly supported pixel topology is VPLL -> VP1_SRC -> VP1.
 * Others report missing even if potentially usable; fractional PLLs rejected.
 */
int k7dr_check(const struct k7dr_io *, unsigned proofs,
              uint64_t expected_sys_hz, struct k7dr_report *);
#endif
