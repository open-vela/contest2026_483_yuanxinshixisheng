/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "reset_probe.h"
static int delay10(const struct sr_port*p)
{
 uint64_t start=p->us(p->ctx),now;unsigned i;
 for(i=0;i<10000;i++){now=p->us(p->ctx);if(now<start)return -110;if(now-start>=10)return 0;}
 return -110;
}
static int snapshot(const struct sr_port*p,uint32_t *v)
{
 static const uint32_t offsets[5]={0x70,0,8,0x38,0x24};unsigned i;
 for(i=0;i<5;i++)if(p->read_sai(p->ctx,offsets[i],&v[i]))return -5;
 return 0;
}
int sr_once(const struct sr_port*p,struct sr_result*r,int allowed)
{
 uint32_t v;unsigned i;int asserted,rc;
 if(!p||!r||!p->read_sai||!p->set_reset||!p->get_reset||!p->us)return -22;
 if(r->attempted)return -16;
 if(!allowed)return r->rc=-38;
 r->attempted=1;
 for(i=0;i<2;i++){
  if(p->get_reset(p->ctx,(enum sr_domain)i,&asserted))return r->rc=-5;
  if(asserted)return r->rc=-16;
 }
 if(snapshot(p,r->before))return r->rc=-5;
 if(r->before[0]!=0x23073576)return r->rc=-38;
 if(r->before[4]&0x01000100)return r->rc=-16;
 if(p->read_sai(p->ctx,0x10,&v))return r->rc=-5;
 if(v&15)return r->rc=-16;
 if(p->read_sai(p->ctx,0x6c,&v))return r->rc=-5;
 if((v&14)!=14)return r->rc=-16;
 for(i=0;i<2;i++){
  if(p->read_sai(p->ctx,0x1c+4*i,&v))return r->rc=-5;
  if(v&0xffffff)return r->rc=-16;
 }
 r->held=r->needs_reapply=1;
 for(i=0;i<4;i++){
  enum sr_domain domain=i<2?SR_H:SR_M;int want=!(i&1);
  if(p->set_reset(p->ctx,domain,want))return r->rc=-5;
  if(p->get_reset(p->ctx,domain,&asserted)||asserted!=want)return r->rc=-5;
  r->steps++;
  rc=delay10(p);if(rc)return r->rc=rc;
 }
 if(snapshot(p,r->after))return r->rc=-5;
 if(r->after[0]!=0x23073576)return r->rc=-71;
 r->completed=1;r->held=0;return r->rc=0;
}
