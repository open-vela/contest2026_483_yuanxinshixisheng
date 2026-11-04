#include "vv_real_upload_private.h"
#include "vv_native_platform_wait_private.h"
#include "vv_real_shuiguang.h"
#include "vv_addressed_window.h"
#include "vv_prompt_admission.h"
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "vv_native_platform.h"
#include "vv_voice_settings.h"
#include "vv_aliyun_provider.h"
#include "vv_report_audio_k7.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
#include "vv_k7_platform_factory.h"
#include "vv_local_time.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_voice_session.h"
#include "voice_session.h"
#include "vv_paraformer_k7.h"
#include "vv_barge_route.h"
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "vv_near_experiment.h"
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "k7_apm21_product_startup.h"
#endif
#include "raw_vad_owner.h"
#include "echo_diag_runtime.h"
#endif
#endif
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
/* Confirmed document endpoint set; backend appends /api/v1 itself. */
#if K7_REAL_SHUIGUANG_PROFILE
static const char host[]=VV_REAL_BUSINESS_HOST,base[]=VV_REAL_BUSINESS_BASE;
#else
static const char host[]="eveaisia.com",base[]="/openvela";
#endif
static const char asr[]="/openvela/asr",tts[]="/openvela/tts";
struct vv_native_platform {
 struct vv_native_platform_config c;struct vv_native_client *client;
 struct vv_native_mode mode;struct vv_native_runtime runtime;
 struct vv_native_assessment assessment;struct vv_assessment_backend_adapter backend;
 struct vv_report_audio report;struct vv_report_audio_k7 speaker;
 unsigned char *pcm;bool bound;atomic_bool authenticated;atomic_uint_fast64_t expires;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 struct vv_dialogue dialogue;
 struct vv_reply_scope *reply_scope;uint64_t scope_owner;
 pthread_mutex_t event_scope_lock;bool event_scope_lock_ready,event_registered;
 bool event_reply_allowed;unsigned event_mode_epoch;
 atomic_bool event_enabled;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 struct vv_voice_event_ops events;
 struct vv_voice_idle_ops idle_events;bool idle_registered;
 struct vv_barge_route *barge;bool barge_attached;
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 bool near_round_finished;
#endif
 struct vv_near_experiment *near_experiment;struct vv_barge_driver near_driver;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 bool nominal_policy_registered;
#endif
#endif
#endif
 /* Confirmed-event callbacks are synchronous under the manager lifetime pin.
  * Unregister drains them before scope/platform memory is released. */
 vv_k7_secure_load_fn business_loader;void *business_loader_arg;
 uint64_t renew_after_ms,renew_last_ms;unsigned renew_failures;
 bool renew_clock_seen,renew_clock_failed;
#endif
};
static bool ready(void *);
/* Identity-check the original ops before dereferencing its opaque argument.
 * No public structure layout, registry or second audio owner is introduced. */
int vv_native_platform_local_setting(struct vv_native_runtime *runtime,const char *text,
 char *ack,size_t capacity,const struct vv_https_cancel *cancel){
 if(!runtime||runtime->config.ops.ready!=ready)return 0;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return -EINVAL;
 struct vv_voice_setting cmd;int rc=vv_voice_setting_parse(text,&cmd);
 /* A recognized bad value is user input rejection, not an owner failure.
  * Do not commit, and preserve cancellation and all other operational errors. */
 if(rc==-ERANGE){
  if(!ack||capacity<128)return -EINVAL;
  ack[0]=0;
  if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
  snprintf(ack,capacity,"音量范围是百分之一到百分之一百，请重新设置。");
  return 2;
 }
 if(rc<=0)return rc;
 if(!ack||capacity<128)return -EINVAL;
 ack[0]=0;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 const struct vv_speech_provider *speech=p->c.speech;
 if(!speech)return -ENOTSUP;
 if(cmd.kind==VV_SETTING_VOICE){
  bool applied=cmd.value!=0;
  rc=cmd.value<0?vv_aliyun_provider_toggle_voice(speech,cancel,&applied):
   vv_aliyun_provider_set_voice(speech,applied,cancel);
  if(!rc)snprintf(ack,capacity,"%s",applied?"已切换流萤音色。":"已恢复默认音色。");
 }else{
  unsigned applied=0;
  rc=vv_aliyun_provider_set_volume(speech,cmd.value,cmd.kind==VV_SETTING_VOLUME_RELATIVE,cancel,&applied);
  if(!rc){
   const char *message="音量已调整。";
   if(cmd.kind==VV_SETTING_VOLUME_RELATIVE){
    if(cmd.value>0)message=applied>=100?"音量已到最大。":"音量已调大。";
    else message=applied<=1?"音量已到最小。":"音量已调小。";
   }
   snprintf(ack,capacity,"%s",message);
  }
 }
 return rc?rc:1;
}

unsigned vv_native_platform_missing(struct vv_native_platform *p){
 if(!p)return VV_NATIVE_CONFIG;
 unsigned bits=p->c.missing(p->c.arg)&~(VV_NATIVE_AUTH|VV_NATIVE_ENDPOINT|VV_NATIVE_BUSINESS_AUTH);uint64_t utc=0;
 if(!p->c.utc_now(p->c.clock_arg,&utc)||!utc)bits|=VV_NATIVE_TIME;
 if(!p->c.tls.security_ready(p->c.tls.security_arg))bits|=VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA;
 uint64_t expires=atomic_load(&p->expires);
 if(!atomic_load(&p->authenticated)||!utc||expires<=utc||expires-utc<=60)bits|=p->c.speech?VV_NATIVE_BUSINESS_AUTH:VV_NATIVE_AUTH;
 return bits;
}
static unsigned missing(void *a){return vv_native_platform_missing(a);}
static bool ready(void *a){return !(missing(a)&~VV_NATIVE_BUSINESS_AUTH);}
static bool assessment_ready(void *a){return !missing(a);}
#if K7_REAL_SHUIGUANG_PROFILE
bool vv_native_platform_local_failure_ready(struct vv_native_runtime *runtime){
 if(!runtime||runtime->config.ops.ready!=ready)return false;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return false;
 return !(missing(p)&~(VV_NATIVE_NETWORK|VV_NATIVE_BUSINESS_AUTH));
}
#endif
/* Business login is synchronous under the platform owner. With a separate
 * speech provider, cap only this optional request so a missing backend cannot
 * consume a full speech-listening interval. The stack copy and pointer are
 * borrowed for this call only; ASR/TTS and photo upload limits stay unchanged.
 * Budget expiry remains HTTPS timeout, never an outer cancellation. */
static int login_business(struct vv_native_platform *p,
 const char *credential,const char *version,const char *proof,
 const struct vv_https_cancel *cancel){
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 if(!p->c.speech||!b->https)
  return vv_backend_client_login(b,credential,version,proof,cancel);
 struct vv_https_client limited=*b->https;
#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
 /* Domain DNS requires a 2000ms worst-case allowance. Keep the optional
  * business call bounded; speech transport limits are separate and unchanged. */
 const unsigned budget_ms=p->c.development_lan_http?500u:3000u;
 if(limited.limits.total_timeout_ms>budget_ms)limited.limits.total_timeout_ms=budget_ms;
#else
 if(limited.limits.total_timeout_ms>500u)limited.limits.total_timeout_ms=500u;
#endif
 struct vv_https_client *original=b->https;b->https=&limited;
 int rc=vv_backend_client_login(b,credential,version,proof,cancel);
 b->https=original;return rc;
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
/* Owner-only inspection. Other threads read the atomic readiness mirror and
 * must never touch the mutable backend token while an HTTP operation runs. */
static void sync_business_auth(struct vv_native_platform *p){
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 if(!b->token[0]||!b->expires_at){atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);}
}
static bool auth_cancelled(const struct vv_https_cancel *c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static void invalidate_business(struct vv_native_platform *p){
 vv_backend_client_clear_session(vv_native_client_backend(p->client));
 atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);
}
#if K7_REAL_SHUIGUANG_PROFILE && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
/* Invoked only by the serialized report owner, already holding runtime.busy.
 * The mandatory caller cancel callback retains deadline/epoch/PHOTO/HOLD gates.
 * Pure network loss is retryable on this read-only path; trust never is. */
static int wait_task_gate(struct vv_native_platform *p,
 const struct vv_https_cancel *cancel){
 if(auth_cancelled(cancel)||atomic_load(&p->runtime.cancelled))return -ECANCELED;
 uint64_t now=p->c.tls.now_ms(p->c.tls.now_arg);
 if(p->renew_clock_failed||now==UINT64_MAX||
    (p->renew_clock_seen&&now<p->renew_last_ms)){
  p->renew_clock_failed=true;invalidate_business(p);return VV_BACKEND_CLIENT_TIME;
 }
 p->renew_clock_seen=true;p->renew_last_ms=now;
 unsigned bits=missing(p)&~VV_NATIVE_BUSINESS_AUTH;
 if(bits&(VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA))return VV_BACKEND_CLIENT_TIME;
 if(bits==VV_NATIVE_NETWORK)return VV_HTTPS_EIO;
 return bits?-EACCES:0;
}
static bool wait_gate_soft_result(int rc){
 return !rc||rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED||
  rc==VV_HTTPS_EIO||rc==VV_HTTPS_ETIMEOUT||rc==VV_BACKEND_CLIENT_AUTH;
}
static int wait_gate_result(int rc,int gate){
 if(!gate)return rc;
 if(gate==VV_HTTPS_EIO)return rc?rc:gate;
 /* A STOP cancellation must not erase a concrete error already returned by
  * HTTP/login/loader. Only enumerated recoverable results yield to cancel. */
 if((gate==-ECANCELED||gate==VV_HTTPS_ECANCELLED)&&!wait_gate_soft_result(rc))return rc;
 return gate;
}
static int wait_task_once(struct vv_native_platform *p,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *cancel){
 int rc=wait_task_gate(p,cancel);if(rc)return rc;
 rc=vv_backend_client_task_wait(vv_native_client_backend(p->client),id,20,task,brief,cancel);
 sync_business_auth(p);
 if(rc==VV_BACKEND_CLIENT_AUTH)invalidate_business(p);
 int gate=wait_task_gate(p,cancel);
 /* Preserve concrete failures over incidental network loss and cancellation;
  * clock or owner safety failures still take precedence. */
 return wait_gate_result(rc,gate);
}
static int wait_task_refresh(struct vv_native_platform *p,
 const struct vv_https_cancel *cancel){
 struct vv_k7_secure_login secrets={{0},{0},{0}};
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 int rc=wait_task_gate(p,cancel);uint64_t utc=0;
 if(rc)goto done;
 if(!p->business_loader){rc=VV_BACKEND_CLIENT_AUTH;goto done;}
 invalidate_business(p);
 rc=p->business_loader(p->business_loader_arg,&secrets);
 if(!rc&&(!memchr(secrets.credential,0,sizeof(secrets.credential))||
    !memchr(secrets.version,0,sizeof(secrets.version))||
    !memchr(secrets.proof,0,sizeof(secrets.proof))||!secrets.credential[0]))rc=-EINVAL;
 int gate=wait_task_gate(p,cancel);
 rc=wait_gate_result(rc,gate);
 if(!rc)rc=login_business(p,secrets.credential,secrets.version,secrets.proof,cancel);
 /* Never publish auth or begin the retry while credentials remain in memory. */
 vv_https_secure_zero(&secrets,sizeof(secrets));
 gate=wait_task_gate(p,cancel);
 rc=wait_gate_result(rc,gate);
 if(!rc&&(!b->token[0]||!p->c.utc_now(p->c.clock_arg,&utc)||!utc||
    b->expires_at<=utc||b->expires_at-utc<=60))rc=VV_BACKEND_CLIENT_AUTH;
 if(!rc){
  atomic_store(&p->expires,b->expires_at);atomic_store(&p->authenticated,true);
  p->renew_failures=0;
  p->renew_after_ms=p->renew_last_ms>UINT64_MAX-30000?UINT64_MAX:p->renew_last_ms+30000;
 }
done:
 vv_https_secure_zero(&secrets,sizeof(secrets));
 if(rc){
  invalidate_business(p);
  unsigned delay=1000u<<(p->renew_failures<6?p->renew_failures:6);
  if(p->renew_failures<6)p->renew_failures++;
  p->renew_after_ms=p->renew_last_ms>UINT64_MAX-delay?UINT64_MAX:p->renew_last_ms+delay;
 }
 return rc;
}
static int wait_task_network(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *cancel){
 if(task)memset(task,0,sizeof(*task));
 if(brief)memset(brief,0,sizeof(*brief));
 struct vv_native_runtime *runtime=runtime_arg;
 if(!runtime||runtime->config.ops.ready!=ready||!id||!task||!brief||
    !cancel||!cancel->is_cancelled)return -EINVAL;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return -EINVAL;
 if(!atomic_load(&runtime->busy))return -EBUSY;
 int rc=wait_task_once(p,id,task,brief,cancel);
 if(rc==VV_BACKEND_CLIENT_AUTH){
  rc=wait_task_refresh(p,cancel);
  if(!rc)rc=wait_task_once(p,id,task,brief,cancel);
 }
 if(rc){memset(task,0,sizeof(*task));memset(brief,0,sizeof(*brief));}
 return rc;
}

static int upload_task_network(struct vv_native_platform *p,
 const struct vv_assessment_request *q,char accepted[37],bool *accepted_valid,
 const struct vv_https_cancel *cancel){
 *accepted_valid=false;
 int rc=wait_task_gate(p,cancel);if(rc)return rc;
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 rc=vv_backend_client_upload(b,q->capture_session,q->consent_ref,q->boundary_hex,
  q->idempotency_key,q->images,accepted,cancel);
 if(!rc)*accepted_valid=true; /* Backend validated the complete 202/UUID. */
 sync_business_auth(p);
 if(rc==VV_BACKEND_CLIENT_AUTH)invalidate_business(p);
 return wait_gate_result(rc,wait_task_gate(p,cancel));
}

/* The service thread owns ASR admission and controls; only this worker owns
 * mutable business TLS/token/renewal and the outer assessment cancel guard. */
