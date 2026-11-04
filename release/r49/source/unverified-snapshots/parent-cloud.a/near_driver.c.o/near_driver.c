#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "near_driver.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define STORED 256u
struct stored {struct k7_voice_frame raw,clean;bool tail;};
struct near_driver {
 pthread_mutex_t lock;
 struct nd_ops ops;struct nd_calibration calibration;struct nd_evidence onset;
 struct vv_barge_route *route;struct stored frames[STORED];
 unsigned count,head;uint64_t first,next,last_revision,last_evidence,tail_first;
 bool installed,closing,pinned,tail_forwarded;
};
static enum vv_barge_material clean_material(struct near_driver*d){return d->calibration.experimental_policy?VV_BARGE_EXPERIMENTAL_RESIDUAL:VV_BARGE_QUALIFIED_CLEAN;}
static enum vv_barge_material tail_material(struct near_driver*d){return d->calibration.experimental_policy?VV_BARGE_EXPERIMENTAL_TAIL:VV_BARGE_QUALIFIED_MUTED_CONTINUOUS;}
static bool context_equal(const struct vv_confirmed_wake_context*a,const struct vv_confirmed_wake_context*b){
#define E(n) a->n==b->n
 return E(owner_epoch)&&E(model_epoch)&&E(clean_epoch)&&E(alignment_epoch)&&E(scope_incarnation)&&E(lease_serial)&&E(reply_id)&&E(input_epoch)&&E(mode_epoch)&&E(asr_epoch)&&E(tx_generation);
#undef E
}
static int pin(struct near_driver*d,struct vv_barge_observation*out){
 if(!d->installed||d->closing)return -ENOTSUP;
 if(!d->ops.pin||!d->ops.unpin)return -ENOTSUP;
 int rc=d->ops.pin(d->ops.arg,&d->calibration,out);if(rc)return rc;
 if(!out->rx_live||out->photo_busy||out->owner_epoch!=d->calibration.context.owner_epoch||
 out->input_epoch!=d->calibration.context.input_epoch||out->mode_epoch!=d->calibration.context.mode_epoch||
 out->live_tx_generation!=d->calibration.context.tx_generation||out->now_ms>d->calibration.expires_ms){d->ops.unpin(d->ops.arg);return -ESTALE;}
 return 0;
}
static int range(struct near_driver*d,uint64_t first,uint64_t next,bool tail){
 if(first>=next||first<d->calibration.first||next>d->calibration.next)return -ERANGE;
 (void)tail; /* Every physical frame in this contiguous interval was checked
  * under the independent pin. PCM cache may roll; this proof interval cannot. */
 return d->count&&first>=d->first&&next<=d->next?0:-EAGAIN;
}
static int authority_enter(void*arg,const struct vv_confirmed_wake_context*c,enum vv_barge_check why,enum vv_barge_material material,uint64_t first,uint64_t next,struct vv_barge_observation*out){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);
 int rc=!context_equal(c,&d->calibration.context)?-ESTALE:pin(d,out);
 if(rc){pthread_mutex_unlock(&d->lock);return rc;}
 bool tail=material==tail_material(d);
 rc=(material!=clean_material(d)&&material!=tail_material(d))?-ENOTSUP:range(d,first,next,false);
 if(!rc&&tail&&!d->calibration.tail_supported)rc=-ENOTSUP;
 if(!rc&&tail&&next-first==320&&(d->tail_first==UINT64_MAX||first<d->tail_first))rc=-ESTALE;
 if(!rc&&(why==VV_BARGE_CHECK_CLAIM||why==VV_BARGE_CHECK_CAPTURE)&&
   (d->tail_first==UINT64_MAX||next<=d->tail_first))rc=-EAGAIN;
 if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL)){
  if(!d->calibration.tail_supported||!d->onset.support.evidence_id||!d->ops.verify_onset)rc=-ENOTSUP;
  else rc=d->ops.verify_onset(d->ops.arg,&d->calibration,&d->onset);
 }
 if(rc){d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);return rc;}
 d->pinned=true;return 0;
}
static void authority_leave(void*arg){struct near_driver*d=arg;d->pinned=false;d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);}
static bool support_equal(const struct vv_barge_onset_support*a,const struct vv_barge_onset_support*b){return a->evidence_id==b->evidence_id&&a->input_first==b->input_first&&a->input_next==b->input_next&&a->advance_first==b->advance_first&&a->advance_next==b->advance_next&&a->completing_frame_first==b->completing_frame_first&&a->completing_frame_next==b->completing_frame_next;}
static int onset_support(void*arg,const struct vv_confirmed_wake_context*c,const struct vv_barge_onset_support*s){
 struct near_driver*d=arg;if(!d->pinned||!context_equal(c,&d->calibration.context)||!support_equal(s,&d->onset.support))return -ESTALE;
 return d->ops.verify_onset?d->ops.verify_onset(d->ops.arg,&d->calibration,&d->onset):-ENOTSUP;
}
static uint64_t now_ms(void*arg){struct near_driver*d=arg;return d->ops.now_ms?d->ops.now_ms(d->ops.arg):UINT64_MAX;}
static int tail_frame(void*arg,const struct vv_barge_ticket*t,const struct k7_voice_frame*raw,struct k7_voice_frame*clean,enum vv_barge_material*material){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);struct vv_barge_observation o;
 int rc=!context_equal(&t->context,&d->calibration.context)?-ESTALE:pin(d,&o);bool held=!rc;
 if(!rc)rc=!d->calibration.tail_supported?-ENOTSUP:-EAGAIN;
 if(held&&d->calibration.tail_supported)for(unsigned i=0;i<d->count;i++){
  struct stored*f=&d->frames[(d->head+i)%STORED];if(f->raw.first_sample!=raw->first_sample)continue;
  if(!f->tail||f->raw.epoch!=raw->epoch||raw->samples!=320||raw->flags||memcmp(f->raw.pcm,raw->pcm,sizeof(raw->pcm))){rc=-ESTALE;break;}
  rc=d->ops.verify_frame?d->ops.verify_frame(d->ops.arg,&d->calibration,raw,&f->clean,true):-ENOTSUP;
  if(!rc){*clean=f->clean;*material=tail_material(d);}break;
 }
 if(held)d->ops.unpin(d->ops.arg);
 pthread_mutex_unlock(&d->lock);return rc;
}
static int attach(void*arg,struct vv_barge_route*r){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);int rc=d->route||d->closing?-EBUSY:!d->ops.start?-ENOTSUP:0;
 if(!rc)d->route=r;
 pthread_mutex_unlock(&d->lock);if(rc)return rc;
 rc=d->ops.start(d->ops.arg,d);if(rc){pthread_mutex_lock(&d->lock);d->route=NULL;pthread_mutex_unlock(&d->lock);}return rc;
}
static int detach(void*arg,struct vv_barge_route*r){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);int rc=d->route!=r?-ESTALE:0;
 if(!rc)d->closing=true;
 pthread_mutex_unlock(&d->lock);if(rc)return rc;
 rc=d->ops.stop_join?d->ops.stop_join(d->ops.arg,d):-ENOTSUP;if(rc)return rc;
 pthread_mutex_lock(&d->lock);d->route=NULL;d->installed=false;pthread_mutex_unlock(&d->lock);return 0;
}
int nd_create(const struct nd_ops*ops,struct near_driver**out){
 if(!ops||!out)return -EINVAL;
 *out=NULL;struct near_driver*d=calloc(1,sizeof(*d));if(!d)return -ENOMEM;
 int rc=pthread_mutex_init(&d->lock,NULL);if(rc){free(d);return -rc;}d->ops=*ops;*out=d;return 0;
}
int nd_install(struct near_driver*d,const struct nd_calibration*c){
 if(!d||!c)return -EINVAL;
 pthread_mutex_lock(&d->lock);
 int rc=d->closing||d->count?-EBUSY:!d->ops.verify_import?-ENOTSUP:0;
 if(!rc&&(c->revision<=d->last_revision||c->first>=c->next||!c->context.owner_epoch||!c->context.input_epoch||!c->context.clean_epoch||!c->context.model_epoch||!c->context.alignment_epoch))rc=-EINVAL;
 if(!rc)rc=d->ops.verify_import(d->ops.arg,c);
 if(!rc){d->calibration=*c;d->last_revision=c->revision;d->installed=true;d->tail_first=UINT64_MAX;}
 pthread_mutex_unlock(&d->lock);return rc;
}
void nd_invalidate(struct near_driver*d){if(d){pthread_mutex_lock(&d->lock);d->installed=false;pthread_mutex_unlock(&d->lock);}}
void nd_export(struct near_driver*d,struct vv_barge_driver*out){*out=(struct vv_barge_driver){.authority={d,authority_enter,authority_leave},.tail={d,now_ms,tail_frame},.onset_support=onset_support,.attach=attach,.detach=detach,.arg=d};}
int nd_frame(struct near_driver*d,const struct vv_confirmed_wake_context*c,const struct k7_voice_frame*raw,const struct k7_voice_frame*clean,bool tail){
 if(!d||!c||!raw||!clean)return -EINVAL;
 pthread_mutex_lock(&d->lock);struct vv_barge_observation o;
 int rc=!d->route||!context_equal(c,&d->calibration.context)?-ESTALE:pin(d,&o);bool held=!rc;
 if(!rc&&(raw->samples!=320||clean->samples!=320||raw->flags||clean->flags||raw->epoch!=c->input_epoch||clean->epoch!=c->input_epoch||raw->first_sample!=clean->first_sample||raw->first_sample>UINT64_MAX-320||
 raw->first_sample<d->calibration.first||raw->first_sample+320>d->calibration.next||raw->first_sample+320>o.microphone_next||
 (d->count&&raw->first_sample!=d->next)))rc=-EPIPE;
 if(!rc)rc=d->ops.verify_frame?d->ops.verify_frame(d->ops.arg,&d->calibration,raw,clean,tail):-ENOTSUP;
 if(!rc&&tail&&!d->calibration.tail_supported)rc=-ENOTSUP;
 if(!rc&&!tail&&d->tail_first!=UINT64_MAX)rc=-EPIPE;
 if(!rc){if(!d->count)d->first=raw->first_sample;if(d->count==STORED){d->head=(d->head+1)%STORED;d->count--;}
  if(tail&&d->tail_first==UINT64_MAX)d->tail_first=raw->first_sample;
  d->frames[(d->head+d->count)%STORED]=(struct stored){*raw,*clean,tail};d->count++;d->next=raw->first_sample+320;}
 if(rc)d->installed=false;
 bool tail_forwarded=d->tail_forwarded;struct vv_barge_route*r=d->route;if(held)d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);if(rc)return rc;
 rc=tail&&!tail_forwarded?vv_barge_route_tail(r,c,raw,o.now_ms):vv_barge_route_clean(r,c,tail?tail_material(d):clean_material(d),clean);
 if(tail&&!tail_forwarded&&rc==1){pthread_mutex_lock(&d->lock);d->tail_forwarded=true;pthread_mutex_unlock(&d->lock);}
 return rc;
}
int nd_onset(struct near_driver*d,const struct nd_evidence*e,uint64_t first,uint64_t next){
 if(!d||!e)return -EINVAL;
 pthread_mutex_lock(&d->lock);struct vv_barge_observation o;int rc=!d->route?-ENODEV:pin(d,&o);bool held=!rc;
 if(!rc&&(e->calibration_revision!=d->calibration.revision||e->support.evidence_id<=d->last_evidence||!d->calibration.tail_supported))rc=-ESTALE;
 if(!rc)rc=d->ops.verify_onset?d->ops.verify_onset(d->ops.arg,&d->calibration,e):-ENOTSUP;
 if(!rc){d->onset=*e;d->last_evidence=e->support.evidence_id;}
 struct vv_barge_route*r=d->route;if(held)d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);if(rc)return rc;
 return vv_barge_route_onset(r,first,next,&e->support);
}
int nd_destroy(struct near_driver*d){if(!d)return 0;pthread_mutex_lock(&d->lock);if(d->route){pthread_mutex_unlock(&d->lock);return -EBUSY;}pthread_mutex_unlock(&d->lock);pthread_mutex_destroy(&d->lock);memset(d,0,sizeof(*d));free(d);return 0;}

