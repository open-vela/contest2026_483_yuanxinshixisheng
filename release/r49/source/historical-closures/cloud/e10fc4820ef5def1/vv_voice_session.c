/* Experimental audio/KWS owner. Keyword observations are diagnostic until the
 * real microphone model and playback-echo behavior have been accepted. */
#include "vv_voice_session.h"
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
#include "k7_apm21_prepared_lifecycle.h"
#if !defined(K7_ECHO_RESIDUAL_EVIDENCE) || !K7_ECHO_RESIDUAL_EVIDENCE || !defined(K7_ECHO_RAW_VAD_OBSERVER) || !K7_ECHO_RAW_VAD_OBSERVER
#error prepared lifecycle requires the exclusive existing residual model owner
#endif
#if !defined(CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER) || !defined(CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME)
#error prepared lifecycle requires the existing model/echo owners
#endif
#endif
#line 4
#include "vv_confirmed_wake.h"
#include "vv_wake_input.h"
#include "vv_paraformer_k7.h"
#include "vv_native_runtime.h"
#include "duplex_session.h"
#include "voicelink/kws_embedded.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
#include "echo_diag_runtime.h"
static struct k7_echo_runtime *diagnostic;
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
#include "raw_vad_owner.h"
static struct k7_raw_vad_loan raw_vad_loan;
/* Service owner only. Nonzero means admission succeeded but no consumer was
 * created, and startup admission retirement still needs a bounded retry. */
static uint64_t startup_retirement;
static int retire_unstarted(void){
 if(!startup_retirement)return 0;
 int rc=k7_raw_vad_owner_voice_end(startup_retirement);
 if(!rc)startup_retirement=0;
 return rc;
}
#endif
#ifdef CONFIG_SMP
#include <sched.h>
#endif
extern int sha256_vector(size_t, const uint8_t *[], const size_t *, uint8_t *);
static _Atomic(struct k7_voice_session *) audio;
static atomic_bool stopping, done, ready;
static atomic_int result;
static atomic_uint asr_epoch, tx_generation;
static atomic_uint_fast64_t owner_sequence, owner_epoch;
static pthread_t consumer;
static pthread_mutex_t event_lock=PTHREAD_MUTEX_INITIALIZER;
static struct vv_voice_event_ops event_ops;
static struct {
 struct vv_voice_idle_ops ops;struct vv_wake_input *queue;
 struct vv_wake_request request;bool eligible,active;unsigned mode_epoch;
 uint64_t model_epoch;
} idle;
static atomic_uint_fast64_t model_sequence;

/* This mutex covers borrowed audio/observer lifetime only. It is never held
 * for a join, audio wait, model inference, TLS, or mode/history operation. */
/* start/stop are called only by the native service owner. */
static bool started,consumer_joined;
static void wait_ms(unsigned ms) {
 struct timespec t={ms/1000,(long)(ms%1000)*1000000};nanosleep(&t,NULL);
}
static uint64_t now_ms(void) {
 struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;
}
static int hash(const void *data,size_t bytes,uint8_t out[32],void *arg) {
 (void)arg;const uint8_t *blocks[]={data};return sha256_vector(1,blocks,&bytes,out);
}
static uint32_t next(atomic_uint *counter) {
 if(!atomic_load_explicit(&ready,memory_order_acquire)||atomic_load(&stopping))return 0;
 unsigned old=atomic_load(counter);
 do {if(old==UINT32_MAX)return 0;}
 while(!atomic_compare_exchange_weak(counter,&old,old+1));
 return old+1;
}
struct k7_voice_session *vv_voice_session_audio(void) {
 return atomic_load_explicit(&audio,memory_order_acquire);
}
uint64_t vv_voice_session_owner_epoch(const struct k7_voice_session *s) {
 if(!s||!atomic_load_explicit(&ready,memory_order_acquire)||atomic_load(&stopping))return 0;
 uint64_t e=atomic_load_explicit(&owner_epoch,memory_order_acquire);
 if(!e||atomic_load_explicit(&audio,memory_order_acquire)!=s)return 0;
 if(!atomic_load_explicit(&ready,memory_order_acquire)||atomic_load(&stopping)||
    atomic_load_explicit(&owner_epoch,memory_order_acquire)!=e)return 0;
 return e;
}