static int idle_admission(void *,const struct vv_https_cancel *);
static void idle_release(void *);
struct wait_task_job {
 struct vv_native_platform *p;
 char id[37];unsigned epoch;
 bool uploading,upload_accepted;struct vv_assessment_request upload_request;char accepted[37];
 atomic_int upload_error;atomic_bool derived_cancel;
 struct vv_https_cancel outer;
 atomic_bool done,cancel;
 atomic_int main_error;
 int result;
 struct vv_backend_task task;
 struct vv_backend_brief brief;
};
static bool wait_worker_cancel(void *arg){
 struct wait_task_job *job=arg;
 return atomic_load_explicit(&job->cancel,memory_order_acquire)||
  job->outer.is_cancelled(job->outer.arg);
}
static void *wait_task_worker(void *arg){
 struct wait_task_job *job=arg;
 const struct vv_https_cancel cancel={wait_worker_cancel,job};
 if(job->uploading)job->result=upload_task_network(job->p,&job->upload_request,
  job->accepted,&job->upload_accepted,&cancel);
 else job->result=wait_task_network(&job->p->runtime,job->id,&job->task,&job->brief,&cancel);
 if(job->uploading)atomic_store_explicit(&job->upload_error,job->result,memory_order_release);
 /* Last use of all borrowed runtime, guard, TLS and result storage. The
  * release is the retirement proof even if the subsequent join reports error. */
 atomic_store_explicit(&job->done,true,memory_order_release);
 return NULL;
}
static int wait_main_safety(struct wait_task_job *job){
 struct vv_native_platform *p=job->p;
 if(atomic_load(&p->runtime.cancelled)||vv_native_mode_epoch(&p->mode)!=job->epoch)
  return -ECANCELED;
 /* Thread-safe platform evidence and atomic auth mirror only. Never renew_*,
  * backend session, or the mutable outer assessment callback on this thread. */
 unsigned bits=missing(p)&~VV_NATIVE_BUSINESS_AUTH;
 if(bits&(VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA))return VV_BACKEND_CLIENT_TIME;
 if(bits==VV_NATIVE_NETWORK)return VV_HTTPS_EIO;
 return bits?-EACCES:0;
}
static int wait_main_record(struct wait_task_job *job){
 int safety=wait_main_safety(job);
 if(safety){
  int seen=atomic_load_explicit(&job->main_error,memory_order_acquire);
  /* A transient failure observed by the ASR worker remains a failure even if
   * a later snapshot recovers and HTTP finishes. Upgrade network to hard loss. */
  while(!seen||(seen==VV_HTTPS_EIO&&safety!=VV_HTTPS_EIO)){
   if(atomic_compare_exchange_weak_explicit(&job->main_error,&seen,safety,
       memory_order_acq_rel,memory_order_acquire))break;
  }
 }
 return atomic_load_explicit(&job->main_error,memory_order_acquire);
}
static bool wait_yield_code(int rc){return rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED;}
static int wait_voice_stop_safety(struct wait_task_job *job){
 struct vv_native_platform *p=job->p;
 int sticky=atomic_load_explicit(&job->main_error,memory_order_acquire);
 if(sticky&&sticky!=VV_HTTPS_EIO&&!wait_yield_code(sticky))return sticky;
 unsigned bits=missing(p)&~(VV_NATIVE_NETWORK|VV_NATIVE_BUSINESS_AUTH);
 if(bits&(VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA))return VV_BACKEND_CLIENT_TIME;
 if(bits)return -EACCES;
 if(atomic_load(&p->runtime.cancelled)||
    vv_native_mode_epoch(&p->mode)!=job->epoch+1u||p->mode.assessment_active)
  return -ECANCELED;
 int held=p->mode.ops.status(p->mode.ops.arg,0);
 if(held)return held<0?held:-EACCES;
 if(atomic_load(&p->runtime.cancelled)||vv_native_mode_epoch(&p->mode)!=job->epoch+1u||
    p->mode.assessment_active)return -ECANCELED;
 return 0;
}
static bool wait_stop_worker_soft(int rc){
 /* Only original read-retry network failures and cancellation can coexist
  * with an independently proven applied voice STOP. Every other error wins. */
 return !rc||wait_yield_code(rc)||rc==VV_HTTPS_EIO||rc==VV_HTTPS_ETIMEOUT;
}
static bool wait_main_cancel(void *arg){
 struct wait_task_job *job=arg;
 int error=wait_main_record(job);
 if(error)return true;
 if(job->uploading&&atomic_load_explicit(&job->done,memory_order_acquire)&&
    atomic_load_explicit(&job->upload_error,memory_order_acquire)){
  atomic_store_explicit(&job->derived_cancel,true,memory_order_release);return true;
 }
 return (!job->uploading&&atomic_load_explicit(&job->done,memory_order_acquire))||
  atomic_load_explicit(&job->cancel,memory_order_acquire);
}
static int run_business_job(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *outer,
 int (*started)(void *,const struct vv_https_cancel *),void *started_arg,
 const struct vv_assessment_request *upload,char accepted[37]){
 if(accepted)memset(accepted,0,37);
 if(task)memset(task,0,sizeof(*task));
 if(brief)memset(brief,0,sizeof(*brief));
 struct vv_native_runtime *runtime=runtime_arg;
 if(!runtime||runtime->config.ops.ready!=ready||
    (upload?!accepted:(!id||!task||!brief))||
    !outer||!outer->is_cancelled)return -EINVAL;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return -EINVAL;
 if(!atomic_load(&runtime->busy))return -EBUSY;
 if(!upload){size_t id_len=0;while(id_len<37&&id[id_len])id_len++;
  if(id_len!=36)return -EINVAL;}
 struct wait_task_job *job=calloc(1,sizeof(*job));if(!job)return -ENOMEM;
 job->p=p;job->epoch=vv_native_mode_epoch(&p->mode);job->outer=*outer;
 job->uploading=upload!=NULL;
 if(upload)job->upload_request=*upload;else memcpy(job->id,id,37);
 atomic_init(&job->done,false);atomic_init(&job->cancel,false);
 atomic_init(&job->main_error,0);atomic_init(&job->upload_error,0);atomic_init(&job->derived_cancel,false);
 int rc=wait_main_record(job);
 if(rc){vv_https_secure_zero(job,sizeof(*job));free(job);return rc;}
 pthread_attr_t attr;int e=pthread_attr_init(&attr);
 if(e){vv_https_secure_zero(job,sizeof(*job));free(job);return -e;}
 e=pthread_attr_setstacksize(&attr,32768);
 if(e){(void)pthread_attr_destroy(&attr);vv_https_secure_zero(job,sizeof(*job));free(job);return -e;}
 pthread_t thread;e=pthread_create(&thread,&attr,wait_task_worker,job);
 int destroyed=pthread_attr_destroy(&attr);
 if(e){vv_https_secure_zero(job,sizeof(*job));free(job);return -e;}
 const struct vv_https_cancel main_cancel={wait_main_cancel,job};
 rc=destroyed?-destroyed:0;bool voice_stop=false,voice_stop_requested=false;
 /* Start the HTTP worker before this optional main-thread local cue. Its
  * independent cancel token can yield to a completed request, never calls the
  * outer assessment guard, and retains sticky safety/STOP failures. */
 if(!rc&&started&&(job->uploading||!atomic_load_explicit(&job->done,memory_order_acquire))){
  rc=wait_main_record(job);
  if(!rc){
   int cue=started(started_arg,&main_cancel);
   int safety=wait_main_record(job);
   if(safety)rc=(safety==VV_HTTPS_EIO&&cue&&!wait_yield_code(cue))?cue:safety;
   else if(cue&&!( !job->uploading&&wait_yield_code(cue)&&atomic_load_explicit(&job->done,memory_order_acquire)))rc=cue;
  }
 }
 if(rc)atomic_store_explicit(&job->cancel,true,memory_order_release);
 while(!rc&&!atomic_load_explicit(&job->done,memory_order_acquire)){
  rc=wait_main_record(job);
  if(rc)break; /* In particular, never start network ASR while offline. */
  /* Upload preserves the original no-ASR POST phase. Only the two main-thread
   * completion cues overlap; never nest a wait worker or touch shared ASR. */
  if(job->uploading){p->c.sleep_ms(p->c.arg,50);continue;}
  int admitted=idle_admission(p,&main_cancel);
  if(admitted>1){rc=-EPROTO;break;}
  if(admitted<0){
   int safety=wait_main_record(job);
   if(safety&&(safety!=VV_HTTPS_EIO||wait_yield_code(admitted)))rc=safety;
   else if(!safety&&wait_yield_code(admitted)&&atomic_load_explicit(&job->done,memory_order_acquire))break;
   else rc=admitted;
   break;
  }
  if(!admitted){p->c.sleep_ms(p->c.arg,50);continue;}
  int control=vv_native_runtime_wait_control(runtime,p->c.samples,&main_cancel);
  idle_release(p);
  if(control==VV_REAL_WAIT_VOICE_STOP){
   voice_stop_requested=true;
   int stopped=wait_voice_stop_safety(job);
   if(stopped)rc=stopped;
   else{voice_stop=true;rc=VV_REAL_WAIT_VOICE_STOP;}
   break;
  }
  int safety=wait_main_record(job);
  if(safety){rc=(safety==VV_HTTPS_EIO&&control&&!wait_yield_code(control))?control:safety;break;}
  if(control){
   /* A finalized STOP changes epoch and was caught above. Only this exact
    * done-triggered cancellation yields; all other errors remain failures. */
   if(wait_yield_code(control)&&atomic_load_explicit(&job->done,memory_order_acquire))break;
   rc=control;break;
  }
  if(!atomic_load_explicit(&job->done,memory_order_acquire))p->c.sleep_ms(p->c.arg,50);
 }
 if(rc)atomic_store_explicit(&job->cancel,true,memory_order_release);
 /* Never return while the worker can still call the borrowed outer guard. */
 while(!atomic_load_explicit(&job->done,memory_order_acquire))p->c.sleep_ms(p->c.arg,50);
 e=pthread_join(thread,NULL);
 if(e){
  /* Preserve heap storage and fail closed: unknown joinability must not free
   * worker state. done proves external borrows retired, not a successful join. */
  printf("NATIVE_WAIT join_rc=%d retired=1 storage_retained=1\n",e);
  return -e;
 }
 if(voice_stop_requested){
  /* Expected one-step STOP epoch is not generic cancellation. No rearm,
   * cancel clearing or motion is permitted here, including on success. */
  int stopped=wait_voice_stop_safety(job);
  if(!wait_stop_worker_soft(job->result))rc=job->result;
  else if(voice_stop)rc=stopped?stopped:VV_REAL_WAIT_VOICE_STOP;
  else if(stopped&&!wait_yield_code(stopped)&&stopped!=VV_HTTPS_EIO)rc=stopped;
 }else{
  /* Do not turn a concrete worker/control hard error into a network retry. */
  if(!rc||((rc==VV_HTTPS_EIO||(wait_yield_code(rc)&&!wait_stop_worker_soft(job->result)))&&job->result&&job->result!=VV_HTTPS_EIO&&
           !wait_yield_code(job->result)))rc=job->result;
  if(job->uploading&&atomic_load_explicit(&job->derived_cancel,memory_order_acquire)&&
     wait_yield_code(rc)&&job->result)rc=job->result;
  int safety=wait_main_record(job);
  if(safety&&(safety!=VV_HTTPS_EIO||!rc||wait_yield_code(rc)))rc=safety;
 }
 if(job->uploading){if(job->upload_accepted)memcpy(accepted,job->accepted,37);}
 else if(!rc){*task=job->task;*brief=job->brief;}
 vv_https_secure_zero(job,sizeof(*job));free(job);
 return rc;
}
int vv_native_platform_wait_task_progress(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *outer,
 int (*started)(void *,const struct vv_https_cancel *),void *started_arg){
 return run_business_job(runtime_arg,id,task,brief,outer,started,started_arg,NULL,NULL);
}
int vv_native_platform_upload_task_progress(void *runtime_arg,
 const struct vv_assessment_request *q,char accepted[37],const struct vv_https_cancel *outer,
 int (*started)(void *,const struct vv_https_cancel *),void *started_arg){
 if(!q){if(accepted)memset(accepted,0,37);return -EINVAL;}
 return run_business_job(runtime_arg,NULL,NULL,NULL,outer,started,started_arg,q,accepted);
}
int vv_native_platform_wait_task(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *outer){
 return vv_native_platform_wait_task_progress(runtime_arg,id,task,brief,outer,NULL,NULL);
}

#endif

static int maintain_business(void *arg,const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(!p->business_loader)return 0;
 bool idle=false;
 if(!atomic_compare_exchange_strong(&p->runtime.busy,&idle,true))return -EBUSY;
 int rc=0;struct vv_k7_secure_login secrets={{0},{0},{0}};
 if(p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy)){
  rc=-EBUSY;goto done;
 }
 sync_business_auth(p);
 if(auth_cancelled(cancel)){rc=-ECANCELED;goto done;}
 uint64_t now=p->c.tls.now_ms(p->c.tls.now_arg),utc=0;
 if(p->renew_clock_failed||now==UINT64_MAX||
    (p->renew_clock_seen&&now<p->renew_last_ms)){
  p->renew_clock_failed=true;invalidate_business(p);rc=VV_BACKEND_CLIENT_TIME;goto done;
 }
 p->renew_clock_seen=true;p->renew_last_ms=now;
 if((missing(p)&~(VV_NATIVE_AUTH|VV_NATIVE_BUSINESS_AUTH))||
    !p->c.utc_now(p->c.clock_arg,&utc)||!utc){rc=-EAGAIN;goto done;}
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 bool valid=b->token[0]&&b->expires_at>utc&&b->expires_at-utc>60;
 if(valid&&b->expires_at-utc>120){p->renew_failures=0;rc=0;goto done;}
 /* A rejected token overrides the success cooldown; failed logins retain
  * their backoff even if another question arrives immediately afterwards. */
 if(now<p->renew_after_ms&&(valid||p->renew_failures)){rc=-EAGAIN;goto done;}
 atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);
 rc=p->business_loader(p->business_loader_arg,&secrets);
 if(!rc&&(!memchr(secrets.credential,0,sizeof(secrets.credential))||
    !memchr(secrets.version,0,sizeof(secrets.version))||
    !memchr(secrets.proof,0,sizeof(secrets.proof))||!secrets.credential[0]))rc=-EINVAL;
 if(!rc&&auth_cancelled(cancel))rc=-ECANCELED;
 if(!rc)rc=login_business(p,secrets.credential,secrets.version,secrets.proof,cancel);
 /* Wipe before publishing readiness or returning to ordinary speech work. */
 vv_https_secure_zero(&secrets,sizeof(secrets));
 uint64_t ended=p->c.tls.now_ms(p->c.tls.now_arg);
 if(ended==UINT64_MAX||ended<now){p->renew_clock_failed=true;rc=VV_BACKEND_CLIENT_TIME;}
 else p->renew_last_ms=ended;
 if(auth_cancelled(cancel))rc=-ECANCELED;
 if(!rc&&(!b->token[0]||!p->c.utc_now(p->c.clock_arg,&utc)||
    b->expires_at<=utc||b->expires_at-utc<=60))rc=VV_BACKEND_CLIENT_AUTH;
 if(rc){
  invalidate_business(p);
  unsigned delay=1000u<<(p->renew_failures<6?p->renew_failures:6);
  if(p->renew_failures<6)p->renew_failures++;
  p->renew_after_ms=ended>UINT64_MAX-delay?UINT64_MAX:ended+delay;
  printf("NATIVE_BUSINESS refresh_rc=%d retry_ms=%u\n",rc,delay);
 }else{
  atomic_store(&p->expires,b->expires_at);atomic_store(&p->authenticated,true);
  p->renew_failures=0;p->renew_after_ms=ended>UINT64_MAX-30000?UINT64_MAX:ended+30000;
  puts("NATIVE_BUSINESS refresh_rc=0");
 }
done:
 vv_https_secure_zero(&secrets,sizeof(secrets));
 atomic_store(&p->runtime.busy,false);return rc;
}
int vv_native_platform_set_business_loader(struct vv_native_platform *p,
 vv_k7_secure_load_fn load,void *arg){
 if(!p||!load)return -EINVAL;
 if(p->bound)return -EBUSY;
 p->business_loader=load;p->business_loader_arg=arg;return 0;
}
#endif
/* Mode dispatch is serial with ASR/report. Epoch invalidation in mode already
 * cancels eligibility; no concurrent network/TTS call exists here to interrupt. */
