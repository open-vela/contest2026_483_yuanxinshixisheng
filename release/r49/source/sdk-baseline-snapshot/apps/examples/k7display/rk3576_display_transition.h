/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RK3576_DISPLAY_TRANSITION_H
#define RK3576_DISPLAY_TRANSITION_H
#include "rk3576_display_resources.h"

/* Hardware validation remains separate from source/offline tests. The native
 * caller uses resources_on; configure_clocks alone supports only VOP/VO0
 * ALREADY powered and
 * non-idle, and VP1_SRC, VP1 and DSIHOST0 clocks ALREADY gated. It deliberately
 * does not power domains, toggle shared gates/resets, or release DMA idle.
 * Caller holds the global clock/power lock and exclusive VP1/DSI ownership
 * until successful shutdown/recovery. Do not manufacture proofs from names,
 * boot logs, or a successful read of one unrelated register.
 */
#define K7DT_EXCLUSIVE_CLOCK_TREE (1u << 5)
#define K7DT_VP1_DSI_QUIESCENT    (1u << 6)
#define K7DT_REQUIRED_PROOFS (K7DR_ALL_PROOFS | K7DT_EXCLUSIVE_CLOCK_TREE | \
                             K7DT_VP1_DSI_QUIESCENT)
enum k7dt_extra_reg {
  K7DT_PWR, K7DT_CHAIN, K7DT_MEM, K7DT_UNGATE,
  K7DT_SEL_VP0, K7DT_SEL_VDEC, K7DT_GATE_VDEC,
  K7DT_SEL_EBC, K7DT_GATE_EBC, K7DT_SEL_CHANNEL,
  K7DT_SEL_VOP_BUS, K7DT_SEL_VO0_BUS, K7DT_SEL_TOP0,
  K7DT_SEL_TOP1, K7DT_GATE_TOP0, K7DT_EXTRA_COUNT
};
enum k7dt_stage {
  K7DT_PREFLIGHT, K7DT_SLOW, K7DT_PLL_DOWN, K7DT_PLL_M,
  K7DT_PLL_PS, K7DT_PLL_K, K7DT_PLL_UP, K7DT_PLL_LOCK,
  K7DT_NORMAL, K7DT_VP1, K7DT_VP1_FINAL, K7DT_DSI, K7DT_VERIFY,
  K7DT_DONE
};
#define K7DT_M_DOMAIN 1u
#define K7DT_M_GATED  2u
#define K7DT_M_OWNER  4u
#define K7DT_M_CPLL   8u
#define K7DT_M_OTHER 16u
#define K7DT_M_BUS   32u
struct k7dt_io {
  void *arg;
  int (*read)(void *, uintptr_t, uint32_t *);
  int (*write)(void *, uintptr_t, uint32_t);
  int (*barrier)(void *); /* completed device writes; e.g. DSB SY */
  int (*delay_us)(void *, unsigned);
};
struct k7dt_report {
  struct k7dr_report resources;
  struct k7dr_report verified_resources; /* fresh post-write, leaves still gated */
  uint32_t extra[K7DT_EXTRA_COUNT];
  uint64_t bus_hz[8]; /* channel A/H, VOP A/H/P, VO0 A/H/P */
  uint32_t vp_vpll_ungated; /* gate61 masks: VP0 bit10 / VP2 bit12 */
  unsigned foreign_vpll_ungated; /* VDEC / EBC final / EBC frac */
  unsigned sampled_extra, missing, write_attempts;
  enum k7dt_stage stage;
  int error, cleanup_error;
  unsigned attempted, configured, pll_touched;
};
uintptr_t k7dt_probe_address(enum k7dt_extra_reg);
/* Reads only PMU/CRU/GRF. Report has the minimum extra snapshot needed before
 * considering a domain transition. Negative result always means zero writes.
 */
