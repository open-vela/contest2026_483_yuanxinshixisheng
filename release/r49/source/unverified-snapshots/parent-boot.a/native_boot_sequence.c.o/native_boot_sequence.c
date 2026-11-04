#include "native_boot_sequence.h"
#include <errno.h>
int vv_native_boot_run(const struct vv_native_boot_ops *o){
 int rc;unsigned waits=0;uint64_t start,now,last;
 if(!o||!o->preview_start||!o->preview_ready||!o->network_ready||!o->prepare_login_bind||!o->service_run||!o->stop_hold||!o->cancelled||!o->now_ms||!o->sleep_ms)return -EINVAL;
 if(o->cancelled(o->arg))return -ECANCELED;
 start=last=o->now_ms(o->arg);
 if((rc=o->preview_start(o->arg)))goto fail;
 for(;;){
  if(o->cancelled(o->arg)){rc=-ECANCELED;goto fail;}
  rc=o->preview_ready(o->arg);if(!rc)break;
  /* An absent camera has no active capture to time out. Keep the one-shot
   * startup pending, cancellable and motionless until it is plugged in.
   * Once enumeration succeeds, the existing first-frame deadline applies. */
  if(rc==-ENXIO){
   now=o->now_ms(o->arg);if(now<last){rc=-ETIMEDOUT;goto fail;}
   start=last=now;waits=0;o->sleep_ms(o->arg,100);continue;
  }
  if(rc!=-EAGAIN)goto fail;
  now=o->now_ms(o->arg);if(now<last||now-start>=30000||++waits>300){rc=-ETIMEDOUT;goto fail;}last=now;
  o->sleep_ms(o->arg,100);
 }
 for(;;){
  if(o->cancelled(o->arg)){rc=-ECANCELED;goto fail;}
  if((rc=o->preview_ready(o->arg)))goto fail;
  rc=o->network_ready(o->arg);if(!rc)break;if(rc!=-EAGAIN)goto fail;
  o->sleep_ms(o->arg,250);
 }
 if(o->cancelled(o->arg)){rc=-ECANCELED;goto fail;}
 if((rc=o->prepare_login_bind(o->arg)))goto fail;
 if(o->cancelled(o->arg)){rc=-ECANCELED;goto fail;}
 if((rc=o->preview_ready(o->arg))||(rc=o->network_ready(o->arg)))goto fail;
 rc=o->service_run(o->arg);
fail:o->stop_hold(o->arg);return rc;
}
