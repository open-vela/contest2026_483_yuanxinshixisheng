/* SPDX-License-Identifier: GPL-2.0-only */
/* RK3588 PLL sequencing adapted from Rockchip Linux clk-pll.c (RK3576 uses
 * this PLL type). Copyright (c) 2014 MundoReader S.L.
 * Author: Heiko Stuebner <heiko@sntech.de>
 * Copyright (c) 2015 Rockchip Electronics Co. Ltd.
 * Author: Xing Zheng <zhengxing@rock-chips.com>
 * Register topology from clk-rk3576.c / pm_domains.c.
 * Power sequencing: Copyright (c) 2015 ROCKCHIP, Co. Ltd. (GPL-2.0-only).
 * See DISPLAY_TRANSITION_SOURCE.md for pinned source and unsupported parts.
 */
#include "rk3576_display_transition.h"
#include <errno.h>
#include <string.h>
#define BIT(n) (1u << (n))
#define MODE ((uintptr_t)0x27200280u)
#define VPLL0 ((uintptr_t)0x27200160u)
#define VPLL1 ((uintptr_t)0x27200164u)
#define VPLL2 ((uintptr_t)0x27200168u)
#define VPLL6 ((uintptr_t)0x27200178u)
static const uintptr_t extra_addr[K7DT_EXTRA_COUNT] = {
  0x27380210u, 0x27380248u, 0x27380250u, 0x27380140u,
  0x27200544u, 0x272004b8u, 0x272008b4u,
  0x272004ecu, 0x272008c8u, 0x2720034cu,
  0x27200540u, 0x27200554u, 0x27200300u,
  0x27200304u, 0x27200800u
};
uintptr_t k7dt_probe_address(enum k7dt_extra_reg n)
{ return (unsigned)n < K7DT_EXTRA_COUNT ? extra_addr[n] : 0; }
static int neg(int e) { return e > 0 ? -EIO : e; }
static uint64_t parent_rate(const uint32_t *v, enum k7dr_reg first,
                            unsigned mode_shift)
{
  uint32_t m=v[first]&1023, p=v[first+1]&63, s=(v[first+1]>>6)&7;
  uint64_t numerator=(uint64_t)24000000*m, divisor=(uint64_t)p<<s;
  if(!p || !m || !divisor || ((v[K7DR_PLL_MODE]>>mode_shift)&3)!=1 ||
     (v[first+1]&BIT(13)) || (v[first+2]&65535) ||
     !(v[first+3]&BIT(15)) || numerator%divisor ||
     numerator<(uint64_t)2250000000u*p || numerator>(uint64_t)4500000000ULL*p)
    return 0;
  return numerator/divisor;
}
static uint64_t divide_exact(uint64_t rate, unsigned div)
{ return rate && div && !(rate%div) ? rate/div : 0; }
static uint64_t ah_parent(uint32_t src, uint64_t g, uint64_t c)
{ return src==0?g:src==1?c:0; }
static int check_bus_parents(struct k7dt_report *r)
{
  uint32_t a=r->extra[K7DT_SEL_CHANNEL], b=r->extra[K7DT_SEL_VOP_BUS];
  uint32_t d=r->extra[K7DT_SEL_VO0_BUS], s;
  uint64_t g=parent_rate(r->resources.raw,K7DR_GPLL0,2);
  uint64_t c=parent_rate(r->resources.raw,K7DR_CPLL0,8);
  uint64_t top[4], hp[4], pp[4];
  unsigned i;
  if(r->resources.raw[K7DR_PHY_REF]&BIT(1)) c=0;
  /* Despite their names, clk_cpll_div20/div10 and gpll_div8/div6 are
   * programmable COMPOSITEs, NOT fixed factors. Read their mux/div/gates. */
  for(i=0;i<4;i++) {
    uint32_t reg=r->extra[i<2?K7DT_SEL_TOP0:K7DT_SEL_TOP1];
    unsigned shift=(i&1)?6:0;
    top[i]=(r->extra[K7DT_GATE_TOP0]&BIT(i))?0:
      divide_exact(ah_parent((reg>>(shift+5))&1,g,c),((reg>>shift)&31)+1);
  }
  hp[0]=top[3];hp[1]=top[1];hp[2]=top[0];hp[3]=24000000;
  pp[0]=top[2];pp[1]=top[1];pp[2]=top[0];pp[3]=24000000;
  r->bus_hz[0]=divide_exact(ah_parent((a>>12)&3,g,c),((a>>8)&15)+1);
  r->bus_hz[1]=hp[(a>>6)&3];
  r->bus_hz[2]=divide_exact(ah_parent((b>>5)&7,g,c),(b&31)+1);
  r->bus_hz[3]=hp[(b>>10)&3];
  s=(b>>12)&3;
  r->bus_hz[4]=s==0?top[1]:s==1?top[0]:s==2?24000000:0;
  r->bus_hz[5]=divide_exact(ah_parent((d>>5)&3,g,c),(d&31)+1);
  r->bus_hz[6]=hp[(d>>7)&3];
  s=(d>>11)&3;
  r->bus_hz[7]=pp[s];
  /* Validate actual parent/divider/gate, not a guessed maximum rate. The
   * caller records the decoded rates for the selected operating point. */
  for(i=0;i<8;i++) if(!r->bus_hz[i]) return -ENOSYS;
  return 0;
}