#else
#include "near_driver.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define STORED 256u
struct stored {struct k7_voice_frame raw,clean;bool tail;};
struct near_driver {
 pthread_mutex_t lock;
 struct nd_ops ops;struct nd_calibration calibration;struct nd_evidence onset;
 struct vv_barge_route *route;struct stored frames[STORED];
 unsigned count,head;uint64_t first,next,last_revision,last_evidence,tail_first;
 bool installed,closing,pinned,tail_forwarded;
};
static bool context_equal(const struct vv_confirmed_wake_context*a,const struct vv_confirmed_wake_context*b){
#define E(n) a->n==b->n
 return E(owner_epoch)&&E(model_epoch)&&E(clean_epoch)&&E(alignment_epoch)&&E(scope_incarnation)&&E(lease_serial)&&E(reply_id)&&E(input_epoch)&&E(mode_epoch)&&E(asr_epoch)&&E(tx_generation);
#undef E
}
static int pin(struct near_driver*d,struct vv_barge_observation*out){
 if(!d->installed||d->closing)return -ENOTSUP;
 if(!d->ops.pin||!d->ops.unpin)return -ENOTSUP;
 int rc=d->ops.pin(d->ops.arg,&d->calibration,out);if(rc)return rc;
 if(!out->rx_live||out->photo_busy||out->owner_epoch!=d->calibration.context.owner_epoch||
 out->input_epoch!=d->calibration.context.input_epoch||out->mode_epoch!=d->calibration.context.mode_epoch||
 out->live_tx_generation!=d->calibration.context.tx_generation||out->now_ms>d->calibration.expires_ms){d->ops.unpin(d->ops.arg);return -ESTALE;}
 return 0;
}
static int range(struct near_driver*d,uint64_t first,uint64_t next,bool tail){
 if(first>=next||first<d->calibration.first||next>d->calibration.next)return -ERANGE;
 (void)tail; /* Every physical frame in this contiguous interval was checked
  * under the independent pin. PCM cache may roll; this proof interval cannot. */
 return d->count&&first>=d->first&&next<=d->next?0:-EAGAIN;
}
static int authority_enter(void*arg,const struct vv_confirmed_wake_context*c,enum vv_barge_check why,enum vv_barge_material material,uint64_t first,uint64_t next,struct vv_barge_observation*out){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);
 int rc=!context_equal(c,&d->calibration.context)?-ESTALE:pin(d,out);
 if(rc){pthread_mutex_unlock(&d->lock);return rc;}
 bool tail=material==VV_BARGE_QUALIFIED_MUTED_CONTINUOUS;
 rc=material==VV_BARGE_RAW_UNKNOWN?-ENOTSUP:range(d,first,next,false);
 if(!rc&&tail&&!d->calibration.tail_supported)rc=-ENOTSUP;
 if(!rc&&tail&&next-first==320&&(d->tail_first==UINT64_MAX||first<d->tail_first))rc=-ESTALE;
 if(!rc&&(why==VV_BARGE_CHECK_CLAIM||why==VV_BARGE_CHECK_CAPTURE)&&
   (d->tail_first==UINT64_MAX||next<=d->tail_first))rc=-EAGAIN;
 if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL)){
  if(!d->calibration.tail_supported||!d->onset.support.evidence_id||!d->ops.verify_onset)rc=-ENOTSUP;
  else rc=d->ops.verify_onset(d->ops.arg,&d->calibration,&d->onset);
 }
 if(rc){d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);return rc;}
 d->pinned=true;return 0;
}
static void authority_leave(void*arg){struct near_driver*d=arg;d->pinned=false;d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);}
static bool support_equal(const struct vv_barge_onset_support*a,const struct vv_barge_onset_support*b){return a->evidence_id==b->evidence_id&&a->input_first==b->input_first&&a->input_next==b->input_next&&a->advance_first==b->advance_first&&a->advance_next==b->advance_next&&a->completing_frame_first==b->completing_frame_first&&a->completing_frame_next==b->completing_frame_next;}
static int onset_support(void*arg,const struct vv_confirmed_wake_context*c,const struct vv_barge_onset_support*s){
 struct near_driver*d=arg;if(!d->pinned||!context_equal(c,&d->calibration.context)||!support_equal(s,&d->onset.support))return -ESTALE;
 return d->ops.verify_onset?d->ops.verify_onset(d->ops.arg,&d->calibration,&d->onset):-ENOTSUP;
}
static uint64_t now_ms(void*arg){struct near_driver*d=arg;return d->ops.now_ms?d->ops.now_ms(d->ops.arg):UINT64_MAX;}
static int tail_frame(void*arg,const struct vv_barge_ticket*t,const struct k7_voice_frame*raw,struct k7_voice_frame*clean,enum vv_barge_material*material){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);struct vv_barge_observation o;
 int rc=!context_equal(&t->context,&d->calibration.context)?-ESTALE:pin(d,&o);bool held=!rc;
 if(!rc)rc=!d->calibration.tail_supported?-ENOTSUP:-EAGAIN;
 if(held&&d->calibration.tail_supported)for(unsigned i=0;i<d->count;i++){
  struct stored*f=&d->frames[(d->head+i)%STORED];if(f->raw.first_sample!=raw->first_sample)continue;
  if(!f->tail||f->raw.epoch!=raw->epoch||raw->samples!=320||raw->flags||memcmp(f->raw.pcm,raw->pcm,sizeof(raw->pcm))){rc=-ESTALE;break;}
  rc=d->ops.verify_frame?d->ops.verify_frame(d->ops.arg,&d->calibration,raw,&f->clean,true):-ENOTSUP;
  if(!rc){*clean=f->clean;*material=VV_BARGE_QUALIFIED_MUTED_CONTINUOUS;}break;
 }
 if(held)d->ops.unpin(d->ops.arg);
 pthread_mutex_unlock(&d->lock);return rc;
}
static int attach(void*arg,struct vv_barge_route*r){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);int rc=d->route||d->closing?-EBUSY:!d->ops.start?-ENOTSUP:0;
 if(!rc)d->route=r;
 pthread_mutex_unlock(&d->lock);if(rc)return rc;
 rc=d->ops.start(d->ops.arg,d);if(rc){pthread_mutex_lock(&d->lock);d->route=NULL;pthread_mutex_unlock(&d->lock);}return rc;
}
static int detach(void*arg,struct vv_barge_route*r){
 struct near_driver*d=arg;pthread_mutex_lock(&d->lock);int rc=d->route!=r?-ESTALE:0;
 if(!rc)d->closing=true;
 pthread_mutex_unlock(&d->lock);if(rc)return rc;
 rc=d->ops.stop_join?d->ops.stop_join(d->ops.arg,d):-ENOTSUP;if(rc)return rc;
 pthread_mutex_lock(&d->lock);d->route=NULL;d->installed=false;pthread_mutex_unlock(&d->lock);return 0;
}
int nd_create(const struct nd_ops*ops,struct near_driver**out){
 if(!ops||!out)return -EINVAL;
 *out=NULL;struct near_driver*d=calloc(1,sizeof(*d));if(!d)return -ENOMEM;
 int rc=pthread_mutex_init(&d->lock,NULL);if(rc){free(d);return -rc;}d->ops=*ops;*out=d;return 0;
}
int nd_install(struct near_driver*d,const struct nd_calibration*c){
 if(!d||!c)return -EINVAL;
 pthread_mutex_lock(&d->lock);
 int rc=d->closing||d->count?-EBUSY:!d->ops.verify_import?-ENOTSUP:0;
 if(!rc&&(c->revision<=d->last_revision||c->first>=c->next||!c->context.owner_epoch||!c->context.input_epoch||!c->context.clean_epoch||!c->context.model_epoch||!c->context.alignment_epoch))rc=-EINVAL;
 if(!rc)rc=d->ops.verify_import(d->ops.arg,c);
 if(!rc){d->calibration=*c;d->last_revision=c->revision;d->installed=true;d->tail_first=UINT64_MAX;}
 pthread_mutex_unlock(&d->lock);return rc;
}
void nd_invalidate(struct near_driver*d){if(d){pthread_mutex_lock(&d->lock);d->installed=false;pthread_mutex_unlock(&d->lock);}}
void nd_export(struct near_driver*d,struct vv_barge_driver*out){*out=(struct vv_barge_driver){.authority={d,authority_enter,authority_leave},.tail={d,now_ms,tail_frame},.onset_support=onset_support,.attach=attach,.detach=detach,.arg=d};}
int nd_frame(struct near_driver*d,const struct vv_confirmed_wake_context*c,const struct k7_voice_frame*raw,const struct k7_voice_frame*clean,bool tail){
 if(!d||!c||!raw||!clean)return -EINVAL;
 pthread_mutex_lock(&d->lock);struct vv_barge_observation o;
 int rc=!d->route||!context_equal(c,&d->calibration.context)?-ESTALE:pin(d,&o);bool held=!rc;
 if(!rc&&(raw->samples!=320||clean->samples!=320||raw->flags||clean->flags||raw->epoch!=c->input_epoch||clean->epoch!=c->input_epoch||raw->first_sample!=clean->first_sample||raw->first_sample>UINT64_MAX-320||
 raw->first_sample<d->calibration.first||raw->first_sample+320>d->calibration.next||raw->first_sample+320>o.microphone_next||
 (d->count&&raw->first_sample!=d->next)))rc=-EPIPE;
 if(!rc)rc=d->ops.verify_frame?d->ops.verify_frame(d->ops.arg,&d->calibration,raw,clean,tail):-ENOTSUP;
 if(!rc&&tail&&!d->calibration.tail_supported)rc=-ENOTSUP;
 if(!rc&&!tail&&d->tail_first!=UINT64_MAX)rc=-EPIPE;
 if(!rc){if(!d->count)d->first=raw->first_sample;if(d->count==STORED){d->head=(d->head+1)%STORED;d->count--;}
  if(tail&&d->tail_first==UINT64_MAX)d->tail_first=raw->first_sample;
  d->frames[(d->head+d->count)%STORED]=(struct stored){*raw,*clean,tail};d->count++;d->next=raw->first_sample+320;}
 if(rc)d->installed=false;
 bool tail_forwarded=d->tail_forwarded;struct vv_barge_route*r=d->route;if(held)d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);if(rc)return rc;
 rc=tail&&!tail_forwarded?vv_barge_route_tail(r,c,raw,o.now_ms):vv_barge_route_clean(r,c,tail?VV_BARGE_QUALIFIED_MUTED_CONTINUOUS:VV_BARGE_QUALIFIED_CLEAN,clean);
 if(tail&&!tail_forwarded&&rc==1){pthread_mutex_lock(&d->lock);d->tail_forwarded=true;pthread_mutex_unlock(&d->lock);}
 return rc;
}
int nd_onset(struct near_driver*d,const struct nd_evidence*e,uint64_t first,uint64_t next){
 if(!d||!e)return -EINVAL;
 pthread_mutex_lock(&d->lock);struct vv_barge_observation o;int rc=!d->route?-ENODEV:pin(d,&o);bool held=!rc;
 if(!rc&&(e->calibration_revision!=d->calibration.revision||e->support.evidence_id<=d->last_evidence||!d->calibration.tail_supported))rc=-ESTALE;
 if(!rc)rc=d->ops.verify_onset?d->ops.verify_onset(d->ops.arg,&d->calibration,e):-ENOTSUP;
 if(!rc){d->onset=*e;d->last_evidence=e->support.evidence_id;}
 struct vv_barge_route*r=d->route;if(held)d->ops.unpin(d->ops.arg);pthread_mutex_unlock(&d->lock);if(rc)return rc;
 return vv_barge_route_onset(r,first,next,&e->support);
}
int nd_destroy(struct near_driver*d){if(!d)return 0;pthread_mutex_lock(&d->lock);if(d->route){pthread_mutex_unlock(&d->lock);return -EBUSY;}pthread_mutex_unlock(&d->lock);pthread_mutex_destroy(&d->lock);memset(d,0,sizeof(*d));free(d);return 0;}

#endif

#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
/* Caller drains observer and verifies old route/source completion first. */
int nd_rearm(struct near_driver*d){
 if(!d)return -EINVAL;
 pthread_mutex_lock(&d->lock);
 int rc=d->closing||d->pinned||!d->route?-EBUSY:0;
 if(!rc){d->installed=false;d->head=d->count=0;d->first=d->next=0;
  d->last_revision=d->last_evidence=0;d->tail_first=UINT64_MAX;d->tail_forwarded=false;
  memset(&d->calibration,0,sizeof(d->calibration));memset(&d->onset,0,sizeof(d->onset));memset(d->frames,0,sizeof(d->frames));}
 pthread_mutex_unlock(&d->lock);return rc;
}
#endif
