/* SPDX-License-Identifier: Apache-2.0 */
#include "rk3576_display_resources.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fake { uint32_t v[K7DR_REG_COUNT]; unsigned calls,fail; };
static int readreg(void *arg,uintptr_t a,uint32_t *v)
{
  struct fake *f=arg; unsigned i;
  f->calls++; if(f->calls==f->fail)return -EIO;
  for(i=0;i<K7DR_REG_COUNT;i++) if(k7dr_address((enum k7dr_reg)i)==a) {
    *v=f->v[i]; return 0;
  }
  assert(0); return -EIO;
}
static void good(struct fake *f)
{
  memset(f,0,sizeof(*f)); f->v[K7DR_PMU_REPAIR]=0x8800;
  f->v[K7DR_SEL_VP1]=0x207; /* VPLL /8 */
  f->v[K7DR_SEL_DSI]=1; /* GPLL /2 =594MHz */
  f->v[K7DR_PLL_MODE]=(1u<<4)|(1u<<2)|(1u<<8);
  f->v[K7DR_VPLL0]=112; f->v[K7DR_VPLL1]=1|(2u<<6);
  f->v[K7DR_VPLL6]=0x8000;
  f->v[K7DR_GPLL0]=198; f->v[K7DR_GPLL1]=2|(1u<<6);
  f->v[K7DR_GPLL6]=0x8000;
}
int main(void)
{
  struct fake f; struct k7dr_io io={&f,readreg}; struct k7dr_report r;
  unsigned i;
  good(&f); assert(k7dr_check(&io,K7DR_ALL_PROOFS,594000000,&r)==0);
  assert(r.ready && r.pixel_hz==84000000 && r.vpll_hz==672000000 && r.dsi_sys_hz==594000000);
  for(i=0;i<5;i++) {
    good(&f); assert(k7dr_check(&io,K7DR_ALL_PROOFS&~(1u<<i),594000000,&r)<0);
    assert(!r.ready && (r.missing&K7DR_M_PROOFS));
    if(i==0 || i==4) assert(f.calls==0);
  }
  for(i=1;i<=K7DR_REG_COUNT;i++) {
    good(&f); f.fail=i;
    assert(k7dr_check(&io,K7DR_ALL_PROOFS,594000000,&r)==-EIO);
    assert(!r.ready && r.sampled==i-1 && (r.missing&K7DR_M_READ));
  }
  good(&f); f.v[K7DR_PMU_REPAIR]&=~0x8000u;
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_POWER));
  good(&f); f.v[K7DR_PMU_ACK]=0x2000;
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_IDLE));
  good(&f); f.v[K7DR_GATE_DSI64]=0x20;
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_GATES));
  good(&f); f.v[K7DR_RESET_PHY1]=0x10;
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_RESETS));
  good(&f); f.v[K7DR_PHY_REF]=0x10;
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_PHYREF));
  good(&f); f.v[K7DR_VPLL1]=0;
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_VPLL));
  good(&f); f.v[K7DR_VPLL2]=1;
  assert(k7dr_check(&io,31,594000000,&r)<0 && !r.pixel_hz);
  good(&f); f.v[K7DR_VPLL0]=99; /* previous594MHz /8 isn't84MHz */
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_PIXEL_RATE));
  good(&f); f.v[K7DR_SEL_FINAL]=0x1000;
  assert(k7dr_check(&io,31,594000000,&r)<0 && (r.missing&K7DR_M_VP1_SOURCE));
  good(&f); assert(k7dr_check(&io,31,0,&r)<0 && (r.missing&K7DR_M_DSI_RATE));
  /* Unknown DSI parent is never guessed from a nominal boot frequency. */
  good(&f); f.v[K7DR_SEL_DSI]=0x100;
  assert(k7dr_check(&io,31,594000000,&r)<0 && !r.dsi_sys_hz);
  puts("display resource read-only preflight: prerequisites/rates/faults PASS");
  return 0;
}
