/* SPDX-License-Identifier: GPL-2.0-only */
#include "rk3576_display_transition.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define B(n) (1u << (n))
struct fake {
  uint32_t r[K7DR_REG_COUNT], x[K7DT_EXTRA_COUNT];
  unsigned reads, writes, waits, barriers, fail_write;
  int lock_timeout, cleanup_fail;
  unsigned resources, power_timeout, pmu_writes, mem_writes, qos_reads;
  uint32_t mem_req, qos[15];
  uint32_t vp[2];
  unsigned vp_reads, vp_read_error, vp_unstable, ignore_vp_gate, pll_writes;
  uint32_t vp1;
  unsigned bus_writes, ignore_bus_write;
};
static uint32_t *find(struct fake *f, uintptr_t a)
{
  unsigned i;
  if(a==0x27d00c00u || a==0x27d00e00u) return &f->vp[a==0x27d00e00u];
  if(a==0x27d00d00u) return &f->vp1;
  for (i=0;i<K7DR_REG_COUNT;i++) if (k7dr_address(i)==a) return &f->r[i];
  for (i=0;i<K7DT_EXTRA_COUNT;i++) if (k7dt_probe_address(i)==a) return &f->x[i];
  if(a==0x27380300u) return &f->mem_req;
  for(i=0;i<3;i++) {
    uintptr_t b=i==0?0x27f12800u:i==1?0x27f12880u:0x27f11000u;
    if(a>=b+8 && a<=b+24 && !(a&3)) return &f->qos[i*5+(a-b-8)/4];
  }
  assert(0); return NULL;
}
static int rd(void *p, uintptr_t a, uint32_t *v)
{
  struct fake *f=p; f->reads++;
  if(a==0x27d00c00u || a==0x27d00d00u || a==0x27d00e00u) {
    f->vp_reads++;
    if(f->vp_read_error) return -EIO;
    if(f->vp_unstable && f->vp_reads==2) { *v=*find(f,a)^1; return 0; }
  }
  if(a>=0x27f00000 && a<0x28000000) {
    assert((f->r[K7DR_PMU_REPAIR]&0x8800)==0x8800);
    assert(!(f->r[K7DR_PMU_IDLE]&0x6800));
    f->qos_reads++;
  }
  *v=*find(f,a); return 0;
}
static int wr(void *p, uintptr_t a, uint32_t v)
{
  struct fake *f=p;
  uint32_t *r=find(f,a), mask=v>>16;
  f->writes++;
  if(a>=0x27200160u && a<=0x27200178u) f->pll_writes++;
  /* Every writable address and exact mask is independently bounded. */
  if(a==k7dr_address(K7DR_PLL_MODE)) assert(mask==0x30);
  else if(a==k7dr_address(K7DR_VPLL0)) assert(mask==1023);
  else if(a==k7dr_address(K7DR_VPLL1)) assert(mask==8192 || mask==511);
  else if(a==k7dr_address(K7DR_VPLL2)) assert(mask==65535);
  else if(a==k7dr_address(K7DR_SEL_VP1)) assert(mask==0x7ff);
  else if(a==k7dr_address(K7DR_SEL_FINAL)) assert(mask==4096);
  else if(a==k7dr_address(K7DR_SEL_DSI)) assert(mask==0x3ff);
  else if(a==k7dt_probe_address(K7DT_SEL_VOP_BUS)) {
    assert(f->resources && mask==0xff && (v&0xffff)==0x21);
    /* Never gate the VOP AXI root for a live-parent mux update. */
    assert(!(f->r[K7DR_GATE_VOP61]&(B(0)|B(9))));
    f->bus_writes++;
  }
  else {
    unsigned i,found=0;
    const enum k7dr_reg rr[]={K7DR_GATE_CHANNEL,K7DR_GATE_VOP61,K7DR_GATE_VOP62,
      K7DR_GATE_VO63,K7DR_GATE_DSI64,K7DR_GATE_PHY0,K7DR_GATE_PMUPHY5,K7DR_GATE_PMU7,
      K7DR_RESET_CHANNEL,K7DR_RESET_VOP61,K7DR_RESET_VOP62,K7DR_RESET_VO63,
      K7DR_RESET_DSI64,K7DR_RESET_PHY0,K7DR_RESET_PHY1};
    const uint32_t mm[]={3,0x030d,1,0x0b,32,4,1,512,3,0x3f0,0x0d,0x4a0,0x60,0x0c,0x18};
    assert(f->resources);
    if(a==0x27380210u || a==0x27380300u) {
      assert(mask==B(11) || mask==B(15));found=1;
      if(a==0x27380210u) assert(!(v&mask)); /* never power DOWN */
    }
    else if(a==0x27380110u) { assert(mask==0x6000 || mask==B(11)); assert(!(v&mask));found=1; }
    else if(a==0x27380140u) { assert(mask==0x6000 || mask==0x6800);found=1; }
    else for(i=0;i<sizeof(rr)/sizeof(rr[0]);i++) {
      if(a==k7dr_address(rr[i]) && mask==mm[i]) {found=1;break;}
    }
    /* Individual owned leaf masks are allowed in addition to bus masks. */
    if((a==k7dr_address(K7DR_GATE_VOP61) && mask==B(11)) ||
       (a==k7dr_address(K7DR_GATE_DSI64) && mask==B(6))) found=1;
    if(a==k7dr_address(K7DR_GATE_VOP61) && mask &&
       !(mask&~(B(10)|B(12)))) { assert((v&mask)==mask);found=1; }
    assert(found); /* No CPLL/QoS/USB/VO1 writes. */
  }
  if(f->writes==f->fail_write) return -EFAULT;
  if(f->cleanup_fail && f->fail_write && f->writes>f->fail_write) return -EACCES;
  if(f->ignore_bus_write && a==k7dt_probe_address(K7DT_SEL_VOP_BUS)) return 0;
  if(f->ignore_vp_gate && a==k7dr_address(K7DR_GATE_VOP61) &&
     (mask&(B(10)|B(12)))) return 0;
  *r=(*r&~mask)|(v&mask);
  if(a==0x27380210u) {
    f->pmu_writes++;
    if(!(f->power_timeout&mask)) {
      f->r[K7DR_PMU_REPAIR]|=mask;
      f->x[K7DT_CHAIN]|=mask;
      f->x[K7DT_MEM]&=~mask;
    }
  }
  if(a==0x27380300u) {
    f->mem_writes++;
    f->x[K7DT_MEM]=(f->x[K7DT_MEM]&~mask)|(v&mask);
  }
  if(a==0x27380110u) {
    f->r[K7DR_PMU_ACK]&=~mask;f->r[K7DR_PMU_IDLE]&=~mask;
  }
  if(a==k7dr_address(K7DR_VPLL1) && mask==8192)
    f->r[K7DR_VPLL6]=(v&B(13)) || f->lock_timeout ? 0:B(15);
  return 0;
}
static int barrier(void *p) { ((struct fake*)p)->barriers++; return 0; }
static int delay(void *p,unsigned us)
{ assert(us==1||us==10||us==15||us==60); ((struct fake*)p)->waits+=us; return 0; }
static struct k7dt_io init(struct fake *f)
{
  struct k7dt_io io={f,rd,wr,barrier,delay};
  memset(f,0,sizeof(*f));
  f->vp[0]=f->vp[1]=0x8000000f;
  f->vp1=0x8000000f;
  f->r[K7DR_PMU_REPAIR]=0x8800; f->x[K7DT_CHAIN]=0x8800;
  f->r[K7DR_GATE_VOP61]=B(11); f->r[K7DR_GATE_VOP62]=1;
  f->r[K7DR_GATE_DSI64]=B(6);
  f->r[K7DR_PLL_MODE]=(1<<4)|(1<<8)|(1<<2);
  f->r[K7DR_CPLL0]=500; f->r[K7DR_CPLL1]=3|(2<<6);
  f->r[K7DR_CPLL6]=B(15);
  f->r[K7DR_VPLL0]=99; f->r[K7DR_VPLL1]=1|(2<<6);
  f->r[K7DR_VPLL6]=B(15);
  f->r[K7DR_GPLL0]=99;f->r[K7DR_GPLL1]=1|(1<<6);f->r[K7DR_GPLL6]=B(15);
  f->x[K7DT_SEL_CHANNEL]=2<<8; /* GPLL/3=396MHz, H=198MHz */
  f->x[K7DT_SEL_VOP_BUS]=2; /* A=396MHz H=198MHz P=CPLL/10=100MHz */
  f->x[K7DT_SEL_VO0_BUS]=2|(1<<11); /* A=396MHz H=198MHz P=100MHz */
  f->x[K7DT_SEL_TOP0]=19|(1<<5)|(9<<6)|(1<<11); /* CPLL /20, /10 */
  f->x[K7DT_SEL_TOP1]=7|(5<<6); /* GPLL /8, /6 */
  return io;
}
int main(void)
{
  struct fake f;
  struct k7dt_io io;
  struct k7dt_report r;
  unsigned i,normal_writes;
#define RESET() do { io=init(&f); memset(&r,0,sizeof(r)); } while(0)
  RESET();
  assert(k7dt_configure_clocks(&io,0,&r)==-EPERM && f.reads==0 && f.writes==0);
  RESET(); f.x[K7DT_MEM]=B(11);
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ENOSYS && !f.writes);
  RESET(); f.r[K7DR_GATE_DSI64]=0;
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ENOSYS && !f.writes);
  RESET(); f.r[K7DR_CPLL0]=499;
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ENOSYS && !f.writes);
  RESET(); f.x[K7DT_SEL_CHANNEL]=3<<12; /* unknown BPLL parent */
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ENOSYS && !f.writes);
  RESET(); f.r[K7DR_GPLL6]=0;
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ENOSYS && !f.writes);
  RESET(); f.x[K7DT_GATE_TOP0]=B(3); /* selected hclk upstream actually gated */
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ENOSYS && !f.writes);
  RESET(); f.x[K7DT_SEL_TOP1]=(1<<11)|(4<<6)|7; /* H source switched to CPLL /5 */
  assert(k7dt_probe(&io,K7DT_REQUIRED_PROOFS,&r)==0 && r.bus_hz[3]==200000000);
  /* All five non-owned consumer branches must independently fail closed. */
  for(i=0;i<5;i++) {
    RESET();
    if(i==0) f.x[K7DT_SEL_VP0]=2<<8;
    if(i==1) f.r[K7DR_SEL_FINAL]=2<<8;
    if(i==2) f.x[K7DT_SEL_VDEC]=1<<14;
    if(i==3) f.x[K7DT_SEL_EBC]=2<<12;
    if(i==4) f.x[K7DT_SEL_EBC]=2;
    assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ENOSYS);
    assert((r.missing&K7DT_M_OWNER) && !f.writes);
  }
  RESET();
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==0);
  assert(r.configured && r.attempted && r.stage==K7DT_DONE);
  assert(f.r[K7DR_GATE_VOP61]==B(11) && f.r[K7DR_GATE_DSI64]==B(6));
  assert(f.r[K7DR_CPLL0]==500 && f.r[K7DR_CPLL1]==(3|(2<<6)));
  assert(f.r[K7DR_SEL_VP1]==0x207 && f.r[K7DR_SEL_DSI]==0x89);
  assert(r.bus_hz[0]==396000000 && r.bus_hz[3]==198000000 && r.bus_hz[7]==100000000);
  normal_writes=f.writes;
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-EALREADY);
  assert(f.writes==normal_writes);
  for(i=1;i<=normal_writes;i++) {
    RESET(); f.fail_write=i;
    assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-EFAULT);
    assert(r.error==-EFAULT && !r.configured && r.attempted);
    assert(f.writes==i+1 && r.cleanup_error==0);
    assert((f.r[K7DR_PLL_MODE]&0x30)==0);
  }
  RESET(); f.fail_write=3; f.cleanup_fail=1;
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-EFAULT);
  assert(r.error==-EFAULT && r.cleanup_error==-EACCES);
  RESET(); f.lock_timeout=1;
  assert(k7dt_configure_clocks(&io,K7DT_REQUIRED_PROOFS,&r)==-ETIMEDOUT);
  assert(f.waits==1000 && r.stage==K7DT_PLL_LOCK && !r.configured);
  assert((f.r[K7DR_PLL_MODE]&0x30)==0);
  {
    struct k7dt_resource_report rr;
    unsigned writes;
    RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
    assert(k7dt_resources_on(&io,0,&rr)==-EPERM && !f.reads && !f.writes);
    /* Full observed attempt-10 topology: SPLL source is NOT assumed measured.
     * All seven other rates are decoded, and only VOP A moves to CPLL/2. */
    RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
    f.x[K7DT_SEL_VOP_BUS]=0x60;f.x[K7DT_SEL_CHANNEL]=0x200;
    f.x[K7DT_SEL_VO0_BUS]=2;f.x[K7DT_SEL_TOP0]=0xcf3;
    f.x[K7DT_SEL_TOP1]=0x147;f.r[K7DR_SEL_FINAL]=0x207;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    assert(rr.vop_bus_planned && rr.vop_bus_touched && rr.vop_bus_verified);
    assert(rr.vop_bus_before==0x60 && rr.vop_bus_after==0x21 && f.bus_writes==1);
    assert(rr.before.bus_hz[2]==0 && rr.clocks.bus_hz[2]==500000000);
    assert(rr.clocks.bus_hz[0]==396000000 && rr.clocks.bus_hz[1]==198000000);
    assert(rr.clocks.bus_hz[3]==198000000 && rr.clocks.bus_hz[4]==50000000);
    assert(rr.clocks.bus_hz[5]==396000000 && rr.clocks.bus_hz[6]==198000000);
    assert(rr.clocks.bus_hz[7]==148500000);
    assert(f.x[K7DT_SEL_CHANNEL]==0x200 && f.x[K7DT_SEL_VO0_BUS]==2);
    assert(f.x[K7DT_SEL_TOP0]==0xcf3 && f.x[K7DT_SEL_TOP1]==0x147);
    /* Preserve H/P mux fields exactly, never turn another unknown source
     * into a fallback, and never reparent a known valid GPLL/CPLL root. */
    RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
    f.x[K7DT_SEL_VOP_BUS]=0x1460;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    assert(f.x[K7DT_SEL_VOP_BUS]==0x1421);
    for(i=0;i<8;i++) {
      int expected=i<3?-EBUSY:i==3?-ENOSYS:i==4?-EBUSY:
                   i==5?-EBUSY:i==6?-ENOSYS:-EACCES;
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.x[K7DT_SEL_VOP_BUS]=0x60;
      if(i==0) f.vp[0]=15;
      if(i==1) f.vp1=15;
      if(i==2) f.vp[1]=15;
      if(i==3) f.x[K7DT_SEL_CHANNEL]=3<<12;
      if(i==4) f.r[K7DR_CPLL0]=499;
      if(i==5) f.x[K7DT_SEL_VDEC]=1<<14;
      if(i==6) f.x[K7DT_SEL_VOP_BUS]=2<<5; /* unknown AUPLL */
      if(i==7) f.r[K7DR_GATE_VOP61]|=B(9);
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==expected);
      assert(!f.writes && !rr.attempted);
    }
    RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    assert(!f.bus_writes && !rr.vop_bus_planned);
    /* Failed or dropped bus update is not retried or restored to unmeasured
     * SPLL. No VPLL write follows it; cleanup only closes owned pixel leaves. */
    for(i=0;i<3;i++) {
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.x[K7DT_SEL_VOP_BUS]=0x60;
      if(i<2) f.fail_write=1;
      if(i==1) f.cleanup_fail=1;
      if(i==2) f.ignore_bus_write=1;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==(i<2?-EFAULT:-EIO));
      assert(rr.stage==K7DT_R_VOP_BUS_REPARENT && rr.vop_bus_touched);
      assert(!rr.vop_bus_verified && !f.pll_writes && f.bus_writes==1);
      assert(rr.cleanup_error==(i==1?-EACCES:0));
      assert(!(f.r[K7DR_GATE_VOP61]&(B(0)|B(9))));
    }
    /* Every later resource write fault retains the verified new live bus. */
    RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
    f.x[K7DT_SEL_VOP_BUS]=0x60;f.r[K7DR_SEL_FINAL]=0x207;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    writes=f.writes;
    for(i=2;i<=writes;i++) {
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.x[K7DT_SEL_VOP_BUS]=0x60;f.r[K7DR_SEL_FINAL]=0x207;f.fail_write=i;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EFAULT);
      assert(rr.vop_bus_verified && f.x[K7DT_SEL_VOP_BUS]==0x21);
      assert(f.bus_writes==1 && !(f.r[K7DR_GATE_VOP61]&(B(0)|B(9))));
    }
    /* Actual dormant VP2 boot state; also exercise VP0 and both sources.
     * Their source gate stays closed; VP2 divider/final mux are preserved. */
    for(i=1;i<4;i++) {
      unsigned mask=(i&1?B(12):0)|(i&2?B(10):0);
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.r[K7DR_GATE_VOP61]=0;
      f.r[K7DR_SEL_FINAL]=(i&1)?0x207:0;
      f.x[K7DT_SEL_VP0]=(i&2)?0x207:1;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
      assert(rr.vp_sources_claimed==mask && rr.vp_sources_touched);
      assert((f.r[K7DR_GATE_VOP61]&(B(10)|B(12)))==mask);
      assert(f.r[K7DR_SEL_FINAL]==((i&1)?0x207:0));
      assert(!rr.clocks.vp_vpll_ungated && !rr.clocks.foreign_vpll_ungated);
    }
    /* Non-owned consumers remain zero-write failures, even with a quiet VP2. */
    for(i=0;i<3;i++) {
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.r[K7DR_SEL_FINAL]=0x207;
      if(i==0) f.x[K7DT_SEL_VDEC]=1<<14;
      if(i==1) f.x[K7DT_SEL_EBC]=2<<12;
      if(i==2) f.x[K7DT_SEL_EBC]=2;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EBUSY);
      assert(!f.writes && !f.vp_reads);
    }
    /* Active, inaccessible, unreadable and changing VP cannot be adopted. */
    for(i=0;i<4;i++) {
      int expected=i==0?-EBUSY:i==1?-EACCES:i==2?-EIO:-EAGAIN;
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.r[K7DR_SEL_FINAL]=0x207;
      if(i==0) f.vp[1]=15;
      if(i==1) f.r[K7DR_GATE_CHANNEL]=3;
      if(i==2) f.vp_read_error=1;
      if(i==3) f.vp_unstable=1;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==expected);
      assert(!f.writes && !rr.attempted);
      if(i==1) assert(!f.vp_reads);
    }
    /* Gate write failure/readback failure fail before touching the PLL. */
    for(i=0;i<3;i++) {
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.r[K7DR_SEL_FINAL]=0x207;
      if(i<2) f.fail_write=1;
      if(i==1) f.cleanup_fail=1;
      if(i==2) f.ignore_vp_gate=1;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==(i<2?-EFAULT:-EIO));
      assert(rr.stage==K7DT_R_CLAIM_VP_SOURCES && !f.pll_writes && rr.attempted);
      assert(rr.cleanup_error==(i==0?0:i==1?-EACCES:-EIO));
      if(i==0) assert(f.r[K7DR_GATE_VOP61]&B(12));
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EALREADY);
    }
    /* A later failure must never reopen the adopted VP2 source. */
    RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
    f.r[K7DR_SEL_FINAL]=0x207;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    writes=f.writes;
    for(i=1;i<=writes;i++) {
      RESET(); memset(&rr,0,sizeof(rr));f.resources=1;
      f.r[K7DR_SEL_FINAL]=0x207;f.fail_write=i;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EFAULT);
      assert(!rr.ready && rr.attempted && rr.vp_sources_claimed==B(12));
      assert(f.r[K7DR_GATE_VOP61]&B(12));
    }
    RESET(); memset(&rr,0,sizeof(rr)); f.resources=1;
    /* Warm USB parent: no PMU power or SRAM write, no QoS write. */
    f.r[K7DR_PMU_REPAIR]|=B(16);f.x[K7DT_CHAIN]|=B(16);
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    assert(rr.ready && !f.pmu_writes && !f.mem_writes && f.qos_reads==15);
    assert(rr.domains_adopted==0x8800 && !rr.domains_powered);
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EALREADY);
    /* Cold VOP+VO0, memory initially off, forced ungate restored verbatim. */
    RESET(); memset(&rr,0,sizeof(rr)); f.resources=1;
    f.r[K7DR_PMU_REPAIR]=0;f.x[K7DT_CHAIN]=0;f.x[K7DT_PWR]=0x8800;
    f.x[K7DT_MEM]=0x8800;f.x[K7DT_UNGATE]=0x2400;
    f.r[K7DR_PMU_REQ]=f.r[K7DR_PMU_ACK]=f.r[K7DR_PMU_IDLE]=0x6800;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    assert(rr.domains_powered==0x8800 && f.pmu_writes==2 && !f.mem_writes);
    assert(f.x[K7DT_UNGATE]==0x2400 && rr.after.ready);
    writes=f.writes;
    /* Every write in complete cold initialization must fail-stop. */
    for(i=1;i<=writes;i++) {
      RESET(); memset(&rr,0,sizeof(rr)); f.resources=1;f.fail_write=i;
      f.r[K7DR_PMU_REPAIR]=0;f.x[K7DT_CHAIN]=0;f.x[K7DT_PWR]=0x8800;
      f.x[K7DT_MEM]=0x8800;f.x[K7DT_UNGATE]=0x2400;
      f.r[K7DR_PMU_REQ]=f.r[K7DR_PMU_ACK]=f.r[K7DR_PMU_IDLE]=0x6800;
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EFAULT);
      assert(rr.error==-EFAULT && !rr.ready && rr.attempted);
      assert(f.r[K7DR_GATE_VOP61]&B(11));assert(f.r[K7DR_GATE_DSI64]&B(6));
      assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EALREADY);
    }
    /* Parent on for USB, child VO0 off but SRAM on: reset child only. */
    RESET(); memset(&rr,0,sizeof(rr)); f.resources=1;
    f.r[K7DR_PMU_REPAIR]=B(11)|B(16);f.x[K7DT_CHAIN]=B(11)|B(16);
    f.x[K7DT_PWR]=B(15);f.x[K7DT_MEM]=0;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==0);
    assert(f.pmu_writes==1 && f.mem_writes==2 && rr.domains_powered==B(15));
    assert(rr.domains_adopted==B(11) && f.waits==60);
    /* Inconsistent parent-off/USB-on state: all preflight, zero writes. */
    RESET(); memset(&rr,0,sizeof(rr)); f.resources=1;
    f.r[K7DR_PMU_REPAIR]=B(16);f.x[K7DT_CHAIN]=B(16);
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EBUSY && !f.writes);
    /* Bounded failed power-on retains stage and leaves closed. */
    RESET(); memset(&rr,0,sizeof(rr)); f.resources=1;f.power_timeout=B(15);
    f.r[K7DR_PMU_REPAIR]=B(11)|B(16);f.x[K7DT_CHAIN]=B(11)|B(16);
    f.x[K7DT_PWR]=B(15);f.x[K7DT_MEM]=0;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-ETIMEDOUT);
    assert(rr.stage==K7DT_R_VO0_POWER && rr.power_stage==K7DT_P_CHAIN);
    assert(f.waits==10000 && !rr.ready && !f.qos_reads);
    RESET(); memset(&rr,0,sizeof(rr)); f.resources=1;f.fail_write=1;f.cleanup_fail=1;
    assert(k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&rr)==-EFAULT);
    assert(rr.cleanup_error==-EACCES);
  }
  puts("display transition tests passed (fake MMIO, no hardware)");
  return 0;
}