static void mode_cancel(void *a){(void)a;}
static void mode_quiet(void *a){(void)a;} /* No TTS overlaps command listening. */
static int claim(void*a){struct vv_native_platform*p=a;return p->c.claim(p->c.arg);}
static void release(void*a){struct vv_native_platform*p=a;p->c.release(p->c.arg);}
static int make(void*a,struct vv_assessment_request*q){struct vv_native_platform*p=a;return p->c.make_request(p->c.arg,q);}
static void release_request(void*a,struct vv_assessment_request*q){struct vv_native_platform*p=a;p->c.release_request(p->c.arg,q);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 sync_business_auth(p);
#endif
}
static void sleep_ms(void*a,unsigned ms){struct vv_native_platform*p=a;p->c.sleep_ms(p->c.arg,ms);}
static void wait_ms(void*a,unsigned ms,const struct vv_https_cancel*c){if(!(c&&c->is_cancelled&&c->is_cancelled(c->arg)))sleep_ms(a,ms);}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
static int dialogue_ask(void*a,const char*text,char*answer,size_t capacity,const struct vv_https_cancel*cancel){
 struct vv_native_platform*p=a;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(p->c.speech&&p->c.speech->chat){
  if(answer&&capacity)vv_https_secure_zero(answer,capacity);
  if(!ready(p))return -EACCES;
  return p->c.speech->chat(p->c.speech->arg,text,answer,capacity,cancel);
 }
#endif
 if(!assessment_ready(p))return -EACCES;
 int rc=vv_backend_client_chat(vv_native_client_backend(p->client),text,answer,capacity,cancel);
 sync_business_auth(p);return rc;
}
static int dialogue_speak(void*a,const char*text,const struct vv_https_cancel*cancel){
 struct vv_native_platform*p=a;return vv_native_client_speak(p->client,text,cancel);
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
static int dialogue_local_reply(void*a,const char*question,char*answer,size_t capacity,
 const struct vv_https_cancel*cancel,const struct vv_reply_scope_token*scope){
 struct vv_native_platform*p=a;
 enum vv_local_time_kind kind=vv_local_time_query(question);
 if(kind==VV_LOCAL_TIME_NONE)return 0;
 if(answer&&capacity)vv_https_secure_zero(answer,capacity);
 if(auth_cancelled(cancel))return -ECANCELED;
 if(!ready(p))return -EACCES;
 uint64_t utc=0;
 if(!p->c.utc_now(p->c.clock_arg,&utc)||!utc)return -EACCES;
 char literal[160];
 int rc=vv_local_time_format(kind,utc,literal,sizeof(literal));
 if(!rc&&!ready(p))rc=-EACCES;
 if(!rc&&auth_cancelled(cancel))rc=-ECANCELED;
 if(!rc)rc=p->c.speech->speak_literal(p->c.speech->arg,literal,answer,capacity,cancel,scope);
 vv_https_secure_zero(literal,sizeof(literal));
 if(rc&&answer&&capacity)vv_https_secure_zero(answer,capacity);
 return rc?rc:1;
}
static int dialogue_stream(void*a,const char*text,char*answer,size_t capacity,const struct vv_https_cancel*cancel){
 struct vv_native_platform*p=a;
 if(answer&&capacity)vv_https_secure_zero(answer,capacity);
 if(!ready(p))return -EACCES;
 return p->c.speech->chat_speak(p->c.speech->arg,text,answer,capacity,cancel);
}
#endif
#if defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)

static int event_target_locked(struct vv_native_platform *p,
 const struct vv_voice_event_state *audio,struct vv_confirmed_wake_context *out){
 if(!atomic_load(&p->event_enabled)||!p->event_reply_allowed||!p->reply_scope||
    !audio->tx_active||p->event_mode_epoch!=vv_native_mode_epoch(&p->mode))return -EAGAIN;
 struct vv_reply_scope_status s;struct vv_dialogue_reply_status d;
 if(vv_reply_scope_snapshot(p->reply_scope,&s)||vv_dialogue_reply_snapshot(&p->dialogue,&d))
  return -EIO;
 if(s.phase!=VV_SCOPE_BOUND||d.phase!=VV_DIALOGUE_REPLY_SPEAKING||
    !d.id||d.id!=s.token.reply_id||s.token.owner_epoch!=audio->owner_epoch||
    !s.tx_generation||s.tx_generation!=audio->tx_generation)return -ESTALE;
 *out=(struct vv_confirmed_wake_context){.owner_epoch=audio->owner_epoch,
  .input_epoch=audio->input_epoch,.mode_epoch=vv_native_mode_epoch(&p->mode),
  .scope_incarnation=s.token.incarnation,.lease_serial=s.token.serial,
  .asr_epoch=s.token.asr_epoch,.reply_id=d.id,.tx_generation=s.tx_generation};
 return 0;
}
static int event_target(void *arg,const struct vv_voice_event_state *audio,
 struct vv_confirmed_wake_context *out){
 struct vv_native_platform *p=arg;
 int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
 int rc=event_target_locked(p,audio,out);
 pthread_mutex_unlock(&p->event_scope_lock);return rc;
}
static int event_submit(void *arg,const struct vv_voice_event_state *audio,
 const struct vv_confirmed_wake_event *event){
 struct vv_native_platform *p=arg;struct vv_confirmed_wake_context current;
 int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
 int rc=event_target_locked(p,audio,&current);
 const struct vv_confirmed_wake_context *old=&event->context;
 if(!rc&&(!event->full_wake||event->negated||event->source!=VV_WAKE_CLEAN_FULL_KWS||
    old->owner_epoch!=current.owner_epoch||old->input_epoch!=current.input_epoch||
    old->mode_epoch!=current.mode_epoch||old->scope_incarnation!=current.scope_incarnation||
    old->lease_serial!=current.lease_serial||old->asr_epoch!=current.asr_epoch||
    old->reply_id!=current.reply_id||old->tx_generation!=current.tx_generation||
    !old->model_epoch||!old->clean_epoch||!old->alignment_epoch))rc=-ESTALE;
 if(!rc){
  /* Epoch values alone are not acoustic proof. The independent gate must
   * revalidate its registered clean stream and consume this exact ticket. */
  current.model_epoch=old->model_epoch;current.clean_epoch=old->clean_epoch;
  current.alignment_epoch=old->alignment_epoch;
  int accepted=p->c.consume_confirmed_wake(p->c.confirmed_wake_arg,event,&current,audio->now_ms);
  if(accepted==1){
   /* Still inside the manager pin. A mode/input invalidation may have raced
    * qualification; consuming the old ticket does not override that change. */
   struct k7_voice_session *session=vv_voice_session_audio();
   if(vv_voice_session_owner_epoch(session)!=current.owner_epoch||
      k7_voice_session_input_epoch(session)!=current.input_epoch||
      k7_voice_session_tx_generation(session)!=current.tx_generation||
      vv_native_mode_epoch(&p->mode)!=current.mode_epoch)rc=-ESTALE;
   else rc=vv_dialogue_cancel_reply_id(&p->dialogue,current.reply_id);
  }else rc=accepted;
  if(rc>1)rc=-EPROTO;
 }
 pthread_mutex_unlock(&p->event_scope_lock);return rc;
}
/* Only the new local-idle entry needs this rejection. PHOTO still requires
 * TRACK; do not turn its legacy mode mismatch into global cancellation. */
static int idle_command_admit(void *arg,enum k7_voice_intent intent,unsigned epoch,
 const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(intent!=K7_VOICE_PHOTO)return 0;
 if(auth_cancelled(cancel)||epoch!=vv_native_mode_epoch(&p->mode))return -ECANCELED;
 if(p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy))return 0;
 int idle_status=p->mode.ops.status(p->mode.ops.arg,0);
 if(auth_cancelled(cancel)||epoch!=vv_native_mode_epoch(&p->mode))return -ECANCELED;
 if(!idle_status&&!p->runtime.pending&&!p->mode.assessment_active&&!atomic_load(&p->mode.busy))
  return -EBUSY;
 return 0; /* Every non-idle path retains the existing mode_apply result. */
}
static unsigned idle_epoch(void *arg){
 struct vv_native_platform *p=arg;return vv_native_mode_epoch(&p->mode);
}
static int barge_observe(void *arg,struct vv_barge_observation *out){
 struct vv_native_platform *p=arg;
 struct vv_voice_event_state st;
 if(vv_voice_session_observe(&st))return VV_SCOPE_OWNER_UNSAFE;
 int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
 unsigned epoch=vv_native_mode_epoch(&p->mode);
 bool allowed=p->event_reply_allowed&&p->event_mode_epoch==epoch&&
   !atomic_load(&p->runtime.cancelled);
 pthread_mutex_unlock(&p->event_scope_lock);
 if(!allowed)return VV_SCOPE_STALE;
 *out=(struct vv_barge_observation){.owner_epoch=st.owner_epoch,.microphone_next=st.microphone_samples,
  .now_ms=st.now_ms,.input_epoch=st.input_epoch,.mode_epoch=epoch,.live_tx_generation=st.tx_generation,
  .rx_live=true,.photo_busy=false,.tx_active=st.tx_active};
 return 0;
}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT && defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
static int near_reset_worker(void *arg);
#endif
static int idle_admission(void *arg,const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 if(p->barge){int admission=vv_barge_route_admit(p->barge);
  if(admission!=2)return admission;}
 if(!p->c.always_wake)return 1;
#if defined(K7_ADDRESSED_WINDOW) && K7_ADDRESSED_WINDOW
 bool occupied=p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy);
 if(vv_addressed_window_expired(p->dialogue.awake,occupied,
      p->dialogue.last_activity,p->dialogue.ops.now_ms(p->dialogue.ops.arg))){
  int slept=vv_dialogue_sleep(&p->dialogue);if(slept)return slept;
  printf("DIALOGUE_GATE waiting_wake=1 reason=idle idle_ms=%u\n",VV_ADDRESSED_IDLE_MS);
 }
#endif
 /* Only this serial service owner examines PHOTO state or pipeline mode. */
 bool idle_mode=!p->runtime.pending&&!p->mode.assessment_active&&
  !atomic_load(&p->mode.busy)&&p->mode.ops.status(p->mode.ops.arg,0)==0;
#if defined(K7_ADDRESSED_WINDOW) && K7_ADDRESSED_WINDOW
 /* Conversation sleep must not halt physical tracking. KWS may run while
  * tracking, provided PHOTO and a mode transition do not own the operation. */
 idle_mode=idle_mode||(!occupied&&!p->dialogue.awake);
#endif
 int rc=vv_voice_session_idle_select(&p->idle_events,idle_mode,vv_native_mode_epoch(&p->mode));
 if(rc)return rc;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 /* The explicit policy admits ordinary questions without a greeting TX.
  * After a completed round, retire the old PF/route/tail before admitting
  * the next ordinary capture. Barge/PF handoff above retains priority. */
 if(p->nominal_policy_registered&&p->c.always_wake&&p->c.scoped_dialogue&&p->dialogue.awake){
  if(!p->dialogue.reply_sequence)return 1;
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
  if(!p->near_experiment||vv_voice_session_health())return VV_SCOPE_OWNER_UNSAFE;
  if(p->near_round_finished){
   int reset=vv_near_experiment_rearm(p->near_experiment,near_reset_worker,p);
   if(reset==-EAGAIN||reset==-EBUSY)return 0;
   if(reset)return reset;
   p->near_round_finished=false;
  }
  return 1;
#endif
 }
#endif
 if(!idle_mode)return 1;
 struct vv_pf_wake_source source;
 rc=vv_voice_session_idle_claim(&p->idle_events,&source);
 /* Candidate expired, was overrun, or lost continuity before claim: wait for
  * a new complete candidate, never turn a raw keyword into an action. */
 if(rc==-ETIMEDOUT||rc==VV_WAKE_STALE||rc==-ESTALE||rc==-EPIPE||rc==-ENOSPC)return 0;
 if(rc!=1)return rc;
 rc=vv_paraformer_k7_wake_arm(&source);
 if(rc){uint64_t first=0,next=0;(void)source.finish(source.arg,&source.request,true,&first,&next);return rc;}
 return 1;
}
static void idle_release(void *arg){
 struct vv_native_platform *p=arg;
 if(p->barge)(void)vv_barge_route_release(p->barge);
 (void)vv_paraformer_k7_wake_disarm();
 if(p->c.always_wake)(void)vv_voice_session_idle_select(&p->idle_events,false,vv_native_mode_epoch(&p->mode));
}
static int scoped_current_owner(uint64_t *owner){
 if(vv_voice_session_health())return VV_SCOPE_OWNER_UNSAFE;
 struct k7_voice_session *audio=vv_voice_session_audio();
 uint64_t e=vv_voice_session_owner_epoch(audio);
 if(!e)return VV_SCOPE_OWNER_UNSAFE;
 *owner=e;return 0;
}
static int scoped_listen_admit(void *arg){
 struct vv_native_platform *p=arg;uint64_t owner=0;
 int rc=scoped_current_owner(&owner);if(rc)return rc;
 if(!p->reply_scope)return 0;
 struct vv_reply_scope_status s;
 if(vv_reply_scope_snapshot(p->reply_scope,&s))return VV_SCOPE_OWNER_UNSAFE;
 if(s.phase!=VV_SCOPE_IDLE&&s.phase!=VV_SCOPE_CLOSED)return VV_SCOPE_OWNER_UNSAFE;
 if(s.tx_generation&&(!s.tx_retired||!s.speaker_joined))return VV_SCOPE_OWNER_UNSAFE;
 if(owner!=p->scope_owner){
  /* Only a new healthy physical owner and fully retired prior turn may reset
   * the ASR epoch namespace. Quarantine never reaches this branch. */
  if(vv_reply_scope_set_owner(p->reply_scope,owner))return VV_SCOPE_OWNER_UNSAFE;
  p->scope_owner=owner;
 }else if(s.tx_generation&&!s.handoff_eligible)return VV_SCOPE_OWNER_UNSAFE;
 return 0;
}
#if defined(CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL) || defined(CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE)
struct time_idle {struct vv_native_platform *p;const struct vv_https_cancel *cancel;
 uint64_t owner;unsigned mode_epoch;};