int k7dt_probe(const struct k7dt_io *io, unsigned proofs,
               struct k7dt_report *r)
{
  struct k7dr_io ro;
  uint32_t *v, *x, m, p, s;
  unsigned i;
  int e;
  if (!r) return -EINVAL;
  memset(r, 0, sizeof(*r));
  if (!io || !io->read) return r->error = -EINVAL;
  if ((proofs & K7DT_REQUIRED_PROOFS) != K7DT_REQUIRED_PROOFS)
    return r->error = -EPERM;
  ro.arg = io->arg; ro.read = io->read;
  e = k7dr_check(&ro, proofs & K7DR_ALL_PROOFS, 100000000, &r->resources);
  if (e && e != -EAGAIN) return r->error = e;
  for (i = 0; i < K7DT_EXTRA_COUNT; i++) {
    e = neg(io->read(io->arg, extra_addr[i], &r->extra[i]));
    if (e) return r->error = e;
    r->sampled_extra++;
  }
  v = r->resources.raw; x = r->extra;
  if (check_bus_parents(r)) r->missing |= K7DT_M_BUS;
  if ((v[K7DR_PMU_REPAIR] & 0x8800) != 0x8800 ||
      (x[K7DT_PWR] & 0x8800) || (x[K7DT_CHAIN] & 0x8800) != 0x8800 ||
      (x[K7DT_MEM] & 0x8800) ||
      ((v[K7DR_PMU_IDLE] | v[K7DR_PMU_ACK] | v[K7DR_PMU_REQ]) & 0x6800))
    r->missing |= K7DT_M_DOMAIN;
  /* These owned leaves must already be stopped before either mux changes or
   * PLL power-down. Never infer that a display is idle merely from a gate. */
  if (!(v[K7DR_GATE_VOP61] & BIT(11)) ||
      !(v[K7DR_GATE_VOP62] & BIT(0)) ||
      !(v[K7DR_GATE_DSI64] & BIT(6))) r->missing |= K7DT_M_GATED;
  /* Reject ANY active non-owned direct VPLL consumer. Check VP source gates
   * even if their downstream final mux happens to select HDMI PHY. Reject an
   * active EBC fractional source even if final EBC is temporarily deselected. */
  if (((x[K7DT_SEL_VP0] >> 8) & 7) == 2 &&
      !(v[K7DR_GATE_VOP61] & BIT(10))) r->vp_vpll_ungated |= BIT(10);
  if (((v[K7DR_SEL_FINAL] >> 8) & 7) == 2 &&
      !(v[K7DR_GATE_VOP61] & BIT(12))) r->vp_vpll_ungated |= BIT(12);
  if (((x[K7DT_SEL_VDEC] >> 14) & 3) == 1 &&
      !(x[K7DT_GATE_VDEC] & BIT(2))) r->foreign_vpll_ungated |= 1;
  if (((x[K7DT_SEL_EBC] >> 12) & 7) == 2 &&
      !(x[K7DT_GATE_EBC] & BIT(12))) r->foreign_vpll_ungated |= 2;
  if ((x[K7DT_SEL_EBC] & 7) == 2 &&
      !(x[K7DT_GATE_EBC] & BIT(9))) r->foreign_vpll_ungated |= 4;
  if (r->vp_vpll_ungated || r->foreign_vpll_ungated)
    r->missing |= K7DT_M_OWNER;
  /* CPLL is shared: verify exactly 1GHz, never reprogram it. */
  m = v[K7DR_CPLL0] & 1023; p = v[K7DR_CPLL1] & 63;
  s = (v[K7DR_CPLL1] >> 6) & 7;
  if ((v[K7DR_PHY_REF] & BIT(1)) ||
      ((v[K7DR_PLL_MODE] >> 8) & 3) != 1 || !p || !m ||
      (v[K7DR_CPLL1] & BIT(13)) || (v[K7DR_CPLL2] & 65535) ||
      !(v[K7DR_CPLL6] & BIT(15)) ||
      (uint64_t)24000000 * m != (uint64_t)1000000000 * (p << s) ||
      (uint64_t)24000000 * m < (uint64_t)2250000000u * p ||
      (uint64_t)24000000 * m > (uint64_t)4500000000ULL * p)
    r->missing |= K7DT_M_CPLL;
  /* No unimplemented prerequisite may be partly fixed by this transition.
   * Gate/reset deficiencies are outside this module except owned leaf gates. */
  if ((v[K7DR_GATE_CHANNEL] & 3) || (v[K7DR_GATE_VOP61] & 0x030d) ||
      (v[K7DR_GATE_VO63] & 0x0b) || (v[K7DR_GATE_DSI64] & BIT(5)) ||
      (v[K7DR_GATE_PHY0] & 4) || (v[K7DR_GATE_PMUPHY5] & 1) ||
      (v[K7DR_GATE_PMU7] & BIT(9)) ||
      (r->resources.missing & (K7DR_M_RESETS | K7DR_M_PHYREF)))
    r->missing |= K7DT_M_OTHER;
  return r->error = r->missing ? -ENOSYS : 0;
}

