/* SPDX-License-Identifier: Apache-2.0 */
#include "vv_microcrystal.h"
#include <errno.h>
#include <string.h>

static int guard(const struct vv_microcrystal_ops *o) {
  if(o->cancelled(o->arg)) return -ECANCELED;
  if(!o->connected(o->arg)) return -ENOTCONN;
  return 0;
}
static int wait_ms(const struct vv_microcrystal_ops *o,unsigned ms) {
  uint64_t start=o->now_ms(o->arg),previous=start;
  /* Bound both wall-clock progress and iteration count. */
  for(unsigned i=0;i<=ms/10+2;i++) {
    int rc=guard(o);if(rc)return rc;
    uint64_t now=o->now_ms(o->arg);
    if(now<previous)return -EIO;
    if(now-start>=ms)return 0;
    previous=now;unsigned remain=(unsigned)(ms-(now-start));
    o->sleep_ms(o->arg,remain<10?remain:10);
  }
  return -ETIMEDOUT;
}
static int write_level(const struct vv_microcrystal_ops *o,char value,
                       struct vv_microcrystal_result *r) {
  int rc=guard(o);if(rc)return rc;
  /* Even a transport failure may have occurred after remote execution. */
  r->output_may_be_active=true;
  return o->write_ascii(o->arg,value);
}
int vv_microcrystal_run(const struct vv_microcrystal_ops *o,bool genuine,
                       bool playback,struct vv_microcrystal_result *r) {
  if(!r)return -EINVAL;
  memset(r,0,sizeof(*r));
  if(!o||!o->now_ms||!o->sleep_ms||!o->cancelled||!o->connect||
     !o->connected||!o->write_ascii||!o->stop_confirmed||!o->disconnect)
    return r->error=-EINVAL;
  if(!genuine||!playback)return r->error=-EPERM;
  if(o->cancelled(o->arg))return r->error=-ECANCELED;
  uint64_t start=o->now_ms(o->arg);
  int rc=o->connect(o->arg);
  if(!rc)rc=guard(o);
  /* Initial stop is mandatory before the first energizing command. */
  if(!rc)rc=o->write_ascii(o->arg,'0');
  if(!rc&&!o->stop_confirmed(o->arg))rc=-EAGAIN;
  if(!rc)rc=write_level(o,'1',r);
  if(!rc)rc=wait_ms(o,300);
  static const char levels[]={'1','3','2'};
  static const unsigned durations[]={2000,2000,3000};
  for(unsigned i=0;i<3&&!rc;i++) {
    rc=write_level(o,levels[i],r);
    if(!rc)rc=wait_ms(o,durations[i]);
    if(!rc)r->steps_completed++;
  }
  /* Cancellation forbids further levels but must not suppress stop. */
  if(o->connected(o->arg)) {
    r->stop_error=o->write_ascii(o->arg,'0');
    r->stop_written=!r->stop_error;
    r->stopped_confirmed=r->stop_written&&o->stop_confirmed(o->arg);
    if(r->stopped_confirmed)r->output_may_be_active=false;
  } else if(r->output_may_be_active)r->stop_error=-ENOTCONN;
  if(!rc&&r->stop_error)rc=r->stop_error;
  if(!rc&&!r->stopped_confirmed)rc=-EAGAIN;
  if(!rc&&o->cancelled(o->arg))rc=-ECANCELED;
  r->resume_allowed=!rc&&r->steps_completed==3&&r->stopped_confirmed;
  o->disconnect(o->arg);
  uint64_t end=o->now_ms(o->arg);
  if(end<start){rc=-EIO;r->resume_allowed=false;}else r->elapsed_ms=end-start;
  r->error=rc;return rc;
}