#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
/* Attach precedes consumer readiness. Validate the admitted lifetime without
 * granting the public ready-gated owner API or depending on started's race. */
uint64_t vv_voice_session_apm21_startup_owner_epoch(const struct k7_voice_session *s) {
 if(!s||atomic_load_explicit(&done,memory_order_acquire)||
    atomic_load_explicit(&stopping,memory_order_acquire))return 0;
 uint64_t e=atomic_load_explicit(&owner_epoch,memory_order_acquire);
 if(!e||atomic_load_explicit(&audio,memory_order_acquire)!=s)return 0;
 if(atomic_load_explicit(&done,memory_order_acquire)||
    atomic_load_explicit(&stopping,memory_order_acquire)||
    atomic_load_explicit(&audio,memory_order_acquire)!=s||
    atomic_load_explicit(&owner_epoch,memory_order_acquire)!=e)return 0;
 return e;
}
#endif

static bool same_event_ops(const struct vv_voice_event_ops *o){
 return o&&o->snapshot==event_ops.snapshot&&o->submit==event_ops.submit&&o->arg==event_ops.arg;
}
int vv_voice_session_event_register(const struct vv_voice_event_ops *o){
 if(!o||!o->snapshot||!o->submit||!o->arg)return -EINVAL;
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=event_ops.arg?-EBUSY:0;if(!rc)event_ops=*o;
 pthread_mutex_unlock(&event_lock);return rc;
}
int vv_voice_session_event_unregister(const struct vv_voice_event_ops *o){
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=!same_event_ops(o)?-ESTALE:0;
 if(!rc)memset(&event_ops,0,sizeof(event_ops));
 pthread_mutex_unlock(&event_lock);return rc;
}
static int event_state(struct vv_voice_event_state *out){
 struct k7_voice_session *s=vv_voice_session_audio();
 uint64_t owner=vv_voice_session_owner_epoch(s);
 if(!owner)return -ENOTCONN;
 struct k7_voice_session_status st;k7_voice_session_status(s,&st);
 uint64_t now=now_ms();
 if(!st.running||!st.ready||st.finished||st.stop_requested||!st.input_epoch||
    now==UINT64_MAX||vv_voice_session_owner_epoch(s)!=owner)return -ENOTCONN;
 *out=(struct vv_voice_event_state){owner,st.microphone_samples,now,
   st.input_epoch,st.tx_generation,st.tx_state!=K7_VOICE_TX_IDLE};
 return 0;
}
int vv_voice_session_observe(struct vv_voice_event_state *out){
 if(!out)return -EINVAL;
 memset(out,0,sizeof(*out));
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=atomic_load_explicit(&done,memory_order_acquire)?-ENOTCONN:event_state(out);
 if(rc)memset(out,0,sizeof(*out));
 pthread_mutex_unlock(&event_lock);return rc;
}
int vv_voice_session_event_snapshot(struct vv_confirmed_wake_context *out,
 uint64_t *sample_next,uint64_t *time_ms){
 if(!out||!sample_next||!time_ms)return -EINVAL;
 memset(out,0,sizeof(*out));*sample_next=0;*time_ms=0;
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 struct vv_voice_event_state st;int rc=event_ops.arg?event_state(&st):-ENOTSUP;
 if(!rc)rc=event_ops.snapshot(event_ops.arg,&st,out);
 if(!rc){*sample_next=st.microphone_samples;*time_ms=st.now_ms;}
 else memset(out,0,sizeof(*out));
 pthread_mutex_unlock(&event_lock);return rc;
}
int vv_voice_session_event_submit(const struct vv_confirmed_wake_event *input){
 if(!input)return -EINVAL;
 const struct vv_confirmed_wake_event event=*input;
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 struct vv_voice_event_state st;int rc=event_ops.arg?event_state(&st):-ENOTSUP;
 if(!rc&&(event.context.owner_epoch!=st.owner_epoch||event.context.input_epoch!=st.input_epoch||
    !st.tx_active||!event.context.tx_generation||event.context.tx_generation!=st.tx_generation||
    !event.event_id||!event.sample_next||event.sample_first>=event.sample_next||
    event.sample_next>st.microphone_samples||st.microphone_samples-event.sample_next>12800||
    event.confirmed_ms>st.now_ms||st.now_ms-event.confirmed_ms>1000))rc=-ESTALE;
 if(!rc)rc=event_ops.submit(event_ops.arg,&st,&event);
 pthread_mutex_unlock(&event_lock);return rc;
}

