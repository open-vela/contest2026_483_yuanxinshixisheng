/* SPDX-License-Identifier: Apache-2.0 */
#include "rk3576_display_resources.h"
#include <errno.h>
#include <string.h>

/* Original read-only implementation of register facts, not copied drivers.
 * Sources: official rk3576.dtsi, clk-rk3576.c, and
 * u-boot/include/dt-bindings/clock/rockchip,rk3576-cru.h (SOFTRST offsets).
 * https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/soc/rockchip/pm_domains.c
 * rk3576: VOP repair11, VO0 repair15; VOP idle/ack13|14, VO0 idle/ack11.
 * https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/clk/rockchip/clk-pll.c
 * RK3588 PLL: M[9:0], P[5:0], S[8:6], signed K[15:0], lock CON6[15].
 * Only integer K=0 is accepted here. VPLL 672MHz /8 is representable by
 * P=1 M=112 S=2 (2.688GHz VCO), but this module NEVER writes that setting.
 * VOP memory power sequencing, PLL programming/rollback and bus-idle
 * handshake are intentionally NOT implemented by this read-only module.
 */
static const uintptr_t addresses[K7DR_REG_COUNT] = {
  0x27380570u,0x27380128u,0x27380120u,0x27380110u,
  0x27200808u,0x272008f4u,0x272008f8u,0x272008fcu,0x27200900u,
  0x27220800u,0x27220814u,0x2722081cu,
  0x27200a08u,0x27200af4u,0x27200af8u,0x27200afcu,0x27200b00u,
  0x27220a00u,0x27220a04u,
  0x27200548u,0x2720054cu,0x2720055cu,0x26024018u,0x27200280u,
  0x27200160u,0x27200164u,0x27200168u,0x27200178u,
  0x272001c0u,0x272001c4u,0x272001c8u,0x272001d8u,
  0x272001a0u,0x272001a4u,0x272001a8u,0x272001b8u
};
uintptr_t k7dr_address(enum k7dr_reg r)
{
  return (unsigned)r<K7DR_REG_COUNT ? addresses[r] : 0;
}
#define BIT(n) (1u<<(n))
static uint64_t pll(const struct k7dr_report *r, enum k7dr_reg first,
                    unsigned shift)
{
  uint32_t a=r->raw[first], b=r->raw[first+1];
  uint32_t p=b&63, m=a&1023, s=(b>>6)&7;
  uint64_t numerator=(uint64_t)24000000*m, denominator=(uint64_t)p<<s;
  if (((r->raw[K7DR_PLL_MODE]>>shift)&3)!=1 || !p || !m ||
      (b&BIT(13)) || (r->raw[first+2]&65535) ||
      !(r->raw[first+3]&BIT(15)) || !denominator ||
      numerator%denominator) return 0;
  if(numerator < (uint64_t)2250000000u*p ||
     numerator > (uint64_t)4500000000ULL*p) return 0;
  return numerator/denominator;
}
int k7dr_check(const struct k7dr_io *io,unsigned proofs,
              uint64_t expected_sys_hz,struct k7dr_report *r)
{
  unsigned i,src,div;
  uint64_t rate;
  if(!r) return -EINVAL;
  memset(r,0,sizeof(*r)); r->proofs=proofs;
  if(!io || !io->read) { r->missing=K7DR_M_PROOFS; return -EINVAL; }
  if((proofs&K7DR_ALL_PROOFS)!=K7DR_ALL_PROOFS) r->missing|=K7DR_M_PROOFS;
  if(!(proofs&K7DR_MAPS) || !(proofs&K7DR_STABLE)) return -EPERM;
  for(i=0;i<K7DR_REG_COUNT;i++) {
    int e=io->read(io->arg,addresses[i],&r->raw[i]);
    if(e) { r->read_error=e<0 ? e : -EIO; r->missing|=K7DR_M_READ;
      return r->read_error; }
    r->sampled++;
  }
  if((r->raw[K7DR_PMU_REPAIR]&0x8800)!=0x8800) r->missing|=K7DR_M_POWER;
  if((r->raw[K7DR_PMU_IDLE]|r->raw[K7DR_PMU_ACK]|r->raw[K7DR_PMU_REQ])&0x6800)
    r->missing|=K7DR_M_IDLE;
  if((r->raw[K7DR_GATE_CHANNEL]&3) ||
     (r->raw[K7DR_GATE_VOP61]&0x0b0d) || /* root A/H/P + VOP A/H + VP1 src */
     (r->raw[K7DR_GATE_VOP62]&1) ||
     (r->raw[K7DR_GATE_VO63]&0x0b) ||
     (r->raw[K7DR_GATE_DSI64]&0x60) ||
     (r->raw[K7DR_GATE_PHY0]&4) ||
     (r->raw[K7DR_GATE_PMUPHY5]&1) ||
     (r->raw[K7DR_GATE_PMU7]&BIT(9))) r->missing|=K7DR_M_GATES;
  if((r->raw[K7DR_RESET_CHANNEL]&3) ||
     (r->raw[K7DR_RESET_VOP61]&0x3f0) ||
     (r->raw[K7DR_RESET_VOP62]&0x0d) ||
     (r->raw[K7DR_RESET_VO63]&0x4a0) ||
     (r->raw[K7DR_RESET_DSI64]&0x60) ||
     (r->raw[K7DR_RESET_PHY0]&0x0c) ||
     (r->raw[K7DR_RESET_PHY1]&0x18)) r->missing|=K7DR_M_RESETS;
  if(r->raw[K7DR_PHY_REF]&BIT(4)) r->missing|=K7DR_M_PHYREF;
  src=(r->raw[K7DR_SEL_VP1]>>8)&7;
  div=(r->raw[K7DR_SEL_VP1]&255)+1;
  if(src!=2 || (r->raw[K7DR_SEL_FINAL]&BIT(12))) r->missing|=K7DR_M_VP1_SOURCE;
  r->vpll_hz=pll(r,K7DR_VPLL0,4);
  if(!r->vpll_hz) r->missing|=K7DR_M_VPLL;
  if(src==2 && !(r->raw[K7DR_SEL_FINAL]&BIT(12)) && r->vpll_hz%div==0)
    r->pixel_hz=r->vpll_hz/div;
  if(r->pixel_hz!=84000000) r->missing|=K7DR_M_PIXEL_RATE;
  src=(r->raw[K7DR_SEL_DSI]>>7)&7; div=(r->raw[K7DR_SEL_DSI]&127)+1;
  rate=src==0 ? pll(r,K7DR_GPLL0,2) : src==1 ? pll(r,K7DR_CPLL0,8) :
       src==3 ? r->vpll_hz : 0;
  /* A non-crystal CPLL input needs its own upstream rate decoder. */
  if(src==1 && (r->raw[K7DR_PHY_REF]&BIT(1))) rate=0;
  if(rate && rate%div==0) r->dsi_sys_hz=rate/div;
  if(!expected_sys_hz || r->dsi_sys_hz!=expected_sys_hz) r->missing|=K7DR_M_DSI_RATE;
  r->ready=r->missing==0;
  return r->ready ? 0 : -EAGAIN;
}