static int masked(const struct k7dt_io *io, struct k7dt_report *r,
                  uintptr_t a, uint32_t mask, uint32_t data)
{
  uint32_t v;
  int e;
  r->write_attempts++;
  e = neg(io->write(io->arg, a, (mask << 16) | (data & mask)));
  if (!e) e = neg(io->barrier(io->arg));
  if (!e) e = neg(io->read(io->arg, a, &v));
  if (!e && (v & mask) != (data & mask)) e = -EIO;
  return e;
}
int k7dt_configure_clocks(const struct k7dt_io *io, unsigned proofs,
                         struct k7dt_report *r)
{
  unsigned i;
  uint32_t v;
  int e;
  if (!r || !io || !io->read || !io->write || !io->barrier || !io->delay_us)
    return -EINVAL;
  if (r->attempted) return -EALREADY;
  e = k7dt_probe(io, proofs, r);
  if (e) return e; /* Every prerequisite sampled before the FIRST write. */
  r->attempted = 1;
#define SET(stage_, address_, mask_, data_) do { \
    r->stage = (stage_); \
    e = masked(io, r, (address_), (mask_), (data_)); \
    if (e) goto fail; \
  } while (0)
  r->pll_touched = 1; /* a failed bus write is also an uncertain mutation */
  SET(K7DT_SLOW, MODE, 3u << 4, 0);
  SET(K7DT_PLL_DOWN, VPLL1, BIT(13), BIT(13));
  SET(K7DT_PLL_M, VPLL0, 1023, 112);
  SET(K7DT_PLL_PS, VPLL1, 511, 1 | (2u << 6));
  SET(K7DT_PLL_K, VPLL2, 65535, 0);
  SET(K7DT_PLL_UP, VPLL1, BIT(13), 0);
  r->stage = K7DT_PLL_LOCK;
  for (i = 0; i <= 1000; i++) {
    e = neg(io->read(io->arg, VPLL6, &v));
    if (e) goto fail;
    if (v & BIT(15)) break;
    if (i == 1000) { e = -ETIMEDOUT; goto fail; }
    e = neg(io->delay_us(io->arg, 1));
    if (e) goto fail;
  }
  SET(K7DT_NORMAL, MODE, 3u << 4, 1u << 4);
  SET(K7DT_VP1, k7dr_address(K7DR_SEL_VP1), 0x7ff, 0x207);
  SET(K7DT_VP1_FINAL, k7dr_address(K7DR_SEL_FINAL), BIT(12), 0);
  SET(K7DT_DSI, k7dr_address(K7DR_SEL_DSI), 0x3ff, 0x89);
  r->stage = K7DT_VERIFY;
  {
    struct k7dr_io ro = { io->arg, io->read };
    struct k7dr_report *final = &r->verified_resources;
    e = k7dr_check(&ro, proofs & K7DR_ALL_PROOFS, 100000000, final);
    if ((e && e != -EAGAIN) || final->vpll_hz != 672000000 ||
        final->pixel_hz != 84000000 || final->dsi_sys_hz != 100000000 ||
        (final->missing & ~K7DR_M_GATES) ||
        !(final->raw[K7DR_GATE_VOP61] & BIT(11)) ||
        !(final->raw[K7DR_GATE_VOP62] & BIT(0)) ||
        !(final->raw[K7DR_GATE_DSI64] & BIT(6))) {
      if (!e || e == -EAGAIN) e = -EIO;
      goto fail;
    }
  }
  r->stage = K7DT_DONE; r->configured = 1; return 0;
