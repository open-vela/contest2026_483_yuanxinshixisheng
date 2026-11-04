#include "vv_native_mode.h"
#include <errno.h>
#include <string.h>
int vv_native_mode_init(struct vv_native_mode *s,const struct vv_native_mode_ops *o){if(!s||!o||!o->request||!o->status||!o->halt||!o->cancel_work||!o->quiet||!o->sleep_ms||!o->now_ms)return -EINVAL;memset(s,0,sizeof(*s));s->ops=*o;atomic_init(&s->busy,false);atomic_init(&s->epoch,1);return 0;}
unsigned vv_native_mode_epoch(const struct vv_native_mode *s){return s?atomic_load(&s->epoch):0;}
void vv_native_mode_invalidate(struct vv_native_mode *s){if(s)atomic_fetch_add(&s->epoch,1);}
static void stop(struct vv_native_mode *s){s->assessment_active=false;s->assessment_epoch=0;s->ops.cancel_work(s->ops.arg);s->ops.halt(s->ops.arg);}
int vv_native_mode_apply(struct vv_native_mode *s,enum k7_voice_intent intent,unsigned expected){
 if(!s)return -EINVAL;
 bool idle=false;if(!atomic_compare_exchange_strong(&s->busy,&idle,true))return -EBUSY;
 int rc=0;
 if(expected!=atomic_load(&s->epoch)){rc=-ECANCELED;goto done;}
 if(intent==K7_VOICE_STOP){vv_native_mode_invalidate(s);stop(s);goto done;}
 if(intent==K7_VOICE_NONE)goto done;
 if(intent==K7_VOICE_QUIET){s->ops.quiet(s->ops.arg);goto done;}
 if(intent!=K7_VOICE_TRACK&&intent!=K7_VOICE_PHOTO){rc=-EINVAL;goto done;}
 if(s->assessment_active){rc=-EBUSY;goto done;}
 unsigned mode=intent==K7_VOICE_TRACK?1:2;
 if(mode==2){
  rc=s->ops.status(s->ops.arg,1);
  /* Tracking may still be awaiting the pipeline ACK.  No photo request has
   * been made, so leave the transition intact and keep listening.  A changed
   * mode (HOLD/STOP) and genuine status failures retain their own errors. */
  if(rc==-EAGAIN)rc=expected==atomic_load(&s->epoch)?-EBUSY:-ECANCELED;
  if(rc)goto done;
 }
 if(expected!=atomic_load(&s->epoch)){rc=-ECANCELED;goto failed;}
 uint64_t began=s->ops.now_ms(s->ops.arg),previous=began;
 bool pending_ack=false,ack_timeout=false,clock_reversed=false;
 rc=s->ops.request(s->ops.arg,mode);if(rc)goto failed;
 for(unsigned i=0;i<=300;i++){
  if(expected!=atomic_load(&s->epoch)){rc=-ECANCELED;goto failed;}
  uint64_t now=s->ops.now_ms(s->ops.arg);
  if(now<previous)clock_reversed=true;
  previous=now;
  if(now<began||now-began>=3000){
   ack_timeout=pending_ack&&!clock_reversed&&now>=began&&now-began>=3000;
   rc=-ETIMEDOUT;break;
  }
  rc=s->ops.status(s->ops.arg,mode);pending_ack=rc==-EAGAIN;if(rc!=-EAGAIN)break;
  if(i==300){rc=-ETIMEDOUT;break;}s->ops.sleep_ms(s->ops.arg,10);
 }
 uint64_t finished=s->ops.now_ms(s->ops.arg);
 if(finished<previous)clock_reversed=true;
 if(finished<began||finished-began>=3000){
  if(rc==-EAGAIN)ack_timeout=pending_ack&&!clock_reversed&&finished>=began&&finished-began>=3000;
  rc=-ETIMEDOUT;
 }
 if(expected!=atomic_load(&s->epoch))rc=-ECANCELED;
 /* Only a real pending TRACK ACK budget expiry is recoverable upstream.
  * The loop-count bound, reversed clock and callback errors stay ordinary errors. */
 if(rc==-ETIMEDOUT&&mode==1&&ack_timeout&&!clock_reversed)rc=VV_NATIVE_TRACK_ACK_TIMEOUT;
 if(rc)goto failed;
 if(mode==2){s->assessment_active=true;s->assessment_epoch=expected;}
 goto done;
 failed:vv_native_mode_invalidate(s);stop(s);
 done:atomic_store(&s->busy,false);return rc;
}
int vv_native_mode_text(struct vv_native_mode *s,const char *t,size_t n,bool verified,bool completed,unsigned epoch){return vv_native_mode_apply(s,k7_voice_completed_text(t,n,verified,completed),epoch);}

/* Release only; caller separately uses pipeline revision guarded report resume. */
int vv_native_mode_assessment_done(struct vv_native_mode *s,unsigned expected){
 if(!s)return -EINVAL;
 bool idle=false;if(!atomic_compare_exchange_strong(&s->busy,&idle,true))return -EBUSY;
 int rc=expected!=atomic_load(&s->epoch)?-ECANCELED:!s->assessment_active||s->assessment_epoch!=expected?-EINVAL:0;
 if(!rc){s->assessment_active=false;s->assessment_epoch=0;}
 atomic_store(&s->busy,false);return rc;
}