static bool idle_ops_match(const struct vv_voice_idle_ops *o){
 return o&&o->arg&&o->arg==idle.ops.arg&&o->mode_epoch==idle.ops.mode_epoch;
}
static bool idle_context_match(const struct vv_confirmed_wake_context *a,
 const struct vv_confirmed_wake_context *b){
 return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&
 a->mode_epoch==b->mode_epoch&&a->model_epoch==b->model_epoch&&
 a->clean_epoch==b->clean_epoch&&a->alignment_epoch==b->alignment_epoch&&
 a->scope_incarnation==b->scope_incarnation&&a->lease_serial==b->lease_serial&&
 a->reply_id==b->reply_id&&a->asr_epoch==b->asr_epoch&&a->tx_generation==b->tx_generation;
}
static bool idle_ticket(const struct vv_wake_request *r){
 return r&&idle.active&&r->id&&r->id==idle.request.id&&
 r->sample_first==idle.request.sample_first&&r->keyword_end==idle.request.keyword_end&&
 r->created_ms==idle.request.created_ms&&idle_context_match(&r->context,&idle.request.context);
}
static int idle_context(struct vv_confirmed_wake_context *c,uint64_t *now){
 if(!idle.queue||!idle.eligible||!idle.model_epoch||!idle.mode_epoch||
    idle.ops.mode_epoch(idle.ops.arg)!=idle.mode_epoch)return -ESTALE;
 struct vv_voice_event_state st;int rc=event_state(&st);if(rc)return rc;
 if(st.tx_active)return -EBUSY;
 *c=(struct vv_confirmed_wake_context){.owner_epoch=st.owner_epoch,
 .input_epoch=st.input_epoch,.model_epoch=idle.model_epoch,.mode_epoch=idle.mode_epoch};
 *now=st.now_ms;return 0;
}
static int idle_source_health_locked(const struct vv_wake_request *r){
 if(!idle_ticket(r))return VV_WAKE_STALE;
 /* This is a per-candidate loss, not a broadened runtime/network policy.
  * Real owner failure and explicit stop retain their distinct provenance. */
 if(atomic_load(&stopping))return -ECANCELED;
 struct vv_voice_event_state st;
 if(event_state(&st))return VV_ASR_OWNER_UNAVAILABLE;
 struct vv_confirmed_wake_context c;uint64_t now=0;int rc=idle_context(&c,&now);
 if(rc==-ESTALE||rc==-EBUSY)rc=-EPIPE;
 if(!rc&&!idle_context_match(&c,&r->context))rc=-EPIPE;
 if(!rc)rc=vv_wake_input_health(idle.queue,r,now);
 return rc;
}
static int idle_source_health(void *arg,const struct vv_wake_request *r){
 if(arg!=&idle)return -EINVAL;
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=idle_source_health_locked(r);pthread_mutex_unlock(&event_lock);return rc;
}
static int idle_source_take(void *arg,const struct vv_wake_request *r,struct k7_voice_frame *f){
 if(!f||arg!=&idle)return -EINVAL;
 memset(f,0,sizeof(*f));int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=idle_source_health_locked(r);
 if(!rc)rc=vv_wake_input_take(idle.queue,r,now_ms(),f);
 pthread_mutex_unlock(&event_lock);return rc;
}
static int idle_source_finish(void *arg,const struct vv_wake_request *r,bool cancel,
 uint64_t *first,uint64_t *next_sample){
 if(first)*first=0;
 if(next_sample)*next_sample=0;
 if(!first||!next_sample||arg!=&idle)return -EINVAL;
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=idle_ticket(r)?0:VV_WAKE_STALE;
 if(!rc){
  rc=idle_source_health_locked(r);
  if(cancel||rc)vv_wake_input_reset(idle.queue);
  uint64_t f=0,n=0;int closed=vv_wake_input_finish(idle.queue,r,now_ms(),&f,&n);
  if(!rc)rc=cancel?-ECANCELED:closed;
  if(!rc){*first=f;*next_sample=n;}
  /* Only the matching immutable ticket releases the borrowed owner pin. */
  idle.active=false;memset(&idle.request,0,sizeof(idle.request));
 }
 pthread_mutex_unlock(&event_lock);return rc;
}
int vv_voice_session_idle_register(const struct vv_voice_idle_ops *o){
 if(!o||!o->arg||!o->mode_epoch)return -EINVAL;
 struct vv_wake_input *q=NULL;int rc=vv_wake_input_create(&q);if(rc)return rc;
 int e=pthread_mutex_lock(&event_lock);if(e){vv_wake_input_destroy(q);return -e;}
 if(idle.queue)rc=-EBUSY;
 else {idle.ops=*o;idle.queue=q;idle.eligible=false;idle.mode_epoch=0;q=NULL;}
 pthread_mutex_unlock(&event_lock);vv_wake_input_destroy(q);return rc;
}
int vv_voice_session_idle_unregister(const struct vv_voice_idle_ops *o){
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=!idle_ops_match(o)?-ESTALE:idle.active?-EBUSY:0;
 if(!rc){vv_wake_input_destroy(idle.queue);idle.queue=NULL;memset(&idle.ops,0,sizeof(idle.ops));
  idle.eligible=false;idle.mode_epoch=0;}
 pthread_mutex_unlock(&event_lock);return rc;
}
int vv_voice_session_idle_select(const struct vv_voice_idle_ops *o,bool eligible,unsigned epoch){
 int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=!idle_ops_match(o)?-ESTALE:eligible&&!epoch?-EINVAL:0;
 if(!rc){
  if(idle.eligible!=eligible||idle.mode_epoch!=epoch)vv_wake_input_reset(idle.queue);
  idle.eligible=eligible;idle.mode_epoch=epoch;
 }
 pthread_mutex_unlock(&event_lock);return rc;
}
int vv_voice_session_idle_claim(const struct vv_voice_idle_ops *o,struct vv_pf_wake_source *out){
 if(!out)return -EINVAL;
 memset(out,0,sizeof(*out));int e=pthread_mutex_lock(&event_lock);if(e)return -e;
 int rc=!idle_ops_match(o)?-ESTALE:idle.active?-EBUSY:0;
 struct vv_confirmed_wake_context c;uint64_t now=0;
 if(!rc)rc=idle_context(&c,&now);
 if(!rc)rc=vv_wake_input_claim(idle.queue,&c,VV_WAKE_IDLE,now,&idle.request);
 if(rc==1){idle.active=true;*out=(struct vv_pf_wake_source){.arg=&idle,.request=idle.request,
  .health=idle_source_health,.take=idle_source_take,.finish=idle_source_finish};}
 pthread_mutex_unlock(&event_lock);return rc;
}
/* Called only by the existing KWS consumer. No second fanout consumer, and no
 * action dispatch: idle labels merely preserve one raw ASR confirmation task. */