static int time_idle_health(struct time_idle *x){
 struct vv_native_platform *p=x->p;uint64_t owner=0;
 if(auth_cancelled(x->cancel))return -ECANCELED;
 if(vv_native_mode_epoch(&p->mode)!=x->mode_epoch||p->runtime.pending||
    p->mode.assessment_active||atomic_load(&p->mode.busy))return -EAGAIN;
 if(scoped_current_owner(&owner)||owner!=x->owner)return VV_SCOPE_OWNER_UNSAFE;
 struct k7_voice_session *audio=vv_voice_session_audio();struct k7_voice_session_status status={0};
 k7_voice_session_status(audio,&status);
 if(!status.running||!status.ready||status.stop_requested||status.finished||status.result)return VV_SCOPE_OWNER_UNSAFE;
 if(status.tx_state!=K7_VOICE_TX_IDLE||status.asr_epoch)return -EBUSY;
 struct vv_dialogue_reply_status reply={0};
 if(atomic_load(&p->dialogue.busy)||vv_dialogue_reply_snapshot(&p->dialogue,&reply)||
    reply.phase!=VV_DIALOGUE_REPLY_IDLE)return -EBUSY;
 if(p->reply_scope){
  struct vv_reply_scope_status scope={0};
  if(vv_reply_scope_snapshot(p->reply_scope,&scope)||p->scope_owner!=owner)return VV_SCOPE_OWNER_UNSAFE;
  if(scope.phase!=VV_SCOPE_IDLE&&scope.phase!=VV_SCOPE_CLOSED)return VV_SCOPE_OWNER_UNSAFE;
  if(scope.phase==VV_SCOPE_CLOSED&&(!scope.reply_retired||(scope.tx_generation&&!scope.handoff_eligible)))return VV_SCOPE_OWNER_UNSAFE;
  if(scope.tx_generation&&(!scope.tx_retired||!scope.speaker_joined||!scope.rx_live))return VV_SCOPE_OWNER_UNSAFE;
 }
 return 0;
}
static bool time_idle_cancelled(void *arg){return time_idle_health(arg)!=0;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
static int maintain_entropy(struct vv_native_platform*p,bool safe,const struct vv_https_cancel*cancel){
 if(!p->c.entropy_status)return 0;
 if(auth_cancelled(cancel))return -ECANCELED;
 int state=p->c.entropy_status(p->c.entropy_arg);
 if(state==-EBUSY)return 1; /* wait, not ordinary-call permission */
 if(state<0)return state;
 if(state>1)return -EPROTO;
 if(!state)return 0; /* Outside final600000ms: no manager/scope lock or raw. */
 if(!safe||p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy))return 0;
 bool expected=false;
 if(!atomic_compare_exchange_strong(&p->runtime.busy,&expected,true))return 0;
 struct time_idle x={p,cancel,0,vv_native_mode_epoch(&p->mode)};
 bool refreshed=false;
 int rc=scoped_current_owner(&x.owner);
 if(!rc)rc=time_idle_health(&x);
 if(!rc){
  struct vv_https_cancel token={time_idle_cancelled,&x};
  refreshed=true;
  rc=p->c.entropy_refresh(p->c.entropy_arg,&token);
  int after=time_idle_health(&x);
  if(after&&rc!=-ECANCELED)rc=after;
 }
 atomic_store(&p->runtime.busy,false);
 if(auth_cancelled(cancel))return -ECANCELED;
 /* Busy before any refresh is safe-point deferral. Refresh itself must
  * classify outer-entry busy as1; raw -EBUSY is a fatal negative result. */
 if(!refreshed&&(rc==-EAGAIN||rc==-EBUSY))return 0;
 return rc;
}
#endif
static int maintain_time(void *arg,bool safe,const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 int entropy_rc=maintain_entropy(p,safe,cancel);if(entropy_rc)return entropy_rc;
#endif
 if(!p->c.trusted_time_status)return 0;
 int state=p->c.trusted_time_status(p->c.trusted_time_arg);
 if(state<=0)return state; /* Common fast path: no DNS, lock on runtime or scope scan. */
 if(state>2)return -EPROTO;
 if(!safe||p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy))return state==2?1:0;
 bool expected=false;
 if(!atomic_compare_exchange_strong(&p->runtime.busy,&expected,true))return state==2?1:0;
 struct time_idle x={p,cancel,0,vv_native_mode_epoch(&p->mode)};
 int rc=scoped_current_owner(&x.owner);
 if(!rc)rc=time_idle_health(&x);
 if(!rc){struct vv_https_cancel token={time_idle_cancelled,&x};
  rc=p->c.trusted_time_refresh(p->c.trusted_time_arg,&token);
  int after=time_idle_health(&x);
  if(after)rc=after;
 }
 atomic_store(&p->runtime.busy,false);
 if(auth_cancelled(cancel))return -ECANCELED;
 if(rc==-EAGAIN||rc==-EBUSY)return state==2?1:0;
 return rc;
}
#endif
static int dialogue_stream_scoped(void *arg,const char *text,char *answer,size_t capacity,
 const struct vv_https_cancel *cancel,const struct vv_reply_scope_token *token){
 struct vv_native_platform *p=arg;
 if(answer&&capacity)vv_https_secure_zero(answer,capacity);
 if(!ready(p))return -EACCES;
 return p->c.speech->chat_speak_scoped(p->c.speech->arg,text,answer,capacity,cancel,token);
}
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
static int near_reset_worker(void *arg){
 struct vv_native_platform*p=arg;
 return k7_echo_runtime_rearm_owner(p->scope_owner);
}
#endif
#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION && defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT && defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
int vv_native_platform_prompt_admit(struct vv_native_runtime *runtime,const struct vv_https_cancel *cancel){
 if(!runtime||runtime->config.ops.ready!=ready)return 0;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return -EINVAL;
 if(auth_cancelled(cancel))return -ECANCELED;
 if(!p->c.scoped_dialogue||!p->near_round_finished)return 0;
 unsigned epoch=vv_native_mode_epoch(&p->mode);uint64_t owner=0;
 int rc=scoped_listen_admit(p);if(rc)return rc;
 rc=scoped_current_owner(&owner);if(rc)return rc;
 if(!p->near_experiment||owner!=p->scope_owner)return VV_SCOPE_OWNER_UNSAFE;
 /* PF must already have released its route/source borrowers. The existing
  * rearm performs the real old-cut ticket handoff without resetting DSP. */
 bool window=p->c.confirmed_barge||p->c.onset_barge;
 bool saved_allowed=false;unsigned saved_epoch=0;
 if(window){
  int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
  saved_allowed=p->event_reply_allowed;saved_epoch=p->event_mode_epoch;
  p->event_reply_allowed=true;p->event_mode_epoch=epoch;
  pthread_mutex_unlock(&p->event_scope_lock);
 }
 rc=vv_near_experiment_rearm(p->near_experiment,near_reset_worker,p);
 if(!rc)p->near_round_finished=false;
 if(window){
  int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
  p->event_reply_allowed=saved_allowed;p->event_mode_epoch=saved_epoch;
  pthread_mutex_unlock(&p->event_scope_lock);
 }
 if(auth_cancelled(cancel)||epoch!=vv_native_mode_epoch(&p->mode))return -ECANCELED;
 if(rc)return rc;
 uint64_t after=0;rc=scoped_current_owner(&after);if(rc)return rc;
 return after==owner?0:VV_SCOPE_OWNER_UNSAFE;
}
#endif
static int scoped_reply(void *arg,const struct vv_asr_batch_result *result,
 const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(!result||!result->command_finalized||!result->capture_identity.valid)return -EPROTO;
 const struct vv_asr_capture_identity *id=&result->capture_identity;
 if(!id->owner_epoch||!id->asr_epoch||!id->received_samples||id->received_samples%320||
    id->next_sample<=id->first_sample||id->next_sample-id->first_sample!=id->received_samples)
  return -EPROTO;
 int rc=scoped_listen_admit(p);if(rc)return rc;
 uint64_t owner=0;rc=scoped_current_owner(&owner);if(rc)return rc;
 if(owner!=id->owner_epoch)return VV_SCOPE_STALE;
 if((p->c.confirmed_barge||p->c.onset_barge)){int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;}
 if(!p->reply_scope){
  rc=vv_reply_scope_create(owner,&p->reply_scope);if(!rc)p->scope_owner=owner;
 }
 if((p->c.confirmed_barge||p->c.onset_barge))pthread_mutex_unlock(&p->event_scope_lock);
 if(rc)return rc;
 if((p->c.confirmed_barge||p->c.onset_barge)){
  /* Only the serial runtime owner reads PHOTO occupancy. The event thread
   * sees this pinned natural-reply window, never races mutable photo state. */
  int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
  if(p->runtime.pending||p->mode.assessment_active)rc=-EBUSY;
  else {p->event_reply_allowed=true;p->event_mode_epoch=vv_native_mode_epoch(&p->mode);}
  pthread_mutex_unlock(&p->event_scope_lock);if(rc)return rc;
 }
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 if(p->near_experiment&&p->near_round_finished){
  rc=vv_near_experiment_rearm(p->near_experiment,near_reset_worker,p);if(rc)return rc;
  p->near_round_finished=false;
 }
#endif
 if(p->barge){rc=vv_barge_route_begin(p->barge,p->reply_scope,&p->dialogue);if(rc)return rc;}
 rc=vv_dialogue_reply_scoped(&p->dialogue,result->text,cancel,p->reply_scope,
   id->owner_epoch,id->asr_epoch,dialogue_stream_scoped,p);
 if(p->barge)vv_barge_route_end(p->barge);
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 if(p->near_experiment)p->near_round_finished=true;
#endif
 if(p->c.confirmed_barge&&!p->c.onset_barge){
  pthread_mutex_lock(&p->event_scope_lock);p->event_reply_allowed=false;
  pthread_mutex_unlock(&p->event_scope_lock);
 }
 return rc;
}
#endif
static uint64_t dialogue_now(void*a){struct vv_native_platform*p=a;return p->c.tls.now_ms(p->c.tls.now_arg);}
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
static int nominal_startup_register(struct vv_native_platform *p){
 if(!p->c.onset_barge)return 0;
 int rc=k7_raw_vad_owner_product_policy_register(p);
 if(!rc)p->nominal_policy_registered=true;
 return rc;
}
static void nominal_startup_unregister(struct vv_native_platform *p){
 if(p->nominal_policy_registered){k7_raw_vad_owner_product_policy_unregister(p);p->nominal_policy_registered=false;}
}
static int nominal_startup_awake(struct vv_native_platform *p){
 /* This token was registered only after the exact explicit product-arm claim.
  * Wake conversation state without a greeting command, TX or gimbal action. */
 if(!p||!p->nominal_policy_registered||!p->c.always_wake||!p->c.scoped_dialogue)return -EACCES;
#if defined(K7_ADDRESSED_WINDOW) && K7_ADDRESSED_WINDOW
 /* init() already starts asleep; no greeting, motion or cloud ASR here. */
 printf("DIALOGUE_GATE waiting_wake=1 reason=startup idle_ms=%u\n",VV_ADDRESSED_IDLE_MS);
 return 0;
#else
 return vv_dialogue_wake(&p->dialogue);
#endif
}
#endif
struct vv_native_platform *vv_native_platform_create(const struct vv_native_platform_config*c){
 if(!c||!c->missing||!c->claim||!c->release||!c->make_request||!c->release_request||!c->sleep_ms||!c->utc_now||!c->tls.security_ready||!c->tls.now_ms||!c->tls.entropy_read||!c->tls.ca_pem||!c->tls.ca_pem_length||!c->tls.tcp_connect||c->samples<320||c->samples>VV_ASR_LISTEN_SAMPLES_MAX||c->samples%320||!c->pcm_capacity||c->pcm_capacity>960000||c->pcm_capacity%2){printf("NATIVE_PLATFORM create_stage=1 result=%d\n",-EINVAL);return NULL;}
 if(c->always_wake){
  if(!c->speech||!c->speech->listen)return NULL;
#if !defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) || !defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) || !defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
  return NULL;
#endif
 }
 if(c->confirmed_barge&&(!c->scoped_dialogue||!c->consume_confirmed_wake||!c->confirmed_wake_arg))return NULL;
 if(c->onset_barge){
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
  const struct vv_barge_driver *d=c->onset_driver;
  if(!c->scoped_dialogue||c->confirmed_barge)return NULL;
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
  if(d)return NULL; /* this explicit experiment owns its own driver */
  struct k7_raw_vad_owner_status model;
  int model_rc=k7_raw_vad_owner_status(&model);
  if(model_rc||model.phase!=K7_RAW_OWNER_READY||!model.model_generation||model.voice_owner||model.borrow_owner){
   printf("NEAR_EXPERIMENT model_ready=0 rc=%d prepare_required=1\n",model_rc);return NULL;
  }
#else
  if(!d||!d->authority.enter||!d->authority.leave||!d->tail.frame||!d->tail.now_ms||!d->onset_support||!d->attach||!d->detach)return NULL;
#endif
#else
  return NULL;
#endif
 }
 if(c->scoped_dialogue){
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
  if(!c->speech||!c->speech->listen||!c->speech->chat_speak_scoped)return NULL;
#else
  return NULL;
#endif
 }
 int stage=2,error=-ENOMEM;
 struct vv_native_platform*p=calloc(1,sizeof(*p));if(!p){printf("NATIVE_PLATFORM create_stage=2 result=%d\n",-ENOMEM);return NULL;}p->c=*c;atomic_init(&p->authenticated,false);atomic_init(&p->expires,0);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 error=nominal_startup_register(p);if(error)goto fail;
#endif

#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 atomic_init(&p->event_enabled,false);
 if(c->confirmed_barge||c->onset_barge){
  error=-pthread_mutex_init(&p->event_scope_lock,NULL);if(error)goto fail;
  p->event_scope_lock_ready=true;
 }
#endif
 struct vv_native_client_config nc={.tls=c->tls,.limits=c->limits,.host=host,.base_path=base,.asr_path=asr,.tts_path=tts,.port=443,.tts_device_bearer=false,.utc_now=c->utc_now,.clock_arg=c->clock_arg,.speech=c->speech,.development_lan_http=c->development_lan_http};
#if K7_REAL_SHUIGUANG_PROFILE
 if(!c->speech||!c->speech->asr||!c->speech->tts||!vv_real_business_target(nc.host,nc.port,nc.base_path,nc.development_lan_http)){error=-EINVAL;goto fail;}
#else
 if(c->development_lan_http){nc.host="10.3.3.170";nc.port=18085;nc.base_path="";}
#endif
 stage=3;error=-EIO;p->client=vv_native_client_create(&nc);if(!p->client)goto fail;
 stage=4;error=-ENOMEM;p->pcm=malloc(c->pcm_capacity);if(!p->pcm)goto fail;
 stage=5;error=vv_native_mode_k7_init(&p->mode,mode_cancel,mode_quiet,p);if(error)goto fail;
 vv_native_assessment_init(&p->assessment);p->backend=(struct vv_assessment_backend_adapter){vv_native_client_backend(p->client),{nc.host,nc.port,tts},false};
 p->assessment.ops=&vv_assessment_backend_ops;p->assessment.ops_arg=&p->backend;p->assessment.now_ms=c->tls.now_ms;p->assessment.clock_arg=c->tls.now_arg;p->assessment.wait_ms=wait_ms;p->assessment.wait_arg=p;
 vv_report_audio_init(&p->report);p->speaker=(struct vv_report_audio_k7){p->client,assessment_ready,p};p->report.ops=&vv_report_audio_k7_ops;p->report.arg=&p->speaker;
 struct vv_native_runtime_config rc={.client=p->client,.mode=&p->mode,.assessment=&p->assessment,.report=&p->report,.pcm=p->pcm,.pcm_capacity=c->pcm_capacity,.assessment_ready=assessment_ready};vv_native_runtime_k7_ops(&rc.ops,ready,p);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 struct vv_dialogue_ops dc={.ask=dialogue_ask,.speak=dialogue_speak,.now_ms=dialogue_now,.arg=p,
  .resident_idle=c->always_wake};
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(c->speech&&c->speech->chat_speak)dc.ask_speak=dialogue_stream;
 if(dc.ask_speak&&c->speech->speak_literal)dc.local_reply=dialogue_local_reply;
#endif
 stage=6;error=vv_dialogue_init(&p->dialogue,&dc,120000);if(error)goto fail;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 error=nominal_startup_awake(p);if(error)goto fail;
#endif
 rc.dialogue=&p->dialogue;
#if defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 if(c->scoped_dialogue){rc.scoped_reply=scoped_reply;rc.scoped_listen_admit=scoped_listen_admit;rc.scoped_arg=p;}
 if(c->always_wake){rc.command_admit=idle_command_admit;rc.command_admit_arg=p;}
 if(c->onset_barge){
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  /* Explicit experiment build selects policy, not packet/action authority.
   * Worker must separately claim the one-shot product arm before emitting. */
  struct vv_near_experiment_policy policy;vv_near_experiment_apm_nominal_policy(&policy);
#else
  static const struct vv_near_experiment_policy policy={.enabled=true,.revision=2026102201,
   .capture_sha256={0x32,0x47,0x82,0xac,0xe5,0xe8,0x20,0x87,0xcc,0x8f,0x60,0xf2,0xab,0x95,0xfe,0x08,0xb7,0x80,0x5c,0xc7,0x49,0x95,0xd4,0x59,0xc4,0x49,0xe9,0x5d,0x59,0xdd,0x88,0xac},
   .lag_samples=39,.probability=.9f,.consecutive_windows=3,.warmup_samples=16000,.maximum_samples=960000};
#endif
  error=vv_near_experiment_create(&policy,barge_observe,p,&p->near_experiment);if(error)goto fail;
  vv_near_experiment_export(p->near_experiment,&p->near_driver);p->c.onset_driver=&p->near_driver;
  char *enable[]={"k7sound","echo-diag-enable"};error=k7_echo_runtime_command(2,enable);if(error)goto fail;
#endif
  struct vv_barge_route_config bc={.authority=p->c.onset_driver->authority,.tail=p->c.onset_driver->tail,
    .onset_support=p->c.onset_driver->onset_support,
    .observe=barge_observe,.arg=p};
  error=vv_barge_route_create(&bc,&p->barge);if(error)goto fail;
 }
#endif
#endif
 stage=6;error=vv_native_runtime_init(&p->runtime,&rc);if(error)goto fail;
 return p;
 fail:printf("NATIVE_PLATFORM create_stage=%d result=%d pcm_capacity=%lu\n",stage,error,(unsigned long)c->pcm_capacity);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
#if defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT)
 if(p->barge)(void)vv_barge_route_destroy(p->barge);
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 (void)vv_near_experiment_destroy(p->near_experiment);
#endif
#endif
 if(p->event_scope_lock_ready)pthread_mutex_destroy(&p->event_scope_lock);
#endif
 #if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 nominal_startup_unregister(p);
#endif
 if(p->pcm){vv_https_secure_zero(p->pcm,c->pcm_capacity);free(p->pcm);}vv_native_client_destroy(p->client);vv_https_secure_zero(p,sizeof(*p));free(p);return NULL;
}
int vv_native_platform_login(struct vv_native_platform*p,const char*credential,const char*version,const char*proof,const struct vv_https_cancel*cancel){
 if(!p)return -EINVAL;
 if(p->bound)return -EBUSY;
 unsigned bits=vv_native_platform_missing(p)&~(VV_NATIVE_AUTH|VV_NATIVE_BUSINESS_AUTH|VV_NATIVE_PREVIEW|VV_NATIVE_OWNER|VV_NATIVE_METADATA);
 if(bits)return -EAGAIN;
 atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);
 struct vv_backend_client*b=vv_native_client_backend(p->client);int rc=login_business(p,credential,version,proof,cancel);
 if(!rc){atomic_store(&p->expires,b->expires_at);atomic_store(&p->authenticated,true);}return rc;
}
int vv_native_platform_bind(struct vv_native_platform*p){if(!p)return -EINVAL;if(p->bound)return -EALREADY;
 if((p->c.entropy_status==NULL)!=(p->c.entropy_refresh==NULL))return -EINVAL;
#if !defined(CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE) || !defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) || !defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) || !defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 if(p->c.entropy_status||p->c.entropy_refresh)return -ENOTSUP;
#endif
 struct vv_native_service_config c={.runtime=&p->runtime,.missing=missing,.claim=claim,.release=release,
  .make_request=make,.release_request=release_request,.sleep_ms=sleep_ms,.arg=p,.samples=p->c.samples};
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 c.maintenance=maintain_business;
#endif
 int rc=0;
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if((p->c.trusted_time_status==NULL)!=(p->c.trusted_time_refresh==NULL))return -EINVAL;
 if(p->c.trusted_time_status){
  if(!p->c.scoped_dialogue)return -ENOTSUP;
  c.time_maintenance=maintain_time;
 }
#else
 if(p->c.trusted_time_status||p->c.trusted_time_refresh)return -ENOTSUP;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(p->c.entropy_status){
  if(!p->c.scoped_dialogue)return -ENOTSUP;
  c.time_maintenance=maintain_time;
 }
#endif
 if(p->c.always_wake){
  p->idle_events=(struct vv_voice_idle_ops){idle_epoch,p};
  rc=vv_voice_session_idle_register(&p->idle_events);if(rc)return rc;
  p->idle_registered=true;c.listen_admit=idle_admission;c.listen_release=idle_release;
 }
 if(p->barge){
  rc=p->c.onset_driver->attach(p->c.onset_driver->arg,p->barge);
  if(rc){if(p->idle_registered){int e=vv_voice_session_idle_unregister(&p->idle_events);if(e)return e;p->idle_registered=false;}return rc;}
  p->barge_attached=true;c.listen_admit=idle_admission;c.listen_release=idle_release;
 }
 if(p->c.confirmed_barge){
  p->events=(struct vv_voice_event_ops){event_target,event_submit,p};
  rc=vv_voice_session_event_register(&p->events);
  if(rc){if(p->idle_registered){int e=vv_voice_session_idle_unregister(&p->idle_events);if(e)return e;p->idle_registered=false;}return rc;}
  p->event_registered=true;
 }
#endif
 rc=vv_native_service_register(&c);
 if(!rc){p->bound=true;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  atomic_store(&p->event_enabled,p->c.confirmed_barge);
#endif
 }
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 else if(p->event_registered){
  int e=vv_voice_session_event_unregister(&p->events);
  if(e)return e;
  p->event_registered=false;
 }
#endif
 #if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 #ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(rc&&p->barge_attached){
  vv_barge_route_close(p->barge);
  int e=p->c.onset_driver->detach(p->c.onset_driver->arg,p->barge);if(e)return e;
  p->barge_attached=false;
 }
 #endif
 if(rc&&p->idle_registered){int e=vv_voice_session_idle_unregister(&p->idle_events);if(e)return e;p->idle_registered=false;}