fail:
  r->error = e;
  /* Never recursively reprogram the old PLL or touch a non-owned leaf. */
  r->cleanup_error = masked(io, r, MODE, 3u << 4, 0);
  return e;
#undef SET
}
/* No domain-off operation is exported: USB/VO1 may depend on adopted VOP.
 * These masks come from rk3576_pm_domains, never from inferred MMIO contents.
 */
#define PMU_PWR ((uintptr_t)0x27380210u)
#define PMU_MEM_PWR ((uintptr_t)0x27380300u)
#define PMU_CHAIN ((uintptr_t)0x27380248u)
#define PMU_MEM ((uintptr_t)0x27380250u)
#define PMU_REPAIR ((uintptr_t)0x27380570u)
#define PMU_REQ ((uintptr_t)0x27380110u)
#define PMU_ACK ((uintptr_t)0x27380120u)
#define PMU_IDLE ((uintptr_t)0x27380128u)
#define PMU_UNGATE ((uintptr_t)0x27380140u)
static int resource_masked(const struct k7dt_io *io,
                           struct k7dt_resource_report *r,
                           uintptr_t a, uint32_t mask, uint32_t value)
{
  uint32_t v;
  int e;
  r->writes++;
  e = neg(io->write(io->arg, a, (mask << 16) | (value & mask)));
  if (!e) e = neg(io->barrier(io->arg));
  if (!e) e = neg(io->read(io->arg, a, &v));
  if (!e && (v & mask) != (value & mask)) e = -EIO;
  return e;
}
static int power_poll(const struct k7dt_io *io, uintptr_t a,
                      uint32_t mask, uint32_t expected)
{
  unsigned i;
  uint32_t v;
  int e;
  for (i = 0; i <= 1000; i++) {
    e = neg(io->read(io->arg, a, &v));
    if (e) return e;
    if ((v & mask) == expected) return 0;
    if (i == 1000) return -ETIMEDOUT;
    e = neg(io->delay_us(io->arg, 10));
    if (e) return e;
  }
  return -ETIMEDOUT;
}
static int power_domain_on(const struct k7dt_io *io,
                           struct k7dt_resource_report *r,
                           uint32_t bit, uint32_t idle, uint32_t ungate,
                           unsigned delay_us)
{
  uint32_t repair, mem, req, ack, idle_state;
  int e;
  if ((e = neg(io->read(io->arg, PMU_REPAIR, &repair)))) return e;
  if ((e = neg(io->read(io->arg, PMU_MEM, &mem)))) return e;
  if ((e = neg(io->read(io->arg, PMU_REQ, &req)))) return e;
  if ((e = neg(io->read(io->arg, PMU_ACK, &ack)))) return e;
  if ((e = neg(io->read(io->arg, PMU_IDLE, &idle_state)))) return e;
  if (repair & bit) {
    r->domains_adopted |= bit;
    if (!((req | ack | idle_state) & idle)) return 0;
  }
  r->power_stage = K7DT_P_UNGATE;
  r->ungate_touched = 1;
  e = resource_masked(io, r, PMU_UNGATE, ungate, ungate);
  if (e) return e;
  if (!(repair & bit)) {
    r->power_stage = K7DT_P_ON;
    e = resource_masked(io, r, PMU_PWR, bit, 0);
    if (e) return e;
    /* Vendor's SRAM-reset workaround applies only to a domain being powered
     * on while its initial memory-status says already on. Never apply to an
     * adopted domain, especially the USB parent VOP. */
    if (!(mem & bit)) {
      r->power_stage = K7DT_P_CHAIN;
      e = power_poll(io, PMU_CHAIN, bit, bit);
      if (e) return e;
      e = neg(io->delay_us(io->arg, 60));
      if (e) return e;
      r->power_stage = K7DT_P_MEM_OFF;
      e = resource_masked(io, r, PMU_MEM_PWR, bit, bit);
      if (!e) e = power_poll(io, PMU_MEM, bit, bit);
      if (e) return e;
      r->power_stage = K7DT_P_MEM_ON;
      e = resource_masked(io, r, PMU_MEM_PWR, bit, 0);
      if (!e) e = power_poll(io, PMU_MEM, bit, 0);
      if (e) return e;
    }
    r->power_stage = K7DT_P_REPAIR;
    e = power_poll(io, PMU_REPAIR, bit, bit);
    if (!e) e = power_poll(io, PMU_CHAIN, bit, bit);
    if (!e) e = power_poll(io, PMU_MEM, bit, 0);
    if (e) return e;
    r->domains_powered |= bit;
  }
  r->power_stage = K7DT_P_DEIDLE;
  e = resource_masked(io, r, PMU_REQ, idle, 0);
  if (!e) e = power_poll(io, PMU_ACK, idle, 0);
  if (!e) e = power_poll(io, PMU_IDLE, idle, 0);
  if (!e && delay_us) e = neg(io->delay_us(io->arg, delay_us));
  if (e) return e;
  r->power_stage = K7DT_P_RESTORE_UNGATE;
  return resource_masked(io, r, PMU_UNGATE, ungate, r->original_ungate);
}

