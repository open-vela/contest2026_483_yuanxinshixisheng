#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "vv_near_experiment.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define EX_FRAMES 256u
struct ex_frame {struct k7_voice_frame raw,residual;bool tail;};
struct vv_near_experiment {
 pthread_mutex_t lock;struct vv_near_experiment_policy policy;
 int (*observe)(void*,struct vv_barge_observation*);void *arg;
 struct near_driver *driver;struct vv_barge_driver inner;struct vv_barge_route *route;
 struct nd_calibration binding;struct ne_identity identity;
 struct ex_frame frames[EX_FRAMES];unsigned head,count;
 struct ne_report report;struct nd_evidence evidence;
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 struct ne_identity previous;bool have_previous;
#endif
 uint64_t first,next;atomic_bool bound,failed,fired;
};
static bool same(const struct ne_identity*a,const struct ne_identity*b){
#define E(n) a->n==b->n
 return E(owner)&&E(reference_instance)&&E(reference_epoch)&&E(clock_epoch)&&E(dialogue_instance)&&E(scope)&&E(lease)&&E(reply)&&E(mapping_revision)&&E(input_epoch)&&E(tx_generation);
#undef E
}
static int imported(void*arg,const struct nd_calibration*c){struct vv_near_experiment*x=arg;
 return x->policy.enabled&&x->bound&&!x->failed&&c->experimental_policy&&c->revision==x->policy.revision&&
 c->first==x->first&&c->next-x->first==x->policy.maximum_samples&&
 !memcmp(c->evidence_sha256,x->policy.capture_sha256,32)?0:-EACCES;
}
static int pin(void*arg,const struct nd_calibration*c,struct vv_barge_observation*out){
 struct vv_near_experiment*x=arg;pthread_mutex_lock(&x->lock);
 int rc=!x->bound||x->failed||c->revision!=x->binding.revision?-ESTALE:x->observe(x->arg,out);
 /* This pin protects real stored worker PCM and selected policy; platform/
  * manager identities are actual value observations rechecked by route. It
  * does NOT claim a calibrated acoustic authority or hold a hardware lock. */
 if(rc){pthread_mutex_unlock(&x->lock);}return rc;
}
static void unpin(void*arg){struct vv_near_experiment*x=arg;pthread_mutex_unlock(&x->lock);}
static int frame(void*arg,const struct nd_calibration*c,const struct k7_voice_frame*r,const struct k7_voice_frame*z,bool tail){
 struct vv_near_experiment*x=arg;(void)c;
 for(unsigned i=0;i<x->count;i++){struct ex_frame*f=&x->frames[(x->head+i)%EX_FRAMES];
  if(f->raw.first_sample==r->first_sample)return f->tail==tail&&
   f->raw.epoch==r->epoch&&f->residual.epoch==z->epoch&&r->samples==320&&z->samples==320&&!r->flags&&!z->flags&&
   !memcmp(f->raw.pcm,r->pcm,sizeof(r->pcm))&&!memcmp(f->residual.pcm,z->pcm,sizeof(z->pcm))?0:-ESTALE;
 }return -ESTALE;
}
static int onset(void*arg,const struct nd_calibration*c,const struct nd_evidence*e){
 struct vv_near_experiment*x=arg;const struct ne_report*r=&x->report;
 return !x->failed&&c->experimental_policy&&e->calibration_revision==x->policy.revision&&
 !memcmp(&e->support,&x->evidence.support,sizeof(e->support))&&r->candidate&&!r->qualified&&!r->clipped&&
 isfinite(r->model.probability)&&r->model.probability>=x->policy.probability&&r->consecutive>=x->policy.consecutive_windows&&
 r->residual_power>=1000&&r->correlation2<=.5&&r->residual_power>=r->reference_power*.01&&
 same(&r->identity,&x->identity)?0:-EACCES;
}
static uint64_t now(void*arg){struct vv_near_experiment*x=arg;struct vv_barge_observation o;return x->observe(x->arg,&o)?UINT64_MAX:o.now_ms;}
static void revoke(struct vv_near_experiment*x){pthread_mutex_lock(&x->lock);x->failed=true;pthread_mutex_unlock(&x->lock);nd_invalidate(x->driver);}
static void receive(void*arg,const struct k7_echo_packet*p,int reason){
 struct vv_near_experiment*x=arg;if(reason||!p){revoke(x);return;}
 if(x->failed)return;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(x->policy.apm_nominal.kind){
  if(p->apm_post_stop_hypothesis_samples>p->source_stop_zero_samples||p->apm_post_stop_hypothesis_samples>320||(p->apm_post_stop_hypothesis_samples&&!p->cancel_observed)||!k7_apm21_nominal_identity_valid(&p->apm_nominal)||p->identity.mapping_revision!=x->policy.apm_nominal.mapping_revision){revoke(x);return;}
 }else if(p->apm_nominal.kind){revoke(x);return;}
#endif
 if(p->applied_lag_samples!=x->policy.lag_samples){revoke(x);return;}
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 if(!x->bound&&x->have_previous){
  if(same(&p->identity,&x->previous))return; /* queued old reply cannot rebind */
  if(p->identity.owner!=x->previous.owner||p->identity.input_epoch!=x->previous.input_epoch||
     p->identity.reply==x->previous.reply||p->identity.tx_generation<=x->previous.tx_generation){revoke(x);return;}
 }
#endif
 if(!x->bound){
  struct vv_confirmed_wake_context c;struct vv_barge_observation o;
  if(vv_barge_route_context(x->route,&c)||x->observe(x->arg,&o))return;
  if(p->raw.first_sample>UINT64_MAX-x->policy.maximum_samples||o.now_ms>UINT64_MAX-60000){revoke(x);return;}
  /* IDs identify experimental policy/model stream, not imported clean proof. */
  c.clean_epoch=c.alignment_epoch=x->policy.revision;c.model_epoch=p->identity.dialogue_instance;
  x->identity=p->identity;x->first=x->next=p->raw.first_sample;
  x->binding=(struct nd_calibration){.context=c,.first=x->first,.next=x->first+x->policy.maximum_samples,
   .expires_ms=o.now_ms+60000,.revision=x->policy.revision,.tail_supported=true,.experimental_policy=true};
  memcpy(x->binding.evidence_sha256,x->policy.capture_sha256,32);x->bound=true;
  if(nd_install(x->driver,&x->binding)){revoke(x);return;}
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
  printf("NEAR_EXPERIMENT armed=1 policy=%llu lag=%u calibrated=0 multi_round=1 reply=%llu tx=%u\n",(unsigned long long)x->policy.revision,x->policy.lag_samples,(unsigned long long)x->identity.reply,x->identity.tx_generation);
#else
  printf("NEAR_EXPERIMENT armed=1 policy=%llu lag=%u calibrated=0 single_reply=1\n",(unsigned long long)x->policy.revision,x->policy.lag_samples);
#endif
 }
 const struct vv_confirmed_wake_context*c=&x->binding.context;
 if(!same(&p->identity,&x->identity)||p->raw.first_sample!=x->next||p->raw.first_sample!=p->residual.first_sample||
 p->raw.epoch!=c->input_epoch||p->residual.epoch!=c->input_epoch||p->raw.samples!=320||p->residual.samples!=320||p->raw.flags||p->residual.flags||
 c->owner_epoch!=p->identity.owner||c->reply_id!=p->identity.reply||c->scope_incarnation!=p->identity.scope||c->lease_serial!=p->identity.lease||c->tx_generation!=p->identity.tx_generation||
 p->source_stop_zero_samples>320||p->raw.first_sample>x->binding.next-320){revoke(x);return;}
 pthread_mutex_lock(&x->lock);
 if(x->count==EX_FRAMES){x->head=(x->head+1)%EX_FRAMES;x->count--;}
 x->frames[(x->head+x->count)%EX_FRAMES]=(struct ex_frame){p->raw,p->residual,p->cancel_observed};x->count++;x->next+=320;
 bool candidate=!x->fired&&!p->cancel_observed&&p->report&&p->report->candidate&&x->next-x->first>=x->policy.warmup_samples;
 if(candidate){x->report=*p->report;const struct k7_vad_observation*m=&p->report->model;
  x->evidence=(struct nd_evidence){.calibration_revision=x->policy.revision,.support={.evidence_id=m->window_id,
   .input_first=m->input_first_sample,.input_next=m->input_end_sample,.advance_first=m->advance_first_sample,.advance_next=m->advance_end_sample,
   .completing_frame_first=p->raw.first_sample,.completing_frame_next=x->next}};}
 pthread_mutex_unlock(&x->lock);
 int rc=nd_frame(x->driver,c,&p->raw,&p->residual,p->cancel_observed);
 if(rc<0){printf("NEAR_EXPERIMENT frame_rc=%d\n",rc);revoke(x);return;}
 if(candidate){uint64_t support=x->evidence.support.input_first;
  if(support<x->first)return;
  uint64_t first=x->first+((support-x->first)/320)*320;
  if(first-x->first<5120)return;
  rc=nd_onset(x->driver,&x->evidence,first,x->next);
  printf("NEAR_EXPERIMENT onset_rc=%d probability=%.6f calibrated=0\n",rc,(double)x->report.model.probability);
  if(rc==1)x->fired=true;else if(rc<0)revoke(x);
 }
}
static int start(void*arg,struct near_driver*d){struct vv_near_experiment*x=arg;return d==x->driver?k7_echo_observer_subscribe(receive,x):-ESTALE;}
static int stop(void*arg,struct near_driver*d){struct vv_near_experiment*x=arg;return d==x->driver?k7_echo_observer_unsubscribe(receive,x):-ESTALE;}
static int attach(void*arg,struct vv_barge_route*r){struct vv_near_experiment*x=arg;x->route=r;int rc=x->inner.attach(x->inner.arg,r);if(rc)x->route=NULL;return rc;}
static int detach(void*arg,struct vv_barge_route*r){struct vv_near_experiment*x=arg;int rc=x->inner.detach(x->inner.arg,r);if(!rc)x->route=NULL;return rc;}
int vv_near_experiment_create(const struct vv_near_experiment_policy*p,int(*observe)(void*,struct vv_barge_observation*),void*arg,struct vv_near_experiment**out){
#if !defined(K7_NEAR_EXPERIMENT) || !K7_NEAR_EXPERIMENT
 (void)p;(void)observe;(void)arg;if(out)*out=NULL;return -ENOTSUP;
#endif
 if(!p||!out||!observe||!p->enabled||!p->revision||(
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 p->apm_nominal.kind?(!k7_apm21_nominal_identity_valid(&p->apm_nominal)||p->lag_samples!=0||p->revision!=K7_APM21_NOMINAL_REVISION||memcmp(p->capture_sha256,p->apm_nominal.near_capture_sha256,32)):
#endif
 p->lag_samples!=39)||!isfinite(p->probability)||p->probability<.9f||p->probability>1||p->consecutive_windows<3||p->warmup_samples<5120||p->maximum_samples>960000||p->maximum_samples<=p->warmup_samples)return -EINVAL;
 bool hash=false;for(unsigned i=0;i<32;i++)hash|=p->capture_sha256[i]!=0;if(!hash)return -EINVAL;
 *out=calloc(1,sizeof(**out));if(!*out)return -ENOMEM;struct vv_near_experiment*x=*out;x->policy=*p;x->observe=observe;x->arg=arg;
 int rc=pthread_mutex_init(&x->lock,NULL);if(rc){free(x);*out=NULL;return -rc;}
 struct nd_ops ops={x,imported,pin,unpin,frame,onset,now,start,stop};rc=nd_create(&ops,&x->driver);
 if(rc){pthread_mutex_destroy(&x->lock);free(x);*out=NULL;return rc;}nd_export(x->driver,&x->inner);return 0;
}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
void vv_near_experiment_apm_nominal_policy(struct vv_near_experiment_policy *p){
 if(!p)return;
 memset(p,0,sizeof(*p));k7_apm21_nominal_identity_expected(&p->apm_nominal);
 p->enabled=true;p->revision=K7_APM21_NOMINAL_REVISION;p->lag_samples=0;
 memcpy(p->capture_sha256,p->apm_nominal.near_capture_sha256,32);
 p->probability=.9f;p->consecutive_windows=3;p->warmup_samples=16000;p->maximum_samples=960000;
}
#endif
void vv_near_experiment_export(struct vv_near_experiment*x,struct vv_barge_driver*out){*out=x->inner;out->attach=attach;out->detach=detach;out->arg=x;}
int vv_near_experiment_destroy(struct vv_near_experiment*x){if(!x)return 0;int rc=nd_destroy(x->driver);if(rc)return rc;pthread_mutex_destroy(&x->lock);free(x);return 0;}

