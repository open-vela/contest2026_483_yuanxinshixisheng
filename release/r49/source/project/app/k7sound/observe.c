#include "observe.h"
#include <stddef.h>
#include <string.h>
#include <errno.h>
int apo_observe(const struct apo_port *p,struct apo_result *r)
{
 static const unsigned bits[4]={2,3,5,11};
 uint64_t start,prev,now,target;uint32_t v,last=0;unsigned i,j;
 if(!r)return -EINVAL;
 memset(r,0,sizeof(*r));
 if(!p||!p->ticks||!p->ext||!p->frequency)return -EINVAL;
 target=((uint64_t)p->frequency+49)/50; /* ceil20ms, no overflow */
 start=prev=p->ticks(p->ctx);
 for(i=0;i<1000000;i++){
  now=p->ticks(p->ctx);
  if(now<prev)return -EIO;
  r->elapsed_ticks=now-start;
  if(now-prev>r->max_gap_ticks)r->max_gap_ticks=now-prev;
  if(now-start>=target){r->complete=1;return 0;}
  v=p->ext(p->ctx);
  if(!i)r->first=v;
  r->last=v;r->samples++;
  for(j=0;j<4;j++){
   if(v&(1u<<bits[j]))r->high[j]++;
   if(i&&((v^last)&(1u<<bits[j])))r->changes[j]++;
  }
  last=v;prev=now;
 }
 return -ETIMEDOUT;
}
