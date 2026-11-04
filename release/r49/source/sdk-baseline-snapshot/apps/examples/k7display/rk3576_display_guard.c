/* SPDX-License-Identifier: Apache-2.0 */
/* Register facts: vendor rk3576.dtsi, clk-rk3576.c, pm_domains.c and
 * rockchip,rk3576-cru.h. A gate alone NEVER proves a stopped DMA master.
 * This read-only guard must run before any display clock/power write.
 */
#include "rk3576_display_guard.h"
#include <errno.h>
#include <string.h>
#define BIT(n) (1u<<(n))
static const uintptr_t addresses[K7DG_COUNT]={
  0x27380570,0x27380210,0x27380248,0x27380250,0x27380110,
  0x27380120,0x27380128,0x27200808,0x272008f4,0x272008fc,0x27200900,
  0x27220800,0x27220814,0x2722081c,0x27200af4,0x27200af8,0x27200afc,
  0x27200b00,0x27220a00,0x27220a04,0x27200a08
};
int k7dg_sample(const struct k7dg_io *io,struct k7dg_snapshot *s)
{
  unsigned i; int ret;
  if(!io||!io->read||!s) return -EINVAL;
  memset(s,0,sizeof(*s));
  for(i=0;i<K7DG_COUNT;i++) {
    ret=io->read(io->arg,addresses[i],&s->raw[i]);
    if(ret) return ret<0?ret:-EIO;
  }
  return 0;
}
static int on(const uint32_t *r,unsigned bit,unsigned idle)
{
  return (r[K7DG_REPAIR]&bit) && !(r[K7DG_POWER]&bit) &&
    (r[K7DG_CHAIN]&bit) && !(r[K7DG_MEM]&bit) &&
    !((r[K7DG_REQ]|r[K7DG_ACK]|r[K7DG_IDLE])&idle);
}
static int off(const uint32_t *r,unsigned bit,unsigned idle)
{
  return !(r[K7DG_REPAIR]&bit) && (r[K7DG_POWER]&bit) &&
    (r[K7DG_ACK]&idle)==idle && (r[K7DG_IDLE]&idle)==idle &&
    (r[K7DG_REQ]&idle)==idle;
}
int k7dg_device_readable(const struct k7dg_snapshot *s,uintptr_t a)
{
  const uint32_t *r;
  if(!s) return 0;
  r=s->raw;
  if(a==0x27d00c00u||a==0x27d00d00u||a==0x27d00e00u)
    return on(r,BIT(11),0x6000) && !(r[K7DG_CHANNEL]&3) && !(r[K7DG_RCHANNEL]&3) &&
      !(r[K7DG_G61]&0x030d) && !(r[K7DG_R61]&0x3f0) &&
      !(r[K7DG_R62]&0x0c);
  if(a==0x27d8000cu)
    return on(r,BIT(11),0x6000) && on(r,BIT(15),0x0800) &&
      !(r[K7DG_CHANNEL]&3) && !(r[K7DG_RCHANNEL]&3) && !(r[K7DG_G63]&0x0b) &&
      !(r[K7DG_G64]&0x60) && !(r[K7DG_R63]&0x0a0) &&
      !(r[K7DG_R64]&0x60);
  if(a==0x2b020100u || (a>=0x2b020300u&&a<=0x2b020700u&&!(a&0xff)))
    return !(r[K7DG_GP0]&4) && !(r[K7DG_GP5]&1) &&
      !(r[K7DG_GP7]&BIT(9)) && !(r[K7DG_RP0]&4);
  /* Domain QoS words are read during transition, before bus_ready. */
  if ((a>=0x27f12808u&&a<=0x27f12818u) ||
      (a>=0x27f12888u&&a<=0x27f12898u))
    return !(a&3) && on(r,BIT(11),0x6000) &&
      !(r[K7DG_CHANNEL]&3) && !(r[K7DG_RCHANNEL]&3) && !(r[K7DG_G61]&0x030d) &&
      !(r[K7DG_R61]&0x3f0) && !(r[K7DG_R62]&0x0c);
  if (a>=0x27f11008u&&a<=0x27f11018u)
    return !(a&3) && on(r,BIT(11),0x6000) && on(r,BIT(15),0x0800) &&
      !(r[K7DG_CHANNEL]&3) && !(r[K7DG_RCHANNEL]&3) && !(r[K7DG_G63]&0x0b) &&
      !(r[K7DG_R63]&0x0a0);
  return 0;
}
int k7dg_check(const struct k7dg_io *io,struct k7dg_report *out)
{
  struct k7dg_snapshot stable;
  const uint32_t *r;
  unsigned i; int e;
  if(!out) return -EINVAL;
  memset(out,0,sizeof(*out));
#define TRY(x) do { e=(x); if(e) {out->error=e<0?e:-EIO; return out->error;} }while(0)
#define READ(a,d) do { if(!k7dg_device_readable(&out->before,(a))) \
  {out->error=-EACCES;return -EACCES;} TRY(io->read(io->arg,(a),(d))); \
  out->sampled_device++; }while(0)
  TRY(k7dg_sample(io,&out->before));
  TRY(k7dg_sample(io,&stable));
  if(memcmp(&stable,&out->before,sizeof(stable))) return out->error=-EAGAIN;
  r=out->before.raw;
  if(off(r,BIT(11),0x6000) ||
     ((r[K7DG_R61]&0x2300)==0x2300 && (r[K7DG_R62]&3)==3))
    {out->proofs|=K7DG_VOP_QUIET; out->vop_cold=1;}
  else {
    for(i=0;i<3;i++) {
      READ(0x27d00c00u+i*0x100u,&out->vp[i]);
      if(!(out->vp[i]&BIT(31))) return out->error=-EBUSY;
    }
    out->proofs|=K7DG_VOP_QUIET;
  }
  if(off(r,BIT(15),0x0800) || (r[K7DG_R64]&0x60)==0x60)
    out->proofs|=K7DG_DSI_QUIET;
  else {
    READ(0x27d8000cu,&out->dsi_power);
    if(out->dsi_power&1) return out->error=-EBUSY;
    out->proofs|=K7DG_DSI_QUIET;
  }
  if(r[K7DG_RP1]&BIT(3)) out->proofs|=K7DG_PHY_QUIET;
  else {
    READ(0x2b020100u,&out->phy_pll);
    if(out->phy_pll&BIT(12)) return out->error=-EBUSY;
    for(i=0;i<5;i++) {
      READ(0x2b020300u+i*0x100u,&out->phy_lane[i]);
      if(out->phy_lane[i]&1) return out->error=-EBUSY;
    }
    out->proofs|=K7DG_PHY_QUIET;
  }
  TRY(k7dg_sample(io,&out->after));
  if(memcmp(&out->before,&out->after,sizeof(out->before)))
    return out->error=-EAGAIN;
  return 0;
}