static void idle_stage(const struct k7_voice_frame *frame,const struct k7_kws_detection *d){
 pthread_mutex_lock(&event_lock);
 if(idle.queue){
  struct vv_confirmed_wake_context c;uint64_t now=0;int rc=idle_context(&c,&now);
  if(rc)vv_wake_input_reset(idle.queue);
  else if(frame)(void)vv_wake_input_frame(idle.queue,&c,VV_WAKE_IDLE,frame,now);
  else if(d&&d->epoch==c.input_epoch&&d->keyword[0]){
   const char *label=d->keyword;if(*label=='@')label++;
   if(!strcmp(label,"wake")||!strcmp(label,"skin")||!strcmp(label,"stop"))
    (void)vv_wake_input_candidate(idle.queue,&c,d->end_sample,now);
  }
 }
 pthread_mutex_unlock(&event_lock);
}
static int next_model_epoch(void){
 uint_fast64_t prior=atomic_load(&model_sequence);
 do {if(prior==UINT64_MAX)return -EOVERFLOW;}
 while(!atomic_compare_exchange_weak(&model_sequence,&prior,prior+1));
 pthread_mutex_lock(&event_lock);idle.model_epoch=prior+1;
 if(idle.queue)vv_wake_input_reset(idle.queue);
 pthread_mutex_unlock(&event_lock);return 0;
}