#endif
 return rc;}
int vv_native_platform_destroy(struct vv_native_platform*p){
 if(!p)return 0;
 if(p->bound){int rc=vv_native_service_unregister();if(rc)return rc;p->bound=false;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 /* Service unregister retires the serial owner. Disable and drain the
  * synchronous observer pin before touching its borrowed scope/platform. */
 atomic_store(&p->event_enabled,false);
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 #ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(p->barge){
  vv_barge_route_close(p->barge);
  if(p->barge_attached){int rc=p->c.onset_driver->detach(p->c.onset_driver->arg,p->barge);if(rc)return rc;p->barge_attached=false;}
  int rc=vv_barge_route_destroy(p->barge);if(rc)return rc;p->barge=NULL;
 }
 #endif
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 if(p->near_experiment){int rc=vv_near_experiment_destroy(p->near_experiment);if(rc)return rc;p->near_experiment=NULL;}
#endif
 if(p->idle_registered){int rc=vv_voice_session_idle_unregister(&p->idle_events);if(rc)return rc;p->idle_registered=false;}
 if(p->event_registered){
  int rc=vv_voice_session_event_unregister(&p->events);if(rc)return rc;
  p->event_registered=false;
 }
#endif
 /* Unknown reply retirement retains every borrowed resource. */
 if(atomic_load(&p->runtime.busy)||atomic_load(&p->dialogue.busy))return -EBUSY;
 if(p->reply_scope){int rc=vv_reply_scope_destroy(p->reply_scope);if(rc)return rc;p->reply_scope=NULL;}
 if(p->event_scope_lock_ready){int e=pthread_mutex_destroy(&p->event_scope_lock);if(e)return -e;p->event_scope_lock_ready=false;}
#endif
 #if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 nominal_startup_unregister(p);
#endif
 vv_native_client_destroy(p->client);vv_https_secure_zero(p->pcm,p->c.pcm_capacity);free(p->pcm);
 vv_https_secure_zero(p,sizeof(*p));free(p);return 0;
}

#else
#include "vv_native_platform.h"
#include "vv_voice_settings.h"
#include "vv_aliyun_provider.h"
#include "vv_report_audio_k7.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
#include "vv_k7_platform_factory.h"
#include "vv_local_time.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_voice_session.h"
#include "voice_session.h"
#include "vv_paraformer_k7.h"
#include "vv_barge_route.h"
#endif
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
/* Confirmed document endpoint set; backend appends /api/v1 itself. */
#if K7_REAL_SHUIGUANG_PROFILE
static const char host[]=VV_REAL_BUSINESS_HOST,base[]=VV_REAL_BUSINESS_BASE;
#else
static const char host[]="eveaisia.com",base[]="/openvela";
#endif
static const char asr[]="/openvela/asr",tts[]="/openvela/tts";
struct vv_native_platform {
 struct vv_native_platform_config c;struct vv_native_client *client;
 struct vv_native_mode mode;struct vv_native_runtime runtime;
 struct vv_native_assessment assessment;struct vv_assessment_backend_adapter backend;
 struct vv_report_audio report;struct vv_report_audio_k7 speaker;
 unsigned char *pcm;bool bound;atomic_bool authenticated;atomic_uint_fast64_t expires;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 struct vv_dialogue dialogue;
 struct vv_reply_scope *reply_scope;uint64_t scope_owner;
 pthread_mutex_t event_scope_lock;bool event_scope_lock_ready,event_registered;
 bool event_reply_allowed;unsigned event_mode_epoch;
 atomic_bool event_enabled;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 struct vv_voice_event_ops events;
 struct vv_voice_idle_ops idle_events;bool idle_registered;
 struct vv_barge_route *barge;bool barge_attached;
#endif
 /* Confirmed-event callbacks are synchronous under the manager lifetime pin.
  * Unregister drains them before scope/platform memory is released. */
 vv_k7_secure_load_fn business_loader;void *business_loader_arg;
 uint64_t renew_after_ms,renew_last_ms;unsigned renew_failures;
 bool renew_clock_seen,renew_clock_failed;
#endif
};
static bool ready(void *);
/* Identity-check the original ops before dereferencing its opaque argument.
 * No public structure layout, registry or second audio owner is introduced. */
int vv_native_platform_local_setting(struct vv_native_runtime *runtime,const char *text,
 char *ack,size_t capacity,const struct vv_https_cancel *cancel){
 if(!runtime||runtime->config.ops.ready!=ready)return 0;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return -EINVAL;
 struct vv_voice_setting cmd;int rc=vv_voice_setting_parse(text,&cmd);
 /* A recognized bad value is user input rejection, not an owner failure.
  * Do not commit, and preserve cancellation and all other operational errors. */
 if(rc==-ERANGE){
  if(!ack||capacity<128)return -EINVAL;
  ack[0]=0;
  if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
  snprintf(ack,capacity,"音量范围是百分之一到百分之一百，请重新设置。");
  return 2;
 }
 if(rc<=0)return rc;
 if(!ack||capacity<128)return -EINVAL;
 ack[0]=0;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 const struct vv_speech_provider *speech=p->c.speech;
 if(!speech)return -ENOTSUP;
 if(cmd.kind==VV_SETTING_VOICE){
  bool applied=cmd.value!=0;
  rc=cmd.value<0?vv_aliyun_provider_toggle_voice(speech,cancel,&applied):
   vv_aliyun_provider_set_voice(speech,applied,cancel);
  if(!rc)snprintf(ack,capacity,"%s",applied?"已切换流萤音色。":"已恢复默认音色。");
 }else{
  unsigned applied=0;
  rc=vv_aliyun_provider_set_volume(speech,cmd.value,cmd.kind==VV_SETTING_VOLUME_RELATIVE,cancel,&applied);
  if(!rc){
   const char *message="音量已调整。";
   if(cmd.kind==VV_SETTING_VOLUME_RELATIVE){
    if(cmd.value>0)message=applied>=100?"音量已到最大。":"音量已调大。";
    else message=applied<=1?"音量已到最小。":"音量已调小。";
   }
   snprintf(ack,capacity,"%s",message);
  }
 }
 return rc?rc:1;
}

unsigned vv_native_platform_missing(struct vv_native_platform *p){
 if(!p)return VV_NATIVE_CONFIG;
 unsigned bits=p->c.missing(p->c.arg)&~(VV_NATIVE_AUTH|VV_NATIVE_ENDPOINT|VV_NATIVE_BUSINESS_AUTH);uint64_t utc=0;
 if(!p->c.utc_now(p->c.clock_arg,&utc)||!utc)bits|=VV_NATIVE_TIME;
 if(!p->c.tls.security_ready(p->c.tls.security_arg))bits|=VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA;
 uint64_t expires=atomic_load(&p->expires);
 if(!atomic_load(&p->authenticated)||!utc||expires<=utc||expires-utc<=60)bits|=p->c.speech?VV_NATIVE_BUSINESS_AUTH:VV_NATIVE_AUTH;
 return bits;
}
static unsigned missing(void *a){return vv_native_platform_missing(a);}
static bool ready(void *a){return !(missing(a)&~VV_NATIVE_BUSINESS_AUTH);}
static bool assessment_ready(void *a){return !missing(a);}
#if K7_REAL_SHUIGUANG_PROFILE
bool vv_native_platform_local_failure_ready(struct vv_native_runtime *runtime){
 if(!runtime||runtime->config.ops.ready!=ready)return false;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return false;
 return !(missing(p)&~(VV_NATIVE_NETWORK|VV_NATIVE_BUSINESS_AUTH));
}
#endif
/* Business login is synchronous under the platform owner. With a separate
 * speech provider, cap only this optional request so a missing backend cannot
 * consume a full speech-listening interval. The stack copy and pointer are
 * borrowed for this call only; ASR/TTS and photo upload limits stay unchanged.
 * Budget expiry remains HTTPS timeout, never an outer cancellation. */
static int login_business(struct vv_native_platform *p,
 const char *credential,const char *version,const char *proof,
 const struct vv_https_cancel *cancel){
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 if(!p->c.speech||!b->https)
  return vv_backend_client_login(b,credential,version,proof,cancel);
 struct vv_https_client limited=*b->https;
#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
 /* Domain DNS requires a 2000ms worst-case allowance. Keep the optional
  * business call bounded; speech transport limits are separate and unchanged. */
 const unsigned budget_ms=p->c.development_lan_http?500u:3000u;
 if(limited.limits.total_timeout_ms>budget_ms)limited.limits.total_timeout_ms=budget_ms;
#else
 if(limited.limits.total_timeout_ms>500u)limited.limits.total_timeout_ms=500u;
#endif
 struct vv_https_client *original=b->https;b->https=&limited;
 int rc=vv_backend_client_login(b,credential,version,proof,cancel);
 b->https=original;return rc;
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
/* Owner-only inspection. Other threads read the atomic readiness mirror and
 * must never touch the mutable backend token while an HTTP operation runs. */
static void sync_business_auth(struct vv_native_platform *p){
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 if(!b->token[0]||!b->expires_at){atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);}
}
static bool auth_cancelled(const struct vv_https_cancel *c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static void invalidate_business(struct vv_native_platform *p){
 vv_backend_client_clear_session(vv_native_client_backend(p->client));
 atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);
}
#if K7_REAL_SHUIGUANG_PROFILE && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
/* Invoked only by the serialized report owner, already holding runtime.busy.
 * The mandatory caller cancel callback retains deadline/epoch/PHOTO/HOLD gates.
 * Pure network loss is retryable on this read-only path; trust never is. */
static int wait_task_gate(struct vv_native_platform *p,
 const struct vv_https_cancel *cancel){
 if(auth_cancelled(cancel)||atomic_load(&p->runtime.cancelled))return -ECANCELED;
 uint64_t now=p->c.tls.now_ms(p->c.tls.now_arg);
 if(p->renew_clock_failed||now==UINT64_MAX||
    (p->renew_clock_seen&&now<p->renew_last_ms)){
  p->renew_clock_failed=true;invalidate_business(p);return VV_BACKEND_CLIENT_TIME;
 }
 p->renew_clock_seen=true;p->renew_last_ms=now;
 unsigned bits=missing(p)&~VV_NATIVE_BUSINESS_AUTH;
 if(bits&(VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA))return VV_BACKEND_CLIENT_TIME;
 if(bits==VV_NATIVE_NETWORK)return VV_HTTPS_EIO;
 return bits?-EACCES:0;
}
static bool wait_gate_soft_result(int rc){
 return !rc||rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED||
  rc==VV_HTTPS_EIO||rc==VV_HTTPS_ETIMEOUT||rc==VV_BACKEND_CLIENT_AUTH;
}
static int wait_gate_result(int rc,int gate){
 if(!gate)return rc;
 if(gate==VV_HTTPS_EIO)return rc?rc:gate;
 /* A STOP cancellation must not erase a concrete error already returned by
  * HTTP/login/loader. Only enumerated recoverable results yield to cancel. */
 if((gate==-ECANCELED||gate==VV_HTTPS_ECANCELLED)&&!wait_gate_soft_result(rc))return rc;
 return gate;
}
static int wait_task_once(struct vv_native_platform *p,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *cancel){
 int rc=wait_task_gate(p,cancel);if(rc)return rc;
 rc=vv_backend_client_task_wait(vv_native_client_backend(p->client),id,20,task,brief,cancel);
 sync_business_auth(p);
 if(rc==VV_BACKEND_CLIENT_AUTH)invalidate_business(p);
 int gate=wait_task_gate(p,cancel);
 /* Preserve concrete failures over incidental network loss and cancellation;
  * clock or owner safety failures still take precedence. */
 return wait_gate_result(rc,gate);
}
static int wait_task_refresh(struct vv_native_platform *p,
 const struct vv_https_cancel *cancel){
 struct vv_k7_secure_login secrets={{0},{0},{0}};
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 int rc=wait_task_gate(p,cancel);uint64_t utc=0;
 if(rc)goto done;
 if(!p->business_loader){rc=VV_BACKEND_CLIENT_AUTH;goto done;}
 invalidate_business(p);
 rc=p->business_loader(p->business_loader_arg,&secrets);
 if(!rc&&(!memchr(secrets.credential,0,sizeof(secrets.credential))||
    !memchr(secrets.version,0,sizeof(secrets.version))||
    !memchr(secrets.proof,0,sizeof(secrets.proof))||!secrets.credential[0]))rc=-EINVAL;
 int gate=wait_task_gate(p,cancel);
 rc=wait_gate_result(rc,gate);
 if(!rc)rc=login_business(p,secrets.credential,secrets.version,secrets.proof,cancel);
 /* Never publish auth or begin the retry while credentials remain in memory. */
 vv_https_secure_zero(&secrets,sizeof(secrets));
 gate=wait_task_gate(p,cancel);
 rc=wait_gate_result(rc,gate);
 if(!rc&&(!b->token[0]||!p->c.utc_now(p->c.clock_arg,&utc)||!utc||
    b->expires_at<=utc||b->expires_at-utc<=60))rc=VV_BACKEND_CLIENT_AUTH;
 if(!rc){
  atomic_store(&p->expires,b->expires_at);atomic_store(&p->authenticated,true);
  p->renew_failures=0;
  p->renew_after_ms=p->renew_last_ms>UINT64_MAX-30000?UINT64_MAX:p->renew_last_ms+30000;
 }
done:
 vv_https_secure_zero(&secrets,sizeof(secrets));
 if(rc){
  invalidate_business(p);
  unsigned delay=1000u<<(p->renew_failures<6?p->renew_failures:6);
  if(p->renew_failures<6)p->renew_failures++;
  p->renew_after_ms=p->renew_last_ms>UINT64_MAX-delay?UINT64_MAX:p->renew_last_ms+delay;
 }
 return rc;
}
static int wait_task_network(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *cancel){
 if(task)memset(task,0,sizeof(*task));
 if(brief)memset(brief,0,sizeof(*brief));
 struct vv_native_runtime *runtime=runtime_arg;
 if(!runtime||runtime->config.ops.ready!=ready||!id||!task||!brief||
    !cancel||!cancel->is_cancelled)return -EINVAL;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return -EINVAL;
 if(!atomic_load(&runtime->busy))return -EBUSY;
 int rc=wait_task_once(p,id,task,brief,cancel);
 if(rc==VV_BACKEND_CLIENT_AUTH){
  rc=wait_task_refresh(p,cancel);
  if(!rc)rc=wait_task_once(p,id,task,brief,cancel);
 }
 if(rc){memset(task,0,sizeof(*task));memset(brief,0,sizeof(*brief));}
 return rc;
}

static int upload_task_network(struct vv_native_platform *p,
 const struct vv_assessment_request *q,char accepted[37],bool *accepted_valid,
 const struct vv_https_cancel *cancel){
 *accepted_valid=false;
 int rc=wait_task_gate(p,cancel);if(rc)return rc;
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 rc=vv_backend_client_upload(b,q->capture_session,q->consent_ref,q->boundary_hex,
  q->idempotency_key,q->images,accepted,cancel);
 if(!rc)*accepted_valid=true; /* Backend validated the complete 202/UUID. */
 sync_business_auth(p);
 if(rc==VV_BACKEND_CLIENT_AUTH)invalidate_business(p);
 return wait_gate_result(rc,wait_task_gate(p,cancel));
}

/* The service thread owns ASR admission and controls; only this worker owns
 * mutable business TLS/token/renewal and the outer assessment cancel guard. */