#else
#include "vv_near_experiment.h"
#include <errno.h>
int vv_near_experiment_create(const struct vv_near_experiment_policy*p,int(*observe)(void*,struct vv_barge_observation*),void*arg,struct vv_near_experiment**out){(void)p;(void)observe;(void)arg;if(out)*out=0;return -ENOTSUP;}
void vv_near_experiment_export(struct vv_near_experiment*x,struct vv_barge_driver*out){(void)x;if(out)*out=(struct vv_barge_driver){0};}
int vv_near_experiment_destroy(struct vv_near_experiment*x){return x?-ENOTSUP:0;}
#endif

#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
/* Serial platform control only; never observer/PIO callback. Worker reset must
 * occur before this operation, and next reply must not start concurrently. */
int vv_near_experiment_rearm(struct vv_near_experiment*x,int(*reset_worker)(void*),void*worker_arg){
 if(!x||!x->route||!reset_worker)return -EINVAL;
 int rc=vv_barge_route_rearm_ready(x->route);if(rc)return rc;
 rc=k7_echo_observer_unsubscribe(receive,x);if(rc)return rc;
 rc=vv_barge_route_rearm_ready(x->route);
 if(!rc)rc=reset_worker(worker_arg);
 if(!rc)rc=nd_rearm(x->driver);
 if(!rc){pthread_mutex_lock(&x->lock);
  if(x->bound){x->previous=x->identity;x->have_previous=true;}
  x->bound=x->failed=x->fired=false;x->first=x->next=0;x->head=x->count=0;
  memset(&x->identity,0,sizeof(x->identity));memset(&x->binding,0,sizeof(x->binding));
  memset(&x->report,0,sizeof(x->report));memset(&x->evidence,0,sizeof(x->evidence));memset(x->frames,0,sizeof(x->frames));
  pthread_mutex_unlock(&x->lock);
 }
 int restart=k7_echo_observer_subscribe(receive,x);
 if(rc||restart)revoke(x);
 return rc?rc:restart;
}
#endif
