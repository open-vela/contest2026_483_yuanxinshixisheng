/* SPDX-License-Identifier: Apache-2.0 */
#include "rk3576_panel_board.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct reg { uintptr_t address; uint32_t value; };
struct event { char kind; uintptr_t a; uint32_t v; };
struct fake {
  struct reg regs[24];
  struct event events[600];
  unsigned count, total, fail_at, stuck, corrupt_at, read_count, writes;
};
static uint32_t *slot(struct fake *f,uintptr_t a)
{
  unsigned i;
  for(i=0;i<24;i++) {
    if(f->regs[i].address==a) return &f->regs[i].value;
    if(!f->regs[i].address) { f->regs[i].address=a; return &f->regs[i].value; }
  }
  assert(0); return 0;
}
static int event(struct fake *f,char k,uintptr_t a,uint32_t v)
{
  assert(f->count<600);
  f->events[f->count++]=(struct event){k,a,v};
  f->total++;
  return f->total==f->fail_at ? -EIO : 0;
}
static int rd(void *arg,uintptr_t a,uint32_t *v)
{
  struct fake *f=arg;
  int r=event(f,'r',a,0);
  f->read_count++;
  if(r) return r;
  *v=*slot(f,a);
  if(f->total==f->corrupt_at) *v^=0xffff;
  return 0;
}
static int wr(void *arg,uintptr_t a,uint32_t v)
{
  struct fake *f=arg;
  int r=event(f,'w',a,v);
  uint32_t *p;
  f->writes++;
  if(r) return r;
  p=slot(f,a);
  if(a==0x2add1010 || a==0x2add1014) *p=v;
  else *p=(*p & ~(v>>16)) | (v & (v>>16));
  /* V4 update is consumed when local clock/run are enabled. */
  if(a==0x2add1004 && !f->stuck && (*p&3)==3) *p&=~4u;
  return 0;
}
static int waitms(void *arg,unsigned ms) { return event(arg,'d',0,ms); }
static struct k7pb_io init(struct fake *f)
{
  memset(f,0,sizeof(*f));
  *slot(f,0x27320078)=0x010219c8;
  *slot(f,0x2add1000)=0x04000016;
  /* Neighbor fields are deliberately nonzero; B7 is live radio I2C. */
  *slot(f,0x26042000)=0x9012;
  *slot(f,0x26042008)=0x500a;
  *slot(f,0x26040000)=0xb07a;
  *slot(f,0x27200840)=0xffff;
  *slot(f,0x2722081c)=0xffff;
  *slot(f,0x2720041c)=0xaaaa;
  return (struct k7pb_io){f,rd,wr,waitms};
}
static void safe(struct fake *f)
{
  assert(!(*slot(f,0x27320000)&((1u<<13)|(1u<<2))));
  assert(!(*slot(f,0x27320004)&(1u<<6)));
  assert((*slot(f,0x26042000)&0xf0)==0);
  assert((*slot(f,0x26042000)&0xff0f)==0x9002);
}
static unsigned findevent(struct fake *f,char k,uintptr_t a,uint32_t v)
{
  unsigned i;
  for(i=0;i<f->count;i++)
    if(f->events[i].kind==k && f->events[i].a==a && f->events[i].v==v) return i;
  assert(0); return 0;
}
static void success(void)
{
  struct fake f; struct k7pb_state s={0}; struct k7pb_io p=init(&f);
  unsigned i,power,release,delays=0;
  assert(k7pb_prepare(&p,&s,K7PB_ALL_PROOFS)==0);
  assert(s.phase==K7PB_POWERED);
  assert(!(*slot(&f,0x27320000)&4));
  assert(*slot(&f,0x27320004)&64);
  assert(k7pb_reset(&p,&s)==0);
  power=findevent(&f,'w',0x27320004,0x00400040);
  release=findevent(&f,'w',0x27320000,0x00040004);
  for(i=power+1;i<release;i++) if(f.events[i].kind=='d') {
    assert(f.events[i].v==120); delays++;
  }
  assert(delays==2); /* supply settle and asserted-reset minimum */
  assert(f.events[release+2].kind=='d' && f.events[release+2].v==120);
  assert(k7pb_backlight(&p,&s,25,1)==0);
  assert(*slot(&f,0x2add1010)==300 && *slot(&f,0x2add1014)==75);
  assert((*slot(&f,0x26042000)&0xf0)==0xc0);
  assert((*slot(&f,0x26042000)&0xff0f)==0x9002);
  assert((*slot(&f,0x2720041c)&~0x300u)==(0xaaaau&~0x300u));
  assert((*slot(&f,0x27200840)&~0xc00u)==(0xffffu&~0xc00u));
  assert(k7pb_backlight(&p,&s,0,0)==0);
  assert(s.phase==K7PB_RESET_DONE && (*slot(&f,0x2add1004)&3)==0);
  assert(k7pb_off(&p,&s)==0); safe(&f);
}
static void denied(void)
{
  struct fake f; struct k7pb_state s={0}; struct k7pb_io p=init(&f);
  unsigned i;
  for(i=0;i<K7PB_ALL_PROOFS;i++) assert(k7pb_prepare(&p,&s,i)==-EPERM);
  assert(k7pb_reset(&p,&s)==-EPERM);
  assert(k7pb_backlight(&p,&s,10,1)==-EPERM);
  assert(f.count==0);
  assert(k7pb_prepare(&p,&s,K7PB_ALL_PROOFS)==0);
  i=f.count;
  assert(k7pb_prepare(&p,&s,K7PB_ALL_PROOFS)==-EBUSY);
  assert(k7pb_backlight(&p,&s,10,1)==-EPERM);
  assert(f.count==i);
  assert(k7pb_reset(&p,&s)==0); i=f.count;
  assert(k7pb_backlight(&p,&s,101,1)==-EINVAL);
  assert(k7pb_backlight(&p,&s,25,0)==-EPERM);
  assert(f.count==i);
}
static void failed(void)
{
  struct fake base; struct k7pb_state bs={0}; struct k7pb_io bp=init(&base);
  unsigned i,operations;
  assert(k7pb_prepare(&bp,&bs,K7PB_ALL_PROOFS)==0);
  assert(k7pb_reset(&bp,&bs)==0);
  assert(k7pb_backlight(&bp,&bs,30,1)==0); operations=base.total;
  for(i=1;i<=operations;i++) {
    struct fake f; struct k7pb_state s={0}; struct k7pb_io p=init(&f);
    int r;
    f.fail_at=i;
    r=k7pb_prepare(&p,&s,K7PB_ALL_PROOFS);
    if(!r) r=k7pb_reset(&p,&s);
    if(!r) r=k7pb_backlight(&p,&s,30,1);
    assert(r==-EIO && s.phase==K7PB_FAULT && s.primary_error==-EIO);
    if(s.touched) { assert(s.cleanup_error==0); safe(&f); }
    else assert(f.count<=3); /* Never reads GPIO when its gate failed. */
  }
  printf("injected failure at all %u read/write/delay steps: PASS\n",operations);
}
static void incompatible(void)
{
  struct fake f; struct k7pb_state s={0}; struct k7pb_io p=init(&f);
  unsigned i;
  *slot(&f,0x2add1000)=0x03000016;
  assert(k7pb_prepare(&p,&s,K7PB_ALL_PROOFS)==-ENODEV); safe(&f);
  for(i=0;i<f.count;i++) assert(!(f.events[i].kind=='w' &&
    f.events[i].a>=0x2add1000 && f.events[i].a<0x2add2000));
  p=init(&f); memset(&s,0,sizeof(s)); *slot(&f,0x2add1004)=16;
  assert(k7pb_prepare(&p,&s,K7PB_ALL_PROOFS)==-EBUSY); safe(&f);
  assert(*slot(&f,0x2add1004)==16);
}
static void update_timeout_and_cleanup_error(void)
{
  struct fake f; struct k7pb_state s={0}; struct k7pb_io p=init(&f);
  assert(!k7pb_prepare(&p,&s,K7PB_ALL_PROOFS)); assert(!k7pb_reset(&p,&s));
  f.stuck=1;
  assert(k7pb_backlight(&p,&s,40,1)==-ETIMEDOUT); safe(&f);
  assert(s.phase==K7PB_FAULT && !s.cleanup_error);
  /* Cleanup itself failing is never labelled OFF/safe. */
  f.fail_at=f.total+1;
  assert(k7pb_off(&p,&s)==-EIO && s.cleanup_error==-EIO && s.phase==K7PB_FAULT);
  f.fail_at=0;
  assert(k7pb_off(&p,&s)==0 && s.phase==K7PB_OFF);
  assert(s.cleanup_error==-EIO && !s.last_cleanup_error);
  assert(s.primary_error==-ETIMEDOUT);
}
static void dropped_write(void)
{
  struct fake f; struct k7pb_state s={0}; struct k7pb_io p=init(&f);
  /* Gate write returns success, corrupt readback prevents GPIO access. */
  f.corrupt_at=2;
  assert(k7pb_prepare(&p,&s,K7PB_ALL_PROOFS)==-EIO);
  assert(f.count==2 && !s.touched);
}
int main(void)
{
  success(); denied(); failed(); incompatible();
  update_timeout_and_cleanup_error(); dropped_write();
  puts("panel board sequence, ownership, masks, readback, safety tests: PASS");
  return 0;
}