static int idle_admission(void *,const struct vv_https_cancel *);
static void idle_release(void *);
struct wait_task_job {
 struct vv_native_platform *p;
 char id[37];unsigned epoch;
 bool uploading,upload_accepted;struct vv_assessment_request upload_request;char accepted[37];
 atomic_int upload_error;atomic_bool derived_cancel;
 struct vv_https_cancel outer;
 atomic_bool done,cancel;
 atomic_int main_error;
 int result;
 struct vv_backend_task task;
 struct vv_backend_brief brief;
};
static bool wait_worker_cancel(void *arg){
 struct wait_task_job *job=arg;
 return atomic_load_explicit(&job->cancel,memory_order_acquire)||
  job->outer.is_cancelled(job->outer.arg);
}
static void *wait_task_worker(void *arg){
 struct wait_task_job *job=arg;
 const struct vv_https_cancel cancel={wait_worker_cancel,job};
 if(job->uploading)job->result=upload_task_network(job->p,&job->upload_request,
  job->accepted,&job->upload_accepted,&cancel);
 else job->result=wait_task_network(&job->p->runtime,job->id,&job->task,&job->brief,&cancel);
 if(job->uploading)atomic_store_explicit(&job->upload_error,job->result,memory_order_release);
 /* Last use of all borrowed runtime, guard, TLS and result storage. The
  * release is the retirement proof even if the subsequent join reports error. */
 atomic_store_explicit(&job->done,true,memory_order_release);
 return NULL;
}
static int wait_main_safety(struct wait_task_job *job){
 struct vv_native_platform *p=job->p;
 if(atomic_load(&p->runtime.cancelled)||vv_native_mode_epoch(&p->mode)!=job->epoch)
  return -ECANCELED;
 /* Thread-safe platform evidence and atomic auth mirror only. Never renew_*,
  * backend session, or the mutable outer assessment callback on this thread. */
 unsigned bits=missing(p)&~VV_NATIVE_BUSINESS_AUTH;
 if(bits&(VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA))return VV_BACKEND_CLIENT_TIME;
 if(bits==VV_NATIVE_NETWORK)return VV_HTTPS_EIO;
 return bits?-EACCES:0;
}
static int wait_main_record(struct wait_task_job *job){
 int safety=wait_main_safety(job);
 if(safety){
  int seen=atomic_load_explicit(&job->main_error,memory_order_acquire);
  /* A transient failure observed by the ASR worker remains a failure even if
   * a later snapshot recovers and HTTP finishes. Upgrade network to hard loss. */
  while(!seen||(seen==VV_HTTPS_EIO&&safety!=VV_HTTPS_EIO)){
   if(atomic_compare_exchange_weak_explicit(&job->main_error,&seen,safety,
       memory_order_acq_rel,memory_order_acquire))break;
  }
 }
 return atomic_load_explicit(&job->main_error,memory_order_acquire);
}
static bool wait_yield_code(int rc){return rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED;}
static int wait_voice_stop_safety(struct wait_task_job *job){
 struct vv_native_platform *p=job->p;
 int sticky=atomic_load_explicit(&job->main_error,memory_order_acquire);
 if(sticky&&sticky!=VV_HTTPS_EIO&&!wait_yield_code(sticky))return sticky;
 unsigned bits=missing(p)&~(VV_NATIVE_NETWORK|VV_NATIVE_BUSINESS_AUTH);
 if(bits&(VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA))return VV_BACKEND_CLIENT_TIME;
 if(bits)return -EACCES;
 if(atomic_load(&p->runtime.cancelled)||
    vv_native_mode_epoch(&p->mode)!=job->epoch+1u||p->mode.assessment_active)
  return -ECANCELED;
 int held=p->mode.ops.status(p->mode.ops.arg,0);
 if(held)return held<0?held:-EACCES;
 if(atomic_load(&p->runtime.cancelled)||vv_native_mode_epoch(&p->mode)!=job->epoch+1u||
    p->mode.assessment_active)return -ECANCELED;
 return 0;
}
static bool wait_stop_worker_soft(int rc){
 /* Only original read-retry network failures and cancellation can coexist
  * with an independently proven applied voice STOP. Every other error wins. */
 return !rc||wait_yield_code(rc)||rc==VV_HTTPS_EIO||rc==VV_HTTPS_ETIMEOUT;
}
static bool wait_main_cancel(void *arg){
 struct wait_task_job *job=arg;
 int error=wait_main_record(job);
 if(error)return true;
 if(job->uploading&&atomic_load_explicit(&job->done,memory_order_acquire)&&
    atomic_load_explicit(&job->upload_error,memory_order_acquire)){
  atomic_store_explicit(&job->derived_cancel,true,memory_order_release);return true;
 }
 return (!job->uploading&&atomic_load_explicit(&job->done,memory_order_acquire))||
  atomic_load_explicit(&job->cancel,memory_order_acquire);
}
static int run_business_job(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *outer,
 int (*started)(void *,const struct vv_https_cancel *),void *started_arg,
 const struct vv_assessment_request *upload,char accepted[37]){
 if(accepted)memset(accepted,0,37);
 if(task)memset(task,0,sizeof(*task));
 if(brief)memset(brief,0,sizeof(*brief));
 struct vv_native_runtime *runtime=runtime_arg;
 if(!runtime||runtime->config.ops.ready!=ready||
    (upload?!accepted:(!id||!task||!brief))||
    !outer||!outer->is_cancelled)return -EINVAL;
 struct vv_native_platform *p=runtime->config.ops.arg;
 if(!p||&p->runtime!=runtime)return -EINVAL;
 if(!atomic_load(&runtime->busy))return -EBUSY;
 if(!upload){size_t id_len=0;while(id_len<37&&id[id_len])id_len++;
  if(id_len!=36)return -EINVAL;}
 struct wait_task_job *job=calloc(1,sizeof(*job));if(!job)return -ENOMEM;
 job->p=p;job->epoch=vv_native_mode_epoch(&p->mode);job->outer=*outer;
 job->uploading=upload!=NULL;
 if(upload)job->upload_request=*upload;else memcpy(job->id,id,37);
 atomic_init(&job->done,false);atomic_init(&job->cancel,false);
 atomic_init(&job->main_error,0);atomic_init(&job->upload_error,0);atomic_init(&job->derived_cancel,false);
 int rc=wait_main_record(job);
 if(rc){vv_https_secure_zero(job,sizeof(*job));free(job);return rc;}
 pthread_attr_t attr;int e=pthread_attr_init(&attr);
 if(e){vv_https_secure_zero(job,sizeof(*job));free(job);return -e;}
 e=pthread_attr_setstacksize(&attr,32768);
 if(e){(void)pthread_attr_destroy(&attr);vv_https_secure_zero(job,sizeof(*job));free(job);return -e;}
 pthread_t thread;e=pthread_create(&thread,&attr,wait_task_worker,job);
 int destroyed=pthread_attr_destroy(&attr);
 if(e){vv_https_secure_zero(job,sizeof(*job));free(job);return -e;}
 const struct vv_https_cancel main_cancel={wait_main_cancel,job};
 rc=destroyed?-destroyed:0;bool voice_stop=false,voice_stop_requested=false;
 /* Start the HTTP worker before this optional main-thread local cue. Its
  * independent cancel token can yield to a completed request, never calls the
  * outer assessment guard, and retains sticky safety/STOP failures. */
 if(!rc&&started&&(job->uploading||!atomic_load_explicit(&job->done,memory_order_acquire))){
  rc=wait_main_record(job);
  if(!rc){
   int cue=started(started_arg,&main_cancel);
   int safety=wait_main_record(job);
   if(safety)rc=(safety==VV_HTTPS_EIO&&cue&&!wait_yield_code(cue))?cue:safety;
   else if(cue&&!( !job->uploading&&wait_yield_code(cue)&&atomic_load_explicit(&job->done,memory_order_acquire)))rc=cue;
  }
 }
 if(rc)atomic_store_explicit(&job->cancel,true,memory_order_release);
 while(!rc&&!atomic_load_explicit(&job->done,memory_order_acquire)){
  rc=wait_main_record(job);
  if(rc)break; /* In particular, never start network ASR while offline. */
  /* Upload preserves the original no-ASR POST phase. Only the two main-thread
   * completion cues overlap; never nest a wait worker or touch shared ASR. */
  if(job->uploading){p->c.sleep_ms(p->c.arg,50);continue;}
  int admitted=idle_admission(p,&main_cancel);
  if(admitted>1){rc=-EPROTO;break;}
  if(admitted<0){
   int safety=wait_main_record(job);
   if(safety&&(safety!=VV_HTTPS_EIO||wait_yield_code(admitted)))rc=safety;
   else if(!safety&&wait_yield_code(admitted)&&atomic_load_explicit(&job->done,memory_order_acquire))break;
   else rc=admitted;
   break;
  }
  if(!admitted){p->c.sleep_ms(p->c.arg,50);continue;}
  int control=vv_native_runtime_wait_control(runtime,p->c.samples,&main_cancel);
  idle_release(p);
  if(control==VV_REAL_WAIT_VOICE_STOP){
   voice_stop_requested=true;
   int stopped=wait_voice_stop_safety(job);
   if(stopped)rc=stopped;
   else{voice_stop=true;rc=VV_REAL_WAIT_VOICE_STOP;}
   break;
  }
  int safety=wait_main_record(job);
  if(safety){rc=(safety==VV_HTTPS_EIO&&control&&!wait_yield_code(control))?control:safety;break;}
  if(control){
   /* A finalized STOP changes epoch and was caught above. Only this exact
    * done-triggered cancellation yields; all other errors remain failures. */
   if(wait_yield_code(control)&&atomic_load_explicit(&job->done,memory_order_acquire))break;
   rc=control;break;
  }
  if(!atomic_load_explicit(&job->done,memory_order_acquire))p->c.sleep_ms(p->c.arg,50);
 }
 if(rc)atomic_store_explicit(&job->cancel,true,memory_order_release);
 /* Never return while the worker can still call the borrowed outer guard. */
 while(!atomic_load_explicit(&job->done,memory_order_acquire))p->c.sleep_ms(p->c.arg,50);
 e=pthread_join(thread,NULL);
 if(e){
  /* Preserve heap storage and fail closed: unknown joinability must not free
   * worker state. done proves external borrows retired, not a successful join. */
  printf("NATIVE_WAIT join_rc=%d retired=1 storage_retained=1\n",e);
  return -e;
 }
 if(voice_stop_requested){
  /* Expected one-step STOP epoch is not generic cancellation. No rearm,
   * cancel clearing or motion is permitted here, including on success. */
  int stopped=wait_voice_stop_safety(job);
  if(!wait_stop_worker_soft(job->result))rc=job->result;
  else if(voice_stop)rc=stopped?stopped:VV_REAL_WAIT_VOICE_STOP;
  else if(stopped&&!wait_yield_code(stopped)&&stopped!=VV_HTTPS_EIO)rc=stopped;
 }else{
  /* Do not turn a concrete worker/control hard error into a network retry. */
  if(!rc||((rc==VV_HTTPS_EIO||(wait_yield_code(rc)&&!wait_stop_worker_soft(job->result)))&&job->result&&job->result!=VV_HTTPS_EIO&&
           !wait_yield_code(job->result)))rc=job->result;
  if(job->uploading&&atomic_load_explicit(&job->derived_cancel,memory_order_acquire)&&
     wait_yield_code(rc)&&job->result)rc=job->result;
  int safety=wait_main_record(job);
  if(safety&&(safety!=VV_HTTPS_EIO||!rc||wait_yield_code(rc)))rc=safety;
 }
 if(job->uploading){if(job->upload_accepted)memcpy(accepted,job->accepted,37);}
 else if(!rc){*task=job->task;*brief=job->brief;}
 vv_https_secure_zero(job,sizeof(*job));free(job);
 return rc;
}
int vv_native_platform_wait_task_progress(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *outer,
 int (*started)(void *,const struct vv_https_cancel *),void *started_arg){
 return run_business_job(runtime_arg,id,task,brief,outer,started,started_arg,NULL,NULL);
}
int vv_native_platform_upload_task_progress(void *runtime_arg,
 const struct vv_assessment_request *q,char accepted[37],const struct vv_https_cancel *outer,
 int (*started)(void *,const struct vv_https_cancel *),void *started_arg){
 if(!q){if(accepted)memset(accepted,0,37);return -EINVAL;}
 return run_business_job(runtime_arg,NULL,NULL,NULL,outer,started,started_arg,q,accepted);
}
int vv_native_platform_wait_task(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *outer){
 return vv_native_platform_wait_task_progress(runtime_arg,id,task,brief,outer,NULL,NULL);
}

#endif

static int maintain_business(void *arg,const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(!p->business_loader)return 0;
 bool idle=false;
 if(!atomic_compare_exchange_strong(&p->runtime.busy,&idle,true))return -EBUSY;
 int rc=0;struct vv_k7_secure_login secrets={{0},{0},{0}};
 if(p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy)){
  rc=-EBUSY;goto done;
 }
 sync_business_auth(p);
 if(auth_cancelled(cancel)){rc=-ECANCELED;goto done;}
 uint64_t now=p->c.tls.now_ms(p->c.tls.now_arg),utc=0;
 if(p->renew_clock_failed||now==UINT64_MAX||
    (p->renew_clock_seen&&now<p->renew_last_ms)){
  p->renew_clock_failed=true;invalidate_business(p);rc=VV_BACKEND_CLIENT_TIME;goto done;
 }
 p->renew_clock_seen=true;p->renew_last_ms=now;
 if((missing(p)&~(VV_NATIVE_AUTH|VV_NATIVE_BUSINESS_AUTH))||
    !p->c.utc_now(p->c.clock_arg,&utc)||!utc){rc=-EAGAIN;goto done;}
 struct vv_backend_client *b=vv_native_client_backend(p->client);
 bool valid=b->token[0]&&b->expires_at>utc&&b->expires_at-utc>60;
 if(valid&&b->expires_at-utc>120){p->renew_failures=0;rc=0;goto done;}
 /* A rejected token overrides the success cooldown; failed logins retain
  * their backoff even if another question arrives immediately afterwards. */
 if(now<p->renew_after_ms&&(valid||p->renew_failures)){rc=-EAGAIN;goto done;}
 atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);
 rc=p->business_loader(p->business_loader_arg,&secrets);
 if(!rc&&(!memchr(secrets.credential,0,sizeof(secrets.credential))||
    !memchr(secrets.version,0,sizeof(secrets.version))||
    !memchr(secrets.proof,0,sizeof(secrets.proof))||!secrets.credential[0]))rc=-EINVAL;
 if(!rc&&auth_cancelled(cancel))rc=-ECANCELED;
 if(!rc)rc=login_business(p,secrets.credential,secrets.version,secrets.proof,cancel);
 /* Wipe before publishing readiness or returning to ordinary speech work. */
 vv_https_secure_zero(&secrets,sizeof(secrets));
 uint64_t ended=p->c.tls.now_ms(p->c.tls.now_arg);
 if(ended==UINT64_MAX||ended<now){p->renew_clock_failed=true;rc=VV_BACKEND_CLIENT_TIME;}
 else p->renew_last_ms=ended;
 if(auth_cancelled(cancel))rc=-ECANCELED;
 if(!rc&&(!b->token[0]||!p->c.utc_now(p->c.clock_arg,&utc)||
    b->expires_at<=utc||b->expires_at-utc<=60))rc=VV_BACKEND_CLIENT_AUTH;
 if(rc){
  invalidate_business(p);
  unsigned delay=1000u<<(p->renew_failures<6?p->renew_failures:6);
  if(p->renew_failures<6)p->renew_failures++;
  p->renew_after_ms=ended>UINT64_MAX-delay?UINT64_MAX:ended+delay;
  printf("NATIVE_BUSINESS refresh_rc=%d retry_ms=%u\n",rc,delay);
 }else{
  atomic_store(&p->expires,b->expires_at);atomic_store(&p->authenticated,true);
  p->renew_failures=0;p->renew_after_ms=ended>UINT64_MAX-30000?UINT64_MAX:ended+30000;
  puts("NATIVE_BUSINESS refresh_rc=0");
 }
done:
 vv_https_secure_zero(&secrets,sizeof(secrets));
 atomic_store(&p->runtime.busy,false);return rc;
}
int vv_native_platform_set_business_loader(struct vv_native_platform *p,
 vv_k7_secure_load_fn load,void *arg){
 if(!p||!load)return -EINVAL;
 if(p->bound)return -EBUSY;
 p->business_loader=load;p->business_loader_arg=arg;return 0;
}
#endif
/* Mode dispatch is serial with ASR/report. Epoch invalidation in mode already
 * cancels eligibility; no concurrent network/TTS call exists here to interrupt. */