static const struct { enum k7dr_reg reg; uint32_t mask; } bus_gates[] = {
  { K7DR_GATE_CHANNEL, 3 }, { K7DR_GATE_VOP61, 0x030d },
  { K7DR_GATE_VO63, 0x0b }, { K7DR_GATE_DSI64, BIT(5) },
  { K7DR_GATE_PHY0, 4 }, { K7DR_GATE_PMUPHY5, 1 },
  { K7DR_GATE_PMU7, BIT(9) }
}, reset_deassert[] = {
  { K7DR_RESET_CHANNEL, 3 }, { K7DR_RESET_VOP61, 0x3f0 },
  { K7DR_RESET_VOP62, 0x0d }, { K7DR_RESET_VO63, 0x4a0 },
  { K7DR_RESET_DSI64, 0x60 }, { K7DR_RESET_PHY0, 0x0c },
  { K7DR_RESET_PHY1, 0x18 }
};
static int resource_leaves(const struct k7dt_io *io,
                           struct k7dt_resource_report *r, unsigned close,
                           unsigned best_effort)
{
  const enum k7dr_reg reg[3] = { K7DR_GATE_VOP61, K7DR_GATE_VOP62,
                                K7DR_GATE_DSI64 };
  const uint32_t mask[3] = { BIT(11), 1, BIT(6) };
  int first = 0, e;
  unsigned i;
  for (i = 0; i < 3; i++) {
    e = resource_masked(io, r, k7dr_address(reg[i]), mask[i],
                        close ? mask[i] : 0);
    if (e && !first) first = e;
    if (e && !best_effort) break;
  }
  return first;
}
/* Dormant bootloader VP source gates can remain enabled after standby.
 * Check accessibility before touching device MMIO, then verify standby twice
 * under the caller's exclusive clock/display lock. A gate never proves idle.
 * Same register facts as k7dg_device_readable; no VOP write occurs here. */
static int vop_status_readable(const struct k7dt_resource_report *r)
{
  const uint32_t *v=r->before.resources.raw, *x=r->before.extra;
  if (!(v[K7DR_PMU_REPAIR]&BIT(11)) || (x[K7DT_PWR]&BIT(11)) ||
      !(x[K7DT_CHAIN]&BIT(11)) || (x[K7DT_MEM]&BIT(11)) ||
      ((v[K7DR_PMU_REQ]|v[K7DR_PMU_ACK]|v[K7DR_PMU_IDLE])&0x6000) ||
      (v[K7DR_GATE_CHANNEL]&3) || (v[K7DR_RESET_CHANNEL]&3) ||
      (v[K7DR_GATE_VOP61]&0x030d) || (v[K7DR_RESET_VOP61]&0x3f0) ||
      (v[K7DR_RESET_VOP62]&0x0c)) return -EACCES;
  return 0;
}
static int quiet_vp_sources(const struct k7dt_io *io,
                            struct k7dt_resource_report *r)
{
  uint32_t mask=r->before.vp_vpll_ungated, first, second;
  unsigned i; int e;
  if (!mask) return 0;
  e=vop_status_readable(r);
  if (e) return e;
  for (i=0;i<2;i++) {
    if (!(mask&BIT(i?12:10))) continue;
    e=neg(io->read(io->arg,i?0x27d00e00u:0x27d00c00u,&first));
    if (e) return e;
    e=neg(io->read(io->arg,i?0x27d00e00u:0x27d00c00u,&second));
    if (e) return e;
    if (first!=second) return -EAGAIN;
    if (!(second&BIT(31))) return -EBUSY;
    r->vp_standby[i]=second;
  }
  r->vp_sources_claimed=mask;
  return 0;
}