uint32_t vv_voice_session_next_asr_epoch(void) {return next(&asr_epoch);}
uint32_t vv_voice_session_next_tx_generation(void) {return next(&tx_generation);}
static void *consume(void *unused) {
 (void)unused;struct k7_kws_config config; k7_kws_runtime *model=NULL;
 struct k7_voice_session *session=NULL;
 uint64_t began=now_ms();unsigned frames=0,hits=0,resets=0;int rc;
 rc=k7_kws_embedded_config(&config,hash,NULL);
 if(!rc)rc=k7_kws_create(&config,&model);
 if(!rc)rc=next_model_epoch();
 printf("VOICE_SESSION model_create=%d elapsed_ms=%llu\n",rc,
        (unsigned long long)(now_ms()-began));fflush(stdout);
 if(!rc&&atomic_load(&stopping))rc=-ECANCELED;
 if(!rc){
  
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
  const bool diagnostic_requested=k7_echo_runtime_requested();
#endif
  const struct k7_voice_session_config c={.tx_prebuffer_samples=3200,.render_reference=false
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
   ,.fifo_reference=diagnostic_requested,.timing_anchors=diagnostic_requested
#endif
  };
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  /* No live binding ships: NULL declines before a loan/APM allocation. */
  if(c.fifo_reference){
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
   struct k7_apm21_live_binding experiment;
   int take=k7_raw_vad_owner_take_apm21_diagnostic(atomic_load(&owner_epoch),&experiment);
   int pre=take==K7_APM21_STREAM_UNVERIFIED?take:take?take:
    k7_raw_vad_owner_preopen_residual(atomic_load(&owner_epoch),&experiment,&raw_vad_loan,&diagnostic);
#else
   int pre=k7_raw_vad_owner_preopen_residual(atomic_load(&owner_epoch),NULL,&raw_vad_loan,&diagnostic);
#endif
#line 303
   if(pre!=K7_APM21_STREAM_UNVERIFIED)rc=pre;
  }
  if(!rc)rc=k7sound_session_start(&c,&session);
  /* A failed start with retained physical state must not lose its owner. */
  if(session)atomic_store_explicit(&audio,session,memory_order_release);
#else
  rc=k7sound_session_start(&c,&session);
#endif
#line 300
  if(!rc)atomic_store_explicit(&audio,session,memory_order_release);
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
  if(!rc&&c.fifo_reference){
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
   int diagnostic_rc=diagnostic?k7_echo_runtime_attach_prepared(diagnostic,session,atomic_load(&owner_epoch)):
    k7_raw_vad_owner_open(session,atomic_load(&owner_epoch),&raw_vad_loan,&diagnostic);
   if(diagnostic_rc&&k7_echo_runtime_is_prepared(diagnostic))rc=diagnostic_rc;
#else
   int diagnostic_rc=k7_raw_vad_owner_open(session,atomic_load(&owner_epoch),&raw_vad_loan,&diagnostic);
#endif
#line 305
#else
   int diagnostic_rc=k7_echo_runtime_open(session,atomic_load(&owner_epoch),&diagnostic);
#endif
   printf("VOICE_SESSION echo_diagnostic_open=%d actions=0\n",diagnostic_rc);
  }
#endif
 }
 began=now_ms();
 while(!rc&&!atomic_load(&stopping)){
  struct k7_voice_session_status status={0};k7_voice_session_status(session,&status);
  if(status.finished){rc=status.result?status.result:-EPIPE;break;}
  if(!status.ready){
   uint64_t now=now_ms();
   if(now==UINT64_MAX||now<began||now-began>=5000){rc=-ETIMEDOUT;break;}
   wait_ms(1);continue;
  }
  atomic_store_explicit(&ready,true,memory_order_release);
  struct k7_voice_frame frame;int taken=k7_voice_session_take_kws(session,&frame);
  if(taken==-EPIPE){rc=k7_kws_reset(model);if(!rc)rc=next_model_epoch();++resets;continue;}
  if(taken<0){rc=taken;break;}
  if(!taken){wait_ms(1);continue;}
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
  k7_echo_runtime_tap(diagnostic,&frame);
#endif
  idle_stage(&frame,NULL);
  struct k7_kws_detection detection;
  int hit=k7_kws_feed(model,frame.pcm,frame.samples,frame.first_sample,frame.epoch,&detection);
  ++frames;
  if(hit<0){rc=hit;break;}
  if(hit==1){
   k7_voice_session_status(session,&status);
   bool fresh=detection.epoch==status.input_epoch&&
     detection.end_sample<=status.microphone_samples&&
     status.microphone_samples-detection.end_sample<=12800;
   if(fresh){++hits;idle_stage(NULL,&detection);}
   /* Never dispatch motion from an unconfirmed keyword fragment. In
    * particular speaker echo and negated commands need their own gate. */
   printf("VOICE_SESSION keyword=%s epoch=%u fresh=%u tx=%u action=0\n",
     detection.keyword,detection.epoch,(unsigned)fresh,(unsigned)status.tx_state);
   fflush(stdout);
  }
 }
 atomic_store_explicit(&ready,false,memory_order_release);
 if(session)k7_voice_session_stop(session);
 if(model)k7_kws_destroy(model);
 atomic_store(&result,rc);
 printf("VOICE_SESSION consumer_exit=%d frames=%u hits=%u resets=%u\n",rc,frames,hits,resets);
 atomic_store_explicit(&done,true,memory_order_release);return NULL;
}
int vv_voice_session_health(void) {
 if(!started)return -ENOTCONN;
 if(atomic_load_explicit(&done,memory_order_acquire)){
  int rc=atomic_load(&result);return rc?rc:-EPIPE;
 }
 if(atomic_load(&stopping))return -ECANCELED;
 struct k7_voice_session *session=vv_voice_session_audio();
 if(!session)return -ENOTCONN;
 struct k7_voice_session_status status={0};k7_voice_session_status(session,&status);
 if(status.finished||!status.running||!status.ready||status.stop_requested)
  return status.result?status.result:-EPIPE;
 return atomic_load_explicit(&ready,memory_order_acquire)?0:-EPIPE;
}
/* Return cleanup errors separately from the retired owner's run result. A
 * failed RX can be replaced only after its lifetime is proven to have ended. */