static void mode_cancel(void *a){(void)a;}
static void mode_quiet(void *a){(void)a;} /* No TTS overlaps command listening. */
static int claim(void*a){struct vv_native_platform*p=a;return p->c.claim(p->c.arg);}
static void release(void*a){struct vv_native_platform*p=a;p->c.release(p->c.arg);}
static int make(void*a,struct vv_assessment_request*q){struct vv_native_platform*p=a;return p->c.make_request(p->c.arg,q);}
static void release_request(void*a,struct vv_assessment_request*q){struct vv_native_platform*p=a;p->c.release_request(p->c.arg,q);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 sync_business_auth(p);
#endif
}
static void sleep_ms(void*a,unsigned ms){struct vv_native_platform*p=a;p->c.sleep_ms(p->c.arg,ms);}
static void wait_ms(void*a,unsigned ms,const struct vv_https_cancel*c){if(!(c&&c->is_cancelled&&c->is_cancelled(c->arg)))sleep_ms(a,ms);}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
static int dialogue_ask(void*a,const char*text,char*answer,size_t capacity,const struct vv_https_cancel*cancel){
 struct vv_native_platform*p=a;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(p->c.speech&&p->c.speech->chat){
  if(answer&&capacity)vv_https_secure_zero(answer,capacity);
  if(!ready(p))return -EACCES;
  return p->c.speech->chat(p->c.speech->arg,text,answer,capacity,cancel);
 }
#endif
 if(!assessment_ready(p))return -EACCES;
 int rc=vv_backend_client_chat(vv_native_client_backend(p->client),text,answer,capacity,cancel);
 sync_business_auth(p);return rc;
}
static int dialogue_speak(void*a,const char*text,const struct vv_https_cancel*cancel){
 struct vv_native_platform*p=a;return vv_native_client_speak(p->client,text,cancel);
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
static int dialogue_local_reply(void*a,const char*question,char*answer,size_t capacity,
 const struct vv_https_cancel*cancel,const struct vv_reply_scope_token*scope){
 struct vv_native_platform*p=a;
 enum vv_local_time_kind kind=vv_local_time_query(question);
 if(kind==VV_LOCAL_TIME_NONE)return 0;
 if(answer&&capacity)vv_https_secure_zero(answer,capacity);
 if(auth_cancelled(cancel))return -ECANCELED;
 if(!ready(p))return -EACCES;
 uint64_t utc=0;
 if(!p->c.utc_now(p->c.clock_arg,&utc)||!utc)return -EACCES;
 char literal[160];
 int rc=vv_local_time_format(kind,utc,literal,sizeof(literal));
 if(!rc&&!ready(p))rc=-EACCES;
 if(!rc&&auth_cancelled(cancel))rc=-ECANCELED;
 if(!rc)rc=p->c.speech->speak_literal(p->c.speech->arg,literal,answer,capacity,cancel,scope);
 vv_https_secure_zero(literal,sizeof(literal));
 if(rc&&answer&&capacity)vv_https_secure_zero(answer,capacity);
 return rc?rc:1;
}
static int dialogue_stream(void*a,const char*text,char*answer,size_t capacity,const struct vv_https_cancel*cancel){
 struct vv_native_platform*p=a;
 if(answer&&capacity)vv_https_secure_zero(answer,capacity);
 if(!ready(p))return -EACCES;
 return p->c.speech->chat_speak(p->c.speech->arg,text,answer,capacity,cancel);
}
#endif
#if defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)

static int event_target_locked(struct vv_native_platform *p,
 const struct vv_voice_event_state *audio,struct vv_confirmed_wake_context *out){
 if(!atomic_load(&p->event_enabled)||!p->event_reply_allowed||!p->reply_scope||
    !audio->tx_active||p->event_mode_epoch!=vv_native_mode_epoch(&p->mode))return -EAGAIN;
 struct vv_reply_scope_status s;struct vv_dialogue_reply_status d;
 if(vv_reply_scope_snapshot(p->reply_scope,&s)||vv_dialogue_reply_snapshot(&p->dialogue,&d))
  return -EIO;
 if(s.phase!=VV_SCOPE_BOUND||d.phase!=VV_DIALOGUE_REPLY_SPEAKING||
    !d.id||d.id!=s.token.reply_id||s.token.owner_epoch!=audio->owner_epoch||
    !s.tx_generation||s.tx_generation!=audio->tx_generation)return -ESTALE;
 *out=(struct vv_confirmed_wake_context){.owner_epoch=audio->owner_epoch,
  .input_epoch=audio->input_epoch,.mode_epoch=vv_native_mode_epoch(&p->mode),
  .scope_incarnation=s.token.incarnation,.lease_serial=s.token.serial,
  .asr_epoch=s.token.asr_epoch,.reply_id=d.id,.tx_generation=s.tx_generation};
 return 0;
}
static int event_target(void *arg,const struct vv_voice_event_state *audio,
 struct vv_confirmed_wake_context *out){
 struct vv_native_platform *p=arg;
 int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
 int rc=event_target_locked(p,audio,out);
 pthread_mutex_unlock(&p->event_scope_lock);return rc;
}
static int event_submit(void *arg,const struct vv_voice_event_state *audio,
 const struct vv_confirmed_wake_event *event){
 struct vv_native_platform *p=arg;struct vv_confirmed_wake_context current;
 int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
 int rc=event_target_locked(p,audio,&current);
 const struct vv_confirmed_wake_context *old=&event->context;
 if(!rc&&(!event->full_wake||event->negated||event->source!=VV_WAKE_CLEAN_FULL_KWS||
    old->owner_epoch!=current.owner_epoch||old->input_epoch!=current.input_epoch||
    old->mode_epoch!=current.mode_epoch||old->scope_incarnation!=current.scope_incarnation||
    old->lease_serial!=current.lease_serial||old->asr_epoch!=current.asr_epoch||
    old->reply_id!=current.reply_id||old->tx_generation!=current.tx_generation||
    !old->model_epoch||!old->clean_epoch||!old->alignment_epoch))rc=-ESTALE;
 if(!rc){
  /* Epoch values alone are not acoustic proof. The independent gate must
   * revalidate its registered clean stream and consume this exact ticket. */
  current.model_epoch=old->model_epoch;current.clean_epoch=old->clean_epoch;
  current.alignment_epoch=old->alignment_epoch;
  int accepted=p->c.consume_confirmed_wake(p->c.confirmed_wake_arg,event,&current,audio->now_ms);
  if(accepted==1){
   /* Still inside the manager pin. A mode/input invalidation may have raced
    * qualification; consuming the old ticket does not override that change. */
   struct k7_voice_session *session=vv_voice_session_audio();
   if(vv_voice_session_owner_epoch(session)!=current.owner_epoch||
      k7_voice_session_input_epoch(session)!=current.input_epoch||
      k7_voice_session_tx_generation(session)!=current.tx_generation||
      vv_native_mode_epoch(&p->mode)!=current.mode_epoch)rc=-ESTALE;
   else rc=vv_dialogue_cancel_reply_id(&p->dialogue,current.reply_id);
  }else rc=accepted;
  if(rc>1)rc=-EPROTO;
 }
 pthread_mutex_unlock(&p->event_scope_lock);return rc;
}
/* Only the new local-idle entry needs this rejection. PHOTO still requires
 * TRACK; do not turn its legacy mode mismatch into global cancellation. */
static int idle_command_admit(void *arg,enum k7_voice_intent intent,unsigned epoch,
 const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(intent!=K7_VOICE_PHOTO)return 0;
 if(auth_cancelled(cancel)||epoch!=vv_native_mode_epoch(&p->mode))return -ECANCELED;
 if(p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy))return 0;
 int idle_status=p->mode.ops.status(p->mode.ops.arg,0);
 if(auth_cancelled(cancel)||epoch!=vv_native_mode_epoch(&p->mode))return -ECANCELED;
 if(!idle_status&&!p->runtime.pending&&!p->mode.assessment_active&&!atomic_load(&p->mode.busy))
  return -EBUSY;
 return 0; /* Every non-idle path retains the existing mode_apply result. */
}
static unsigned idle_epoch(void *arg){
 struct vv_native_platform *p=arg;return vv_native_mode_epoch(&p->mode);
}
static int barge_observe(void *arg,struct vv_barge_observation *out){
 struct vv_native_platform *p=arg;
 struct vv_voice_event_state st;
 if(vv_voice_session_observe(&st))return VV_SCOPE_OWNER_UNSAFE;
 int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
 unsigned epoch=vv_native_mode_epoch(&p->mode);
 bool allowed=p->event_reply_allowed&&p->event_mode_epoch==epoch&&
   !atomic_load(&p->runtime.cancelled);
 pthread_mutex_unlock(&p->event_scope_lock);
 if(!allowed)return VV_SCOPE_STALE;
 *out=(struct vv_barge_observation){.owner_epoch=st.owner_epoch,.microphone_next=st.microphone_samples,
  .now_ms=st.now_ms,.input_epoch=st.input_epoch,.mode_epoch=epoch,.live_tx_generation=st.tx_generation,
  .rx_live=true,.photo_busy=false,.tx_active=st.tx_active};
 return 0;
}
static int idle_admission(void *arg,const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 if(p->barge){int admission=vv_barge_route_admit(p->barge);
  if(admission!=2)return admission;}
 if(!p->c.always_wake)return 1;
 /* Only this serial service owner examines PHOTO state or pipeline mode. */
 bool idle_mode=!p->runtime.pending&&!p->mode.assessment_active&&
  !atomic_load(&p->mode.busy)&&p->mode.ops.status(p->mode.ops.arg,0)==0;
 int rc=vv_voice_session_idle_select(&p->idle_events,idle_mode,vv_native_mode_epoch(&p->mode));
 if(rc)return rc;
 if(!idle_mode)return 1;
 struct vv_pf_wake_source source;
 rc=vv_voice_session_idle_claim(&p->idle_events,&source);
 /* Candidate expired, was overrun, or lost continuity before claim: wait for
  * a new complete candidate, never turn a raw keyword into an action. */
 if(rc==-ETIMEDOUT||rc==VV_WAKE_STALE||rc==-ESTALE||rc==-EPIPE||rc==-ENOSPC)return 0;
 if(rc!=1)return rc;
 rc=vv_paraformer_k7_wake_arm(&source);
 if(rc){uint64_t first=0,next=0;(void)source.finish(source.arg,&source.request,true,&first,&next);return rc;}
 return 1;
}
static void idle_release(void *arg){
 struct vv_native_platform *p=arg;
 if(p->barge)(void)vv_barge_route_release(p->barge);
 (void)vv_paraformer_k7_wake_disarm();
 if(p->c.always_wake)(void)vv_voice_session_idle_select(&p->idle_events,false,vv_native_mode_epoch(&p->mode));
}
static int scoped_current_owner(uint64_t *owner){
 if(vv_voice_session_health())return VV_SCOPE_OWNER_UNSAFE;
 struct k7_voice_session *audio=vv_voice_session_audio();
 uint64_t e=vv_voice_session_owner_epoch(audio);
 if(!e)return VV_SCOPE_OWNER_UNSAFE;
 *owner=e;return 0;
}
static int scoped_listen_admit(void *arg){
 struct vv_native_platform *p=arg;uint64_t owner=0;
 int rc=scoped_current_owner(&owner);if(rc)return rc;
 if(!p->reply_scope)return 0;
 struct vv_reply_scope_status s;
 if(vv_reply_scope_snapshot(p->reply_scope,&s))return VV_SCOPE_OWNER_UNSAFE;
 if(s.phase!=VV_SCOPE_IDLE&&s.phase!=VV_SCOPE_CLOSED)return VV_SCOPE_OWNER_UNSAFE;
 if(s.tx_generation&&(!s.tx_retired||!s.speaker_joined))return VV_SCOPE_OWNER_UNSAFE;
 if(owner!=p->scope_owner){
  /* Only a new healthy physical owner and fully retired prior turn may reset
   * the ASR epoch namespace. Quarantine never reaches this branch. */
  if(vv_reply_scope_set_owner(p->reply_scope,owner))return VV_SCOPE_OWNER_UNSAFE;
  p->scope_owner=owner;
 }else if(s.tx_generation&&!s.handoff_eligible)return VV_SCOPE_OWNER_UNSAFE;
 return 0;
}
#if defined(CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL) || defined(CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE)
struct time_idle {struct vv_native_platform *p;const struct vv_https_cancel *cancel;
 uint64_t owner;unsigned mode_epoch;};
