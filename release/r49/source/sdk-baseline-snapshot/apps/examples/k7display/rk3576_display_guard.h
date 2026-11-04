/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_DISPLAY_GUARD_H
#define K7_DISPLAY_GUARD_H
#include <stdint.h>
#include <stddef.h>
enum k7dg_reg { K7DG_REPAIR,K7DG_POWER,K7DG_CHAIN,K7DG_MEM,K7DG_REQ,
  K7DG_ACK,K7DG_IDLE,K7DG_CHANNEL,K7DG_G61,K7DG_G63,K7DG_G64,
  K7DG_GP0,K7DG_GP5,K7DG_GP7,K7DG_R61,K7DG_R62,K7DG_R63,
  K7DG_R64,K7DG_RP0,K7DG_RP1,K7DG_RCHANNEL,K7DG_COUNT };
struct k7dg_snapshot { uint32_t raw[K7DG_COUNT]; };
struct k7dg_io { void *arg; int (*read)(void *,uintptr_t,uint32_t *); };
struct k7dg_report {
  struct k7dg_snapshot before,after;
  uint32_t vp[3],dsi_power,phy_pll,phy_lane[5];
  unsigned sampled_device,proofs;
  unsigned vop_cold; /* power-off or all VOP core/pixel resets asserted */
  int error;
};
#define K7DG_VOP_QUIET 1u
#define K7DG_DSI_QUIET 2u
#define K7DG_PHY_QUIET 4u
#define K7DG_ALL_QUIET 7u
/* AON only; neither this nor check writes registers or opens a clock. */
int k7dg_sample(const struct k7dg_io *,struct k7dg_snapshot *);
/* Narrow read capability after real domain, bus gate/reset, idle checks. */
int k7dg_device_readable(const struct k7dg_snapshot *,uintptr_t address);
int k7dg_check(const struct k7dg_io *,struct k7dg_report *);
#endif