static int stop_owned(int *retired_result) {
 *retired_result=0;
 if(!started){
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
  return retire_unstarted();
#else
  return 0;
#endif
 }
 /* Wait for any short callback, then prevent new pins before audio teardown. */
 int event_error=pthread_mutex_lock(&event_lock);if(event_error)return -event_error;
 atomic_store(&stopping,true);
 if(idle.queue)vv_wake_input_reset(idle.queue);
 if(idle.active){pthread_mutex_unlock(&event_lock);return -EBUSY;}
 pthread_mutex_unlock(&event_lock);
 struct k7_voice_session *session=vv_voice_session_audio();
 if(session)k7_voice_session_stop(session);
#line 386
 if(!consumer_joined){
  int err=pthread_join(consumer,NULL);
  if(err)return -err; /* retain shared state when lifetime is uncertain */
  consumer_joined=true;
 }
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
 /* The sole tap producer has joined. Retain the physical owner if the
  * diagnostic consumer's borrowed-session lifetime is still uncertain. */
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 /* Near worker is paused but APM remains alive while physical cleanup occurs.
  * Its raw model loan and physical pointer stay owned on EVERY failure. */
 int pause_cleanup=k7_echo_runtime_pause_prepared(diagnostic,atomic_load(&owner_epoch));
 if(pause_cleanup)return pause_cleanup;
 int physical_join_result=0;
 session=vv_voice_session_audio();
 if(session){
  physical_join_result=k7_voice_session_join(session);
  struct k7_voice_session_status status={0};k7_voice_session_status(session,&status);
  if(!status.finished||!status.cleanup_ok||status.cleanup_error)return status.cleanup_error?status.cleanup_error:-EIO;
  int lifetime=k7_voice_session_apm21_retirement_check(session);if(lifetime)return lifetime;
 }
#endif
 int diagnostic_cleanup=k7_raw_vad_owner_close(&raw_vad_loan,&diagnostic);
#line 396
#else
 int diagnostic_cleanup=k7_echo_runtime_close(&diagnostic);
#endif
 if(diagnostic_cleanup)return diagnostic_cleanup;
#endif
 int rc=atomic_load(&result);
 session=vv_voice_session_audio();
 if(session){
  k7_voice_session_stop(session);
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  int joined=physical_join_result;
#else
  int joined=k7_voice_session_join(session);
#endif
#line 406
  struct k7_voice_session_status status={0};k7_voice_session_status(session,&status);
  /* A joined thread proves only its C lifetime ended. The physical owner
   * must separately prove amp/SAI/codec/platform/clock/I2C cleanup completed.
   * Retain the object and block a fresh owner when cleanup is unknown/bad. */
  if(!status.finished||!status.cleanup_ok||status.cleanup_error)
   return status.cleanup_error?status.cleanup_error:-EIO;
  int freed=k7_voice_session_destroy(session);
  if(freed)return freed;
  atomic_store_explicit(&audio,NULL,memory_order_release);
  if(joined&&joined!=-ECANCELED)rc=joined;
 }
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
 int owner_cleanup=k7_raw_vad_owner_voice_end(atomic_load(&owner_epoch));
 if(owner_cleanup)return owner_cleanup;
#endif
 started=false;
 *retired_result=rc==-ECANCELED?0:rc;return 0;
}
int vv_voice_session_stop(void) {
 int retired=0;int rc=stop_owned(&retired);return rc?rc:retired;
}
int vv_voice_session_restart(struct vv_native_runtime *runtime) {
 if(!runtime)return -EINVAL;
 if(!started)return -ENOTCONN;
 if(atomic_load(&runtime->cancelled))return -ECANCELED;
 int retired=0;int rc=stop_owned(&retired);
 if(rc)return rc;
 return vv_voice_session_start(runtime);
}
int vv_voice_session_start(struct vv_native_runtime *runtime) {
 if(!runtime)return -EINVAL;
 if(started)return -EALREADY;
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
 if(startup_retirement)return -EBUSY;
#endif
 if(atomic_load(&runtime->cancelled))return -ECANCELED;
 uint_fast64_t prior=atomic_load(&owner_sequence);
 do {if(prior==UINT64_MAX)return -EOVERFLOW;}
 while(!atomic_compare_exchange_weak(&owner_sequence,&prior,prior+1));
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
 int admission=k7_raw_vad_owner_voice_begin(prior+1);if(admission)return admission;
 startup_retirement=prior+1;
#endif
 atomic_store_explicit(&owner_epoch,prior+1,memory_order_release);
 atomic_store(&audio,NULL);atomic_store(&stopping,false);
 atomic_store(&done,false);atomic_store(&ready,false);atomic_store(&result,0);
 atomic_store(&asr_epoch,0);atomic_store(&tx_generation,0);
 pthread_attr_t attr;int err=pthread_attr_init(&attr);
 if(err){
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
  int cleanup=retire_unstarted();if(cleanup)return cleanup;
#endif
  return -err;
 }
 err=pthread_attr_setstacksize(&attr,131072);
#ifdef CONFIG_SMP
 if(!err){cpu_set_t cpus;CPU_ZERO(&cpus);
#if CONFIG_SMP_NCPUS > 6
  CPU_SET(6,&cpus);
#else
  CPU_SET(0,&cpus);
#endif
  err=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);}