static int time_idle_health(struct time_idle *x){
 struct vv_native_platform *p=x->p;uint64_t owner=0;
 if(auth_cancelled(x->cancel))return -ECANCELED;
 if(vv_native_mode_epoch(&p->mode)!=x->mode_epoch||p->runtime.pending||
    p->mode.assessment_active||atomic_load(&p->mode.busy))return -EAGAIN;
 if(scoped_current_owner(&owner)||owner!=x->owner)return VV_SCOPE_OWNER_UNSAFE;
 struct k7_voice_session *audio=vv_voice_session_audio();struct k7_voice_session_status status={0};
 k7_voice_session_status(audio,&status);
 if(!status.running||!status.ready||status.stop_requested||status.finished||status.result)return VV_SCOPE_OWNER_UNSAFE;
 if(status.tx_state!=K7_VOICE_TX_IDLE||status.asr_epoch)return -EBUSY;
 struct vv_dialogue_reply_status reply={0};
 if(atomic_load(&p->dialogue.busy)||vv_dialogue_reply_snapshot(&p->dialogue,&reply)||
    reply.phase!=VV_DIALOGUE_REPLY_IDLE)return -EBUSY;
 if(p->reply_scope){
  struct vv_reply_scope_status scope={0};
  if(vv_reply_scope_snapshot(p->reply_scope,&scope)||p->scope_owner!=owner)return VV_SCOPE_OWNER_UNSAFE;
  if(scope.phase!=VV_SCOPE_IDLE&&scope.phase!=VV_SCOPE_CLOSED)return VV_SCOPE_OWNER_UNSAFE;
  if(scope.phase==VV_SCOPE_CLOSED&&(!scope.reply_retired||(scope.tx_generation&&!scope.handoff_eligible)))return VV_SCOPE_OWNER_UNSAFE;
  if(scope.tx_generation&&(!scope.tx_retired||!scope.speaker_joined||!scope.rx_live))return VV_SCOPE_OWNER_UNSAFE;
 }
 return 0;
}
static bool time_idle_cancelled(void *arg){return time_idle_health(arg)!=0;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
static int maintain_entropy(struct vv_native_platform*p,bool safe,const struct vv_https_cancel*cancel){
 if(!p->c.entropy_status)return 0;
 if(auth_cancelled(cancel))return -ECANCELED;
 int state=p->c.entropy_status(p->c.entropy_arg);
 if(state==-EBUSY)return 1; /* wait, not ordinary-call permission */
 if(state<0)return state;
 if(state>1)return -EPROTO;
 if(!state)return 0; /* Outside final600000ms: no manager/scope lock or raw. */
 if(!safe||p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy))return 0;
 bool expected=false;
 if(!atomic_compare_exchange_strong(&p->runtime.busy,&expected,true))return 0;
 struct time_idle x={p,cancel,0,vv_native_mode_epoch(&p->mode)};
 bool refreshed=false;
 int rc=scoped_current_owner(&x.owner);
 if(!rc)rc=time_idle_health(&x);
 if(!rc){
  struct vv_https_cancel token={time_idle_cancelled,&x};
  refreshed=true;
  rc=p->c.entropy_refresh(p->c.entropy_arg,&token);
  int after=time_idle_health(&x);
  if(after&&rc!=-ECANCELED)rc=after;
 }
 atomic_store(&p->runtime.busy,false);
 if(auth_cancelled(cancel))return -ECANCELED;
 /* Busy before any refresh is safe-point deferral. Refresh itself must
  * classify outer-entry busy as1; raw -EBUSY is a fatal negative result. */
 if(!refreshed&&(rc==-EAGAIN||rc==-EBUSY))return 0;
 return rc;
}
#endif
static int maintain_time(void *arg,bool safe,const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 int entropy_rc=maintain_entropy(p,safe,cancel);if(entropy_rc)return entropy_rc;
#endif
 if(!p->c.trusted_time_status)return 0;
 int state=p->c.trusted_time_status(p->c.trusted_time_arg);
 if(state<=0)return state; /* Common fast path: no DNS, lock on runtime or scope scan. */
 if(state>2)return -EPROTO;
 if(!safe||p->runtime.pending||p->mode.assessment_active||atomic_load(&p->mode.busy))return state==2?1:0;
 bool expected=false;
 if(!atomic_compare_exchange_strong(&p->runtime.busy,&expected,true))return state==2?1:0;
 struct time_idle x={p,cancel,0,vv_native_mode_epoch(&p->mode)};
 int rc=scoped_current_owner(&x.owner);
 if(!rc)rc=time_idle_health(&x);
 if(!rc){struct vv_https_cancel token={time_idle_cancelled,&x};
  rc=p->c.trusted_time_refresh(p->c.trusted_time_arg,&token);
  int after=time_idle_health(&x);
  if(after)rc=after;
 }
 atomic_store(&p->runtime.busy,false);
 if(auth_cancelled(cancel))return -ECANCELED;
 if(rc==-EAGAIN||rc==-EBUSY)return state==2?1:0;
 return rc;
}
#endif
static int dialogue_stream_scoped(void *arg,const char *text,char *answer,size_t capacity,
 const struct vv_https_cancel *cancel,const struct vv_reply_scope_token *token){
 struct vv_native_platform *p=arg;
 if(answer&&capacity)vv_https_secure_zero(answer,capacity);
 if(!ready(p))return -EACCES;
 return p->c.speech->chat_speak_scoped(p->c.speech->arg,text,answer,capacity,cancel,token);
}
static int scoped_reply(void *arg,const struct vv_asr_batch_result *result,
 const struct vv_https_cancel *cancel){
 struct vv_native_platform *p=arg;
 if(!result||!result->command_finalized||!result->capture_identity.valid)return -EPROTO;
 const struct vv_asr_capture_identity *id=&result->capture_identity;
 if(!id->owner_epoch||!id->asr_epoch||!id->received_samples||id->received_samples%320||
    id->next_sample<=id->first_sample||id->next_sample-id->first_sample!=id->received_samples)
  return -EPROTO;
 int rc=scoped_listen_admit(p);if(rc)return rc;
 uint64_t owner=0;rc=scoped_current_owner(&owner);if(rc)return rc;
 if(owner!=id->owner_epoch)return VV_SCOPE_STALE;
 if((p->c.confirmed_barge||p->c.onset_barge)){int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;}
 if(!p->reply_scope){
  rc=vv_reply_scope_create(owner,&p->reply_scope);if(!rc)p->scope_owner=owner;
 }
 if((p->c.confirmed_barge||p->c.onset_barge))pthread_mutex_unlock(&p->event_scope_lock);
 if(rc)return rc;
 if((p->c.confirmed_barge||p->c.onset_barge)){
  /* Only the serial runtime owner reads PHOTO occupancy. The event thread
   * sees this pinned natural-reply window, never races mutable photo state. */
  int e=pthread_mutex_lock(&p->event_scope_lock);if(e)return -e;
  if(p->runtime.pending||p->mode.assessment_active)rc=-EBUSY;
  else {p->event_reply_allowed=true;p->event_mode_epoch=vv_native_mode_epoch(&p->mode);}
  pthread_mutex_unlock(&p->event_scope_lock);if(rc)return rc;
 }
 if(p->barge){rc=vv_barge_route_begin(p->barge,p->reply_scope,&p->dialogue);if(rc)return rc;}
 rc=vv_dialogue_reply_scoped(&p->dialogue,result->text,cancel,p->reply_scope,
   id->owner_epoch,id->asr_epoch,dialogue_stream_scoped,p);
 if(p->barge)vv_barge_route_end(p->barge);
 if(p->c.confirmed_barge&&!p->c.onset_barge){
  pthread_mutex_lock(&p->event_scope_lock);p->event_reply_allowed=false;
  pthread_mutex_unlock(&p->event_scope_lock);
 }
 return rc;
}
#endif
static uint64_t dialogue_now(void*a){struct vv_native_platform*p=a;return p->c.tls.now_ms(p->c.tls.now_arg);}
#endif
struct vv_native_platform *vv_native_platform_create(const struct vv_native_platform_config*c){
 if(!c||!c->missing||!c->claim||!c->release||!c->make_request||!c->release_request||!c->sleep_ms||!c->utc_now||!c->tls.security_ready||!c->tls.now_ms||!c->tls.entropy_read||!c->tls.ca_pem||!c->tls.ca_pem_length||!c->tls.tcp_connect||c->samples<320||c->samples>VV_ASR_LISTEN_SAMPLES_MAX||c->samples%320||!c->pcm_capacity||c->pcm_capacity>960000||c->pcm_capacity%2){printf("NATIVE_PLATFORM create_stage=1 result=%d\n",-EINVAL);return NULL;}
 if(c->always_wake){
  if(!c->speech||!c->speech->listen)return NULL;
#if !defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) || !defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) || !defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
  return NULL;
#endif
 }
 if(c->confirmed_barge&&(!c->scoped_dialogue||!c->consume_confirmed_wake||!c->confirmed_wake_arg))return NULL;
 if(c->onset_barge){
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
  const struct vv_barge_driver *d=c->onset_driver;
  if(!c->scoped_dialogue||c->confirmed_barge||!d||!d->authority.enter||!d->authority.leave||
     !d->tail.frame||!d->tail.now_ms||!d->onset_support||!d->attach||!d->detach)return NULL;
#else
  return NULL;
#endif
 }
 if(c->scoped_dialogue){
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
  if(!c->speech||!c->speech->listen||!c->speech->chat_speak_scoped)return NULL;
#else
  return NULL;
#endif
 }
 int stage=2,error=-ENOMEM;
 struct vv_native_platform*p=calloc(1,sizeof(*p));if(!p){printf("NATIVE_PLATFORM create_stage=2 result=%d\n",-ENOMEM);return NULL;}p->c=*c;atomic_init(&p->authenticated,false);atomic_init(&p->expires,0);

#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 atomic_init(&p->event_enabled,false);
 if(c->confirmed_barge||c->onset_barge){
  error=-pthread_mutex_init(&p->event_scope_lock,NULL);if(error)goto fail;
  p->event_scope_lock_ready=true;
 }
#endif
 struct vv_native_client_config nc={.tls=c->tls,.limits=c->limits,.host=host,.base_path=base,.asr_path=asr,.tts_path=tts,.port=443,.tts_device_bearer=false,.utc_now=c->utc_now,.clock_arg=c->clock_arg,.speech=c->speech,.development_lan_http=c->development_lan_http};
#if K7_REAL_SHUIGUANG_PROFILE
 if(!c->speech||!c->speech->asr||!c->speech->tts||!vv_real_business_target(nc.host,nc.port,nc.base_path,nc.development_lan_http)){error=-EINVAL;goto fail;}
#else
 if(c->development_lan_http){nc.host="10.3.3.170";nc.port=18085;nc.base_path="";}
#endif
 stage=3;error=-EIO;p->client=vv_native_client_create(&nc);if(!p->client)goto fail;
 stage=4;error=-ENOMEM;p->pcm=malloc(c->pcm_capacity);if(!p->pcm)goto fail;
 stage=5;error=vv_native_mode_k7_init(&p->mode,mode_cancel,mode_quiet,p);if(error)goto fail;
 vv_native_assessment_init(&p->assessment);p->backend=(struct vv_assessment_backend_adapter){vv_native_client_backend(p->client),{nc.host,nc.port,tts},false};
 p->assessment.ops=&vv_assessment_backend_ops;p->assessment.ops_arg=&p->backend;p->assessment.now_ms=c->tls.now_ms;p->assessment.clock_arg=c->tls.now_arg;p->assessment.wait_ms=wait_ms;p->assessment.wait_arg=p;
 vv_report_audio_init(&p->report);p->speaker=(struct vv_report_audio_k7){p->client,assessment_ready,p};p->report.ops=&vv_report_audio_k7_ops;p->report.arg=&p->speaker;
 struct vv_native_runtime_config rc={.client=p->client,.mode=&p->mode,.assessment=&p->assessment,.report=&p->report,.pcm=p->pcm,.pcm_capacity=c->pcm_capacity,.assessment_ready=assessment_ready};vv_native_runtime_k7_ops(&rc.ops,ready,p);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 struct vv_dialogue_ops dc={.ask=dialogue_ask,.speak=dialogue_speak,.now_ms=dialogue_now,.arg=p,
  .resident_idle=c->always_wake};
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(c->speech&&c->speech->chat_speak)dc.ask_speak=dialogue_stream;
 if(dc.ask_speak&&c->speech->speak_literal)dc.local_reply=dialogue_local_reply;
#endif
 stage=6;error=vv_dialogue_init(&p->dialogue,&dc,120000);if(error)goto fail;
 rc.dialogue=&p->dialogue;
#if defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 if(c->scoped_dialogue){rc.scoped_reply=scoped_reply;rc.scoped_listen_admit=scoped_listen_admit;rc.scoped_arg=p;}
 if(c->always_wake){rc.command_admit=idle_command_admit;rc.command_admit_arg=p;}
 if(c->onset_barge){
  struct vv_barge_route_config bc={.authority=c->onset_driver->authority,.tail=c->onset_driver->tail,
    .onset_support=c->onset_driver->onset_support,
    .observe=barge_observe,.arg=p};
  error=vv_barge_route_create(&bc,&p->barge);if(error)goto fail;
 }
#endif
#endif
 stage=6;error=vv_native_runtime_init(&p->runtime,&rc);if(error)goto fail;
 return p;
 fail:printf("NATIVE_PLATFORM create_stage=%d result=%d pcm_capacity=%lu\n",stage,error,(unsigned long)c->pcm_capacity);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
#if defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT)
 if(p->barge)(void)vv_barge_route_destroy(p->barge);
#endif
 if(p->event_scope_lock_ready)pthread_mutex_destroy(&p->event_scope_lock);
#endif
 if(p->pcm){vv_https_secure_zero(p->pcm,c->pcm_capacity);free(p->pcm);}vv_native_client_destroy(p->client);vv_https_secure_zero(p,sizeof(*p));free(p);return NULL;
}
int vv_native_platform_login(struct vv_native_platform*p,const char*credential,const char*version,const char*proof,const struct vv_https_cancel*cancel){
 if(!p)return -EINVAL;
 if(p->bound)return -EBUSY;
 unsigned bits=vv_native_platform_missing(p)&~(VV_NATIVE_AUTH|VV_NATIVE_BUSINESS_AUTH|VV_NATIVE_PREVIEW|VV_NATIVE_OWNER|VV_NATIVE_METADATA);
 if(bits)return -EAGAIN;
 atomic_store(&p->authenticated,false);atomic_store(&p->expires,0);
 struct vv_backend_client*b=vv_native_client_backend(p->client);int rc=login_business(p,credential,version,proof,cancel);
 if(!rc){atomic_store(&p->expires,b->expires_at);atomic_store(&p->authenticated,true);}return rc;
}
int vv_native_platform_bind(struct vv_native_platform*p){if(!p)return -EINVAL;if(p->bound)return -EALREADY;
 if((p->c.entropy_status==NULL)!=(p->c.entropy_refresh==NULL))return -EINVAL;
#if !defined(CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE) || !defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) || !defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) || !defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 if(p->c.entropy_status||p->c.entropy_refresh)return -ENOTSUP;
#endif
 struct vv_native_service_config c={.runtime=&p->runtime,.missing=missing,.claim=claim,.release=release,
  .make_request=make,.release_request=release_request,.sleep_ms=sleep_ms,.arg=p,.samples=p->c.samples};
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 c.maintenance=maintain_business;
#endif
 int rc=0;
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if((p->c.trusted_time_status==NULL)!=(p->c.trusted_time_refresh==NULL))return -EINVAL;
 if(p->c.trusted_time_status){
  if(!p->c.scoped_dialogue)return -ENOTSUP;
  c.time_maintenance=maintain_time;
 }
#else
 if(p->c.trusted_time_status||p->c.trusted_time_refresh)return -ENOTSUP;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(p->c.entropy_status){
  if(!p->c.scoped_dialogue)return -ENOTSUP;
  c.time_maintenance=maintain_time;
 }
#endif
 if(p->c.always_wake){
  p->idle_events=(struct vv_voice_idle_ops){idle_epoch,p};
  rc=vv_voice_session_idle_register(&p->idle_events);if(rc)return rc;
  p->idle_registered=true;c.listen_admit=idle_admission;c.listen_release=idle_release;
 }
 if(p->barge){
  rc=p->c.onset_driver->attach(p->c.onset_driver->arg,p->barge);
  if(rc){if(p->idle_registered){int e=vv_voice_session_idle_unregister(&p->idle_events);if(e)return e;p->idle_registered=false;}return rc;}
  p->barge_attached=true;c.listen_admit=idle_admission;c.listen_release=idle_release;
 }
 if(p->c.confirmed_barge){
  p->events=(struct vv_voice_event_ops){event_target,event_submit,p};
  rc=vv_voice_session_event_register(&p->events);
  if(rc){if(p->idle_registered){int e=vv_voice_session_idle_unregister(&p->idle_events);if(e)return e;p->idle_registered=false;}return rc;}
  p->event_registered=true;
 }
#endif
 rc=vv_native_service_register(&c);
 if(!rc){p->bound=true;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  atomic_store(&p->event_enabled,p->c.confirmed_barge);
#endif
 }
#if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 else if(p->event_registered){
  int e=vv_voice_session_event_unregister(&p->events);
  if(e)return e;
  p->event_registered=false;
 }
#endif
 #if defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 #ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(rc&&p->barge_attached){
  vv_barge_route_close(p->barge);
  int e=p->c.onset_driver->detach(p->c.onset_driver->arg,p->barge);if(e)return e;
  p->barge_attached=false;
 }
 #endif
 if(rc&&p->idle_registered){int e=vv_voice_session_idle_unregister(&p->idle_events);if(e)return e;p->idle_registered=false;}
#endif
 return rc;}
int vv_native_platform_destroy(struct vv_native_platform*p){
 if(!p)return 0;
 if(p->bound){int rc=vv_native_service_unregister();if(rc)return rc;p->bound=false;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 /* Service unregister retires the serial owner. Disable and drain the
  * synchronous observer pin before touching its borrowed scope/platform. */
 atomic_store(&p->event_enabled,false);
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 #ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 if(p->barge){
  vv_barge_route_close(p->barge);
  if(p->barge_attached){int rc=p->c.onset_driver->detach(p->c.onset_driver->arg,p->barge);if(rc)return rc;p->barge_attached=false;}
  int rc=vv_barge_route_destroy(p->barge);if(rc)return rc;p->barge=NULL;
 }
 #endif
 if(p->idle_registered){int rc=vv_voice_session_idle_unregister(&p->idle_events);if(rc)return rc;p->idle_registered=false;}
 if(p->event_registered){
  int rc=vv_voice_session_event_unregister(&p->events);if(rc)return rc;
  p->event_registered=false;
 }
#endif
 /* Unknown reply retirement retains every borrowed resource. */
 if(atomic_load(&p->runtime.busy)||atomic_load(&p->dialogue.busy))return -EBUSY;
 if(p->reply_scope){int rc=vv_reply_scope_destroy(p->reply_scope);if(rc)return rc;p->reply_scope=NULL;}
 if(p->event_scope_lock_ready){int e=pthread_mutex_destroy(&p->event_scope_lock);if(e)return -e;p->event_scope_lock_ready=false;}
#endif
 vv_native_client_destroy(p->client);vv_https_secure_zero(p->pcm,p->c.pcm_capacity);free(p->pcm);
 vv_https_secure_zero(p,sizeof(*p));free(p);return 0;
}

#endif

#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION && !(defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT && defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND && defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT && defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION))
int vv_native_platform_prompt_admit(struct vv_native_runtime *runtime,const struct vv_https_cancel *cancel){(void)runtime;(void)cancel;return 0;}
#endif

#if !K7_REAL_SHUIGUANG_PROFILE || !defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) || !defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
int vv_native_platform_wait_task_progress(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *cancel,
 int (*started)(void *,const struct vv_https_cancel *),void *started_arg){
 (void)runtime_arg;(void)id;(void)cancel;(void)started;(void)started_arg;
 if(task)memset(task,0,sizeof(*task));
 if(brief)memset(brief,0,sizeof(*brief));
 return -ENOTSUP;
}
int vv_native_platform_wait_task(void *runtime_arg,const char *id,
 struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *cancel){
 return vv_native_platform_wait_task_progress(runtime_arg,id,task,brief,cancel,NULL,NULL);
}
#endif

#if !K7_REAL_SHUIGUANG_PROFILE || !defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE) || !defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
int vv_native_platform_upload_task_progress(void *r,const struct vv_assessment_request *q,
 char accepted[37],const struct vv_https_cancel *cancel,
 int (*started)(void *,const struct vv_https_cancel *),void *arg){
 (void)r;(void)q;(void)cancel;(void)started;(void)arg;
 if(accepted)memset(accepted,0,37);
 return -ENOTSUP;
}
#endif
