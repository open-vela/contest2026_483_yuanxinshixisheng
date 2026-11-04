/* SPDX-License-Identifier: GPL-2.0-only
 * Register sequence derived from frozen vendor i2c-rk3x.c.
 * Original driver: David Wu, Max Schwarz (see frozen source copyrights). */
#include "live.h"
#include "i2c3_timing.h"
#include <errno.h>
#define BASE ((uintptr_t)0x2ac60000u)
#define LIMIT 200000u
static uint32_t rd(struct i3_live *s,unsigned o){return s->p.read(s->p.ctx,BASE+o);}
static void wr(struct i3_live *s,unsigned o,uint32_t v){s->p.write(s->p.ctx,BASE+o,v);}
static int waitbit(struct i3_live *s,uint32_t bit,uint32_t limit,bool nak)
{
 uint64_t start=s->p.us(s->p.ctx),prev=start;unsigned i;
 for(i=0;i<LIMIT;i++){
  uint64_t now=s->p.us(s->p.ctx);uint32_t v;
  if(now<prev)return -EIO;
  prev=now;if(now-start>=limit)return -ETIMEDOUT;
  v=rd(s,0x1c);if(nak&&(v&64u))return -ENXIO;
  if(v&bit)return 0;
 }
 return -ETIMEDOUT;
}
int i3_init(struct i3_live *s,const struct i3_port *p,bool prepared)
{
 if(!s||!p||!p->read||!p->write||!p->us||!p->lock||!p->unlock||!p->lines)return -EINVAL;
 if(s->ready||s->held||s->active||s->poisoned)return -EBUSY;
 if(!prepared)return -EAGAIN;
 s->p=*p;s->ready=true;return 0;
}
static int acquire(struct i3_live *s)
{
 int r;if(!s||!s->ready)return -ENODEV;
 if(s->held||s->poisoned)return -EBUSY;
 r=s->p.lock(s->p.ctx);if(r)return r;s->held=true;
 if((rd(s,0)&0x19u)||rd(s,0x18)||s->p.lines(s->p.ctx)!=1){
  s->p.unlock(s->p.ctx);s->held=false;return -EBUSY;
 }
 return 0;
}
int i3_timing(struct i3_live *s)
{
 struct k7_i2c_timing_request q={24000000,100000,1000,300,true};
 struct k7_i2c_timing t;int r=k7_i2c3_timing(&q,&t);if(r)return r;
 r=acquire(s);if(r)return r;
 wr(s,0,(rd(s,0)&~0xff00u)|t.tuning);wr(s,4,t.clkdiv);
 r=((rd(s,0)&0xff00u)!=t.tuning||rd(s,4)!=t.clkdiv)?-EIO:0;
 if(r)s->poisoned=true;else s->timed=true;
 s->p.unlock(s->p.ctx);s->held=false;return r;
}
int i3_cleanup(struct i3_live *s)
{
 int r;if(!s||!s->ready)return -ENODEV;if(!s->held)return 0;
 if(s->active){
  /* Clear stale STOP only; never accept a previous transaction's STOP. */
  wr(s,0x1c,32);wr(s,0x18,32);wr(s,0,(rd(s,0)|16u)&~8u);
  r=waitbit(s,32,10000,false);
  if(r){s->poisoned=true;return r;}
  wr(s,0x1c,32);wr(s,0x18,0);wr(s,0,s->tuning);
  s->active=false;
 }
 if(s->p.lines(s->p.ctx)!=1){s->poisoned=true;return -EBUSY;}
 s->p.unlock(s->p.ctx);s->held=false;return 0;
}
int i3_xfer(struct i3_live *s,bool reading,uint8_t reg,uint8_t *value,uint32_t timeout)
{
 uint32_t con;uint8_t result=0;int r,c;
 if(!value||!timeout||timeout>100000)return -EINVAL;
 if(!s||!s->timed)return -EAGAIN;
 r=acquire(s);if(r)return r;
 con=rd(s,0);s->tuning=con&0xff00u;s->active=true;
 wr(s,0x1c,0xff);
 if(((con>>16)&0x1ff)>=5)wr(s,0x228,0); /* vendor automatic STOP off */
 if(reading){
  wr(s,8,0x01000020);wr(s,12,0x01000000u|reg);
  wr(s,0x18,8|64);wr(s,0,s->tuning|1|2|8|64);
  wr(s,0,s->tuning|1|2|8|64|32);wr(s,0x14,1);
 }else{
  wr(s,0x18,4|64);wr(s,0x100,0x20u|((uint32_t)reg<<8)|((uint32_t)*value<<16));
  wr(s,0,s->tuning|1|8|64);wr(s,0x10,3);
 }
 r=waitbit(s,reading?8:4,timeout,true);
 if(!r){
  wr(s,0x1c,reading?(8|16):4);
  if(reading)result=(uint8_t)rd(s,0x200);
 }else{s->poisoned=true;wr(s,0x1c,64);}
 c=i3_cleanup(s);if(!r)r=c;
 if(r)s->poisoned=true;else if(reading)*value=result;
 return r;
}
int i3_gpio_lines(uint32_t version,uint32_t ext)
{
 if(version!=0x01000c2b&&version!=0x0101157c&&version!=0x010219c8)return -ENODEV;
 return (ext&0x3000u)==0x3000u;
}