#endif
 if(!err)err=pthread_create(&consumer,&attr,consume,NULL);
 pthread_attr_destroy(&attr);if(err){
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
  int cleanup=retire_unstarted();if(cleanup)return cleanup;
#endif
  return -err;
 }
 started=true;consumer_joined=false;
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
 startup_retirement=0; /* consumer is now owned by the normal joined path */
#endif
 uint64_t began=now_ms();
 while(!atomic_load_explicit(&ready,memory_order_acquire)){
  if(atomic_load_explicit(&done,memory_order_acquire)){
   int rc=atomic_load(&result);int cleanup=vv_voice_session_stop();
   return cleanup?cleanup:rc?rc:-EIO;
  }
  if(atomic_load(&runtime->cancelled)){int cleanup=vv_voice_session_stop();return cleanup?cleanup:-ECANCELED;}
  uint64_t now=now_ms();
  if(now==UINT64_MAX||now<began||now-began>=30000){int cleanup=vv_voice_session_stop();return cleanup?cleanup:-ETIMEDOUT;}
  wait_ms(1);
 }
 /* The worker may publish ready before create returns or while the owner
  * leaves the wait loop. Cancellation must still clean up that live owner. */
 if(atomic_load(&runtime->cancelled)){int cleanup=vv_voice_session_stop();return cleanup?cleanup:-ECANCELED;}
 return 0;
}
