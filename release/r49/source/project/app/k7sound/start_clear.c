/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "start_clear.h"
static int rd(const struct sc_port*p,uint32_t o,uint32_t*v){return p->read(p->ctx,o,v)?-5:0;}
int sc_once(const struct sc_port*p,struct sc_result*r,int trusted)
{
 unsigned i;uint64_t now;
 if(!p||!r||!p->read||!p->write||!p->us)return -22;
 if(r->attempted)return -16;
 if(!trusted)return r->rc=-38;
 r->attempted=1;
 if(rd(p,0x70,&r->version)||rd(p,0x10,&r->xfer)||rd(p,0x24,&r->dmacr)||
    rd(p,0x6c,&r->status)||rd(p,0x14,&r->clr_before))return r->rc=-5;
 if(r->version!=0x23073576)return r->rc=-38;
 if((r->xfer&15)||(r->dmacr&0x01000100)||(r->status&14)!=14||r->clr_before)
   return r->rc=-16;
 for(i=0;i<2;i++)if(rd(p,0x1c+4*i,&r->fifo_before[i]))return r->rc=-5;
 r->held=r->needs_reapply=1;r->start_us=p->us(p->ctx);
 /* W1C command bits: no read/modify side effects, no forced-clear bit. */
 if(p->write(p->ctx,0x14,3))return r->rc=-5;
 for(r->polls=0;r->polls<100000;r->polls++){
  if(rd(p,0x14,&r->clr_after))return r->rc=-5;
  now=p->us(p->ctx);r->end_us=now;
  if(now<r->start_us||now-r->start_us>=1000)return r->rc=-110;
  if(!(r->clr_after&3))break;
 }
 if(r->polls==100000)return r->rc=-110;
 for(i=0;i<2;i++){
  if(rd(p,0x1c+4*i,&r->fifo_after[i]))return r->rc=-5;
  if(r->fifo_after[i]&0xffffff)return r->rc=-71;
 }
 r->completed=1;r->held=0;return r->rc=0;
}
