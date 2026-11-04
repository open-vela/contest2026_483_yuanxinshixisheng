/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform.h"
#define CRU ((uintptr_t)0x27200000u)
#define IOC ((uintptr_t)0x26040000u)
#define PMU ((uintptr_t)0x27380000u)
#define SAI ((uintptr_t)0x2a610000u)
/* source mux, frac M/N, SAI mux/div, frac gate, SAI gates, MCLKOUT,
 * GPIO4A low/high, GPIO4B low. hiword write masks except full fractional. */
static const uintptr_t addr[10]={CRU+0x334,CRU+0x330,CRU+0x3b8,
 CRU+0x804,CRU+0x820,CRU+0x824,IOC+0x4080,IOC+0x4084,IOC+0x4088,IOC+0x6144};
static const uint32_t mask[10]={3,0xffffffffu,0xfff,1u<<10,0x70,1u<<13,0xff00,0xf0f0,0xf000,0xc0};
static const uint32_t value[10]={3,0x00400177,0x100,0,0,0,0x1100,0x1010,0x1000,0};
static int rd(const struct sap_port*p,uintptr_t a,uint32_t*v){return p->read(p->ctx,a,v)?-5:0;}
static int wr(const struct sap_port*p,uintptr_t a,uint32_t v){return p->write(p->ctx,a,v)?-5:0;}
static int hw(const struct sap_port*p,uintptr_t a,uint32_t m,uint32_t v)
{return wr(p,a,(m<<16)|(v&m));}
static int poll(const struct sap_port*p,uintptr_t a,uint32_t m,uint32_t want,uint32_t*out)
{
 uint64_t start=p->us(p->ctx),now;unsigned i;
 for(i=0;i<100000;i++){
  if(rd(p,a,out))return -5;
  now=p->us(p->ctx);if(now<start||now-start>=10000)return -110;
  if((*out&m)==want)return 0;
 }return -110;
}
int sap_setup(const struct sap_port*p,struct sap_state*s,int exclusive)
{
 unsigned i;uint32_t v;int rc=-5;
 if(!p||!p->read||!p->write||!p->us||!s)return -22;
 if(s->attempted)return -16;
 if(!exclusive)return -38;
 s->attempted=1;
 for(i=0;i<10;i++)if(rd(p,addr[i],&s->old[i]))return -5;
 s->saved=1;s->held=1;s->modified=1;
 /* PMU domain force ungate 0x140+req offset4, bit0. */
 if(hw(p,PMU+0x144,1,1))return -5;
 if(hw(p,PMU+0x210,1u<<8,0))return -5;
 rc=poll(p,PMU+0x570,1u<<8,1u<<8,&s->repair);if(rc)return rc;
 if(hw(p,PMU+0x114,1,0))return -5;
 rc=poll(p,PMU+0x120,1u<<16,0,&s->ack);if(rc)return rc;
 rc=poll(p,PMU+0x128,1u<<16,0,&s->idle);if(rc)return rc;
 /* clk-out domain is AUDIO: first access only after power/de-idle proof. */
 if(rd(p,IOC+0x6400,&s->mclkout_before))return -5;
 /* Quiescent exclusive ownership required: gate endpoints before reclock. */
 if(hw(p,CRU+0x824,1u<<13,1u<<13)||hw(p,CRU+0x820,0x70,0x70)||
    hw(p,CRU+0x804,1u<<10,1u<<10))return -5;
 for(i=0;i<10;i++){
  rc=i==1?wr(p,addr[i],value[i]):hw(p,addr[i],mask[i],value[i]);
  if(rc)return rc;
  if(rd(p,addr[i],&v))return -5;
  if((v&mask[i])!=value[i])return -5;
 }
 /* Independent to-IO gate from mclkout_sai1 DT, bit1 set-to-disable. */
 if(hw(p,IOC+0x6400,2,0))return -5;
 if(rd(p,IOC+0x6400,&s->mclkout_after)||(s->mclkout_after&2))return -5;
 /* Force-ungate clear mirrors Linux after successful power transition. */
 if(hw(p,PMU+0x144,1,0))return -5;
 if(rd(p,CRU+0x3b8,&s->clk46)||rd(p,CRU+0x330,&s->fraction))return -5;
 /* First SAI read only now. Root supplies reset/HCLK parent prerequisites. */
 if(rd(p,SAI+0x70,&s->version))return -5;
 s->ready=1;return 0;
}
int sap_cleanup(const struct sap_port*p,struct sap_state*s,int quiescent)
{
 unsigned i;uint32_t v;
 if(!p||!p->read||!p->write||!s)return -22;
 if(!s->modified)return 0;
 if(!s->ready||!quiescent)return -16;
 /* No SAI reads here: rely on caller's real stop proof. */
 if(hw(p,IOC+0x6400,2,2))return -5;
 if(hw(p,CRU+0x824,1u<<13,1u<<13)||hw(p,CRU+0x820,0x70,0x70)||
    hw(p,CRU+0x804,1u<<10,1u<<10))return -5;
 /* Restore parent/div/pins before original gates. */
 for(i=0;i<10;i++){
  if(i>=3&&i<=5)continue;
  if(i==1){if(wr(p,addr[i],s->old[i]))return -5;}
  else if(hw(p,addr[i],mask[i],s->old[i]))return -5;
 }
 for(i=3;i<=5;i++)if(hw(p,addr[i],mask[i],s->old[i]))return -5;
 for(i=0;i<10;i++)if(rd(p,addr[i],&v)||(v&mask[i])!=(s->old[i]&mask[i]))return -5;
 if(hw(p,IOC+0x6400,2,s->mclkout_before))return -5;
 if(rd(p,IOC+0x6400,&v)||(v&2)!=(s->mclkout_before&2))return -5;
 s->ready=s->held=s->modified=0;return 0;
}