int k7dt_probe(const struct k7dt_io *, unsigned proofs, struct k7dt_report *);
/* Start with a zero-initialized report. One explicit attempt only; subsequent
 * calls with attempted=1 fail EALREADY, including after an uncertain error.
 * Failure retains ownership: VP1/DSI remain gated, and a best-effort VPLL slow
 * mode is recorded separately. No automatic PLL rollback/retry. Success means
 * ONLY clocks configured, still gated; it does not mean display ready.
 */
int k7dt_configure_clocks(const struct k7dt_io *, unsigned proofs,
                         struct k7dt_report *);
/* K7 boot-only resource adoption. Verified official K7 DT has no PM-domain
 * clocks/supply properties or QoS-init overrides for these domains. VOP's
 * device supply is boot-on/always-on vdd_logic_s0; this API never changes it.
 * Existing proofs additionally cover all VOP/DSI/DCPHY masters quiescent and
 * no other display/MIPI driver, under a global PMU/clock lock. Source review
 * of the display-only build plus fresh registers establishes this; no new
 * user confirmation is implied. MAPS includes QoS page 0x27f11000 and page
 * 0x27f12000 (both normal Device memory, not cacheable).
 */
enum k7dt_resource_stage {
  K7DT_R_PREFLIGHT, K7DT_R_GATE_LEAVES, K7DT_R_BUS_GATES,
  K7DT_R_VOP_POWER, K7DT_R_VO0_POWER, K7DT_R_RESETS,
  K7DT_R_QOS_SNAPSHOT, K7DT_R_CLOCKS, K7DT_R_LEAVES,
  K7DT_R_VERIFY, K7DT_R_DONE, K7DT_R_CLAIM_VP_SOURCES,
  K7DT_R_VOP_BUS_REPARENT
};
enum k7dt_power_stage {
  K7DT_P_NONE, K7DT_P_UNGATE, K7DT_P_ON, K7DT_P_CHAIN,
  K7DT_P_MEM_OFF, K7DT_P_MEM_ON, K7DT_P_REPAIR,
  K7DT_P_DEIDLE, K7DT_P_RESTORE_UNGATE
};
struct k7dt_resource_report {
  struct k7dt_report before, clocks;
  struct k7dr_report after;
  uint32_t qos[3][5]; /* VOP_M0, VOP_M1RO, HDCP0; read only */
  uint32_t original_ungate;
  uint32_t vp_sources_claimed, vp_standby[2]; /* VP0/VP2, retained closed */
  unsigned vp_sources_touched;
  uint32_t bus_vp_standby[3], vop_bus_before, vop_bus_after;
  unsigned vop_bus_planned, vop_bus_touched, vop_bus_verified;
  /* Total write attempts = writes + clocks.write_attempts. */
  unsigned attempted, ready, writes, domains_adopted, domains_powered;
  unsigned ungate_touched;
  enum k7dt_resource_stage stage;
  enum k7dt_power_stage power_stage;
  int error, cleanup_error;
};
/* Zero-initialize report once. Opens only required gates/deasserts selected
 * resets, powers only missing VOP then VO0, programs the existing clock
 * candidate, opens owned leaves, and performs final read-only validation.
 * NEVER cycles an already-on domain or writes QoS/USB/VO1 power state.
 * On failure domains/gates already opened stay on; no recursive retry or
 * unsafe rollback. Under the existing exclusive-display proof, a live VP0/2
 * VPLL source may be adopted only after two fresh identical standby reads.
 * Its gate remains closed on success and failure; no dormant output resumes.
 * Foreign VDEC/EBC consumers are never adopted. Only owned leaf gates are
 * closed best-effort. A known SPLL-selected VOP AXI root can change to the
 * vendor normal CPLL/2=500MHz OPP only when the other seven bus rates decode,
 * CPLL is verified 1GHz and all three VPs have fresh stable standby reads.
 * This is one masked mux/div update without gating the AXI root or reading
 * secure SPLL registers. Its original rate remains unknown. Failed changes
 * are not retried or rolled back; the report records uncertainty. The caller
 * must retain resource ownership and inspect both error fields.
 */
int k7dt_resources_on(const struct k7dt_io *, unsigned proofs,
                     struct k7dt_resource_report *);
#endif