/* CON144 parent table: GPLL,CPLL,AUPLL,SPLL,LPLL. SPLL is a fixed-clock
 * firmware contract in Linux, not an observable non-secure PLL here. Do not
 * invent its measured rate or read secure CRU. Only this known source may
 * change to the vendor normal OPP, CPLL/2=500MHz; all other buses must decode.
 * Vendor U-Boot updates mux/div in one hiword write, WITHOUT gating AXI root.
 * Standby is not proof that outstanding AXI transactions have drained. */
static int plan_vop_bus(const struct k7dt_io *io,
                         struct k7dt_resource_report *r)
{
  uint32_t first, second;
  unsigned i; int e;
  if (!(r->before.missing&K7DT_M_BUS)) return 0;
  if (((r->before.extra[K7DT_SEL_VOP_BUS]>>5)&7)!=3 ||
      r->before.bus_hz[2]) return -ENOSYS;
  for (i=0;i<8;i++) if (i!=2 && !r->before.bus_hz[i]) return -ENOSYS;
  /* Caller has already rejected unknown/non-1GHz CPLL and foreign owners. */
  e=vop_status_readable(r);
  if (e) return e;
  for (i=0;i<3;i++) {
    e=neg(io->read(io->arg,0x27d00c00u+i*0x100u,&first));
    if (e) return e;
    e=neg(io->read(io->arg,0x27d00c00u+i*0x100u,&second));
    if (e) return e;
    if (first!=second) return -EAGAIN;
    if (!(second&BIT(31))) return -EBUSY;
    r->bus_vp_standby[i]=second;
  }
  r->vop_bus_before=r->before.extra[K7DT_SEL_VOP_BUS];
  r->vop_bus_planned=1;
  return 0;
}

int k7dt_resources_on(const struct k7dt_io *io, unsigned proofs,
                     struct k7dt_resource_report *r)
{
  static const uintptr_t qos[3] = { 0x27f12800u, 0x27f12880u, 0x27f11000u };
  uint32_t *v, *x, repair, active_children;
  struct k7dr_io ro;
  unsigned i, j;
  int e, ce;
  if (!r || !io || !io->read || !io->write || !io->barrier || !io->delay_us)
    return -EINVAL;
  if (r->attempted) return -EALREADY;
  memset(r, 0, sizeof(*r));
  e = k7dt_probe(io, proofs, &r->before);
  if (e && e != -ENOSYS) return r->error = e;
  /* Gating/reset/power deficiencies will be fixed below, but not foreign PLL
   * ownership, an unknown CPLL, or the shared DCPHY reference parent. */
  if (r->before.foreign_vpll_ungated || (r->before.missing & K7DT_M_CPLL))
    return r->error = -EBUSY;
  v = r->before.resources.raw; x = r->before.extra;
  if (v[K7DR_PHY_REF] & BIT(4)) return r->error = -ENOSYS;
  repair = v[K7DR_PMU_REPAIR];
  active_children = repair & (BIT(14) | BIT(15) | BIT(16));
  if (!(repair & BIT(11)) && active_children) return r->error = -EBUSY;
  /* Adopt only internally coherent live domains. Do not "repair" their
   * SRAM or assert parent idle while USB/VO1 might be executing. */
  for (i = 0; i < 2; i++) {
    uint32_t bit = i ? BIT(15) : BIT(11);
    if ((repair & bit) &&
        ((x[K7DT_PWR] & bit) || (x[K7DT_MEM] & bit) || !(x[K7DT_CHAIN] & bit)))
      return r->error = -EAGAIN;
  }
  if (active_children &&
      ((v[K7DR_PMU_REQ] | v[K7DR_PMU_ACK] | v[K7DR_PMU_IDLE]) & 0x6000))
    return r->error = -EBUSY;
  e=plan_vop_bus(io,r);
  if (e) return r->error=e;
  e=quiet_vp_sources(io,r);
  if (e) return r->error=e;
  r->original_ungate = x[K7DT_UNGATE];
  r->attempted = 1;
  if (r->vop_bus_planned) {
    r->stage=K7DT_R_VOP_BUS_REPARENT;
    r->vop_bus_touched=1;
    e=resource_masked(io,r,k7dt_probe_address(K7DT_SEL_VOP_BUS),0xff,0x21);
    if (e) goto fail;
    e=neg(io->read(io->arg,k7dt_probe_address(K7DT_SEL_VOP_BUS),
                    &r->vop_bus_after));
    if (e) goto fail;
    if (r->vop_bus_after != ((r->vop_bus_before&~0xffu)|0x21)) {
      e=-EIO; goto fail;
    }
    r->vop_bus_verified=1;
  }
  if (r->vp_sources_claimed) {
    r->stage=K7DT_R_CLAIM_VP_SOURCES;
    r->vp_sources_touched=1;
    e=resource_masked(io,r,k7dr_address(K7DR_GATE_VOP61),
                      r->vp_sources_claimed,r->vp_sources_claimed);
    if (e) goto fail;
  }
  r->stage = K7DT_R_GATE_LEAVES;
  e = resource_leaves(io, r, 1, 0);
  if (e) goto fail;
  r->stage = K7DT_R_BUS_GATES;
  for (i = 0; i < sizeof(bus_gates) / sizeof(bus_gates[0]); i++) {
    e = resource_masked(io, r, k7dr_address(bus_gates[i].reg), bus_gates[i].mask, 0);
    if (e) goto fail;
  }
  r->stage = K7DT_R_VOP_POWER;
  e = power_domain_on(io, r, BIT(11), 0x6000, 0x6000, 15);
  if (e) goto fail;
  r->stage = K7DT_R_VO0_POWER;
  e = power_domain_on(io, r, BIT(15), BIT(11), 0x6800, 0);
  if (e) goto fail;
  r->stage = K7DT_R_RESETS;
  for (i = 0; i < sizeof(reset_deassert) / sizeof(reset_deassert[0]); i++) {
    e = resource_masked(io, r, k7dr_address(reset_deassert[i].reg),
                        reset_deassert[i].mask, 0);
    if (e) goto fail;
  }
  r->stage = K7DT_R_QOS_SNAPSHOT;
  /* Linux restores QoS only after its own saved power-off, or explicit DT
   * init properties. Neither applies to this boot-only adopter. Preserve
   * current/reset values verbatim, even when they differ from expectations. */
  for (i = 0; i < 3; i++) for (j = 0; j < 5; j++) {
    e = neg(io->read(io->arg, qos[i] + 8 + j * 4, &r->qos[i][j]));
    if (e) goto fail;
  }
  r->stage = K7DT_R_CLOCKS;
  e = k7dt_configure_clocks(io, proofs, &r->clocks);
  if (e) goto fail;
  r->stage = K7DT_R_LEAVES;
  e = resource_leaves(io, r, 0, 0);
  if (e) goto fail;
  r->stage = K7DT_R_VERIFY;
  ro.arg = io->arg; ro.read = io->read;
  e = k7dr_check(&ro, proofs & K7DR_ALL_PROOFS, 100000000, &r->after);
  if (e) goto fail;
  r->stage = K7DT_R_DONE; r->ready = 1; return 0;
fail:
  r->error = e;
  r->cleanup_error = r->clocks.cleanup_error;
  ce = resource_leaves(io, r, 1, 1);
  if (!r->cleanup_error) r->cleanup_error = ce;
  if (r->vp_sources_touched) {
    ce=resource_masked(io,r,k7dr_address(K7DR_GATE_VOP61),
                       r->vp_sources_claimed,r->vp_sources_claimed);
    if (!r->cleanup_error) r->cleanup_error=ce;
  }
  if (r->ungate_touched) {
    ce = resource_masked(io, r, PMU_UNGATE, 0x6800, r->original_ungate);
    if (!r->cleanup_error) r->cleanup_error = ce;
  }
  return e;
}
