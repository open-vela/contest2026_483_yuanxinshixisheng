#include "vv_real_shuiguang.h"
#include "vv_native_service.h"
#include "vv_photo_prompt_recovery.h"
#include <limits.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) && \
    defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
#include "vv_paraformer_k7.h"
#endif
#include <pthread.h>
#ifdef CONFIG_SMP
#include <sched.h>
#if defined(K7_CPU_ROLE_SPLIT) && defined(CONFIG_SMP_NCPUS) && CONFIG_SMP_NCPUS > 7
/* Session orchestration and remote requests leave A72 CPU7 for VAD. */
#define VV_NATIVE_SERVICE_CPU 0
#elif defined(CONFIG_SMP_NCPUS) && CONFIG_SMP_NCPUS > 7
#define VV_NATIVE_SERVICE_CPU 7
#else
#define VV_NATIVE_SERVICE_CPU 0
#endif
#endif
#include <errno.h>
#include <string.h>
#include <stdio.h>
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_voice_session.h"
#endif
#if defined(K7_READY_CUE) && K7_READY_CUE
extern int vv_native_runtime_ready_cue(struct vv_native_runtime *);
#endif
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct vv_native_service_config config;
static pthread_t thread;
static bool registered,running,joinable,finished;
static atomic_bool stopping;
static int last_error,report_error;static bool report_available;
static struct vv_assessment_result last_assessment;static struct vv_report_audio_result last_audio;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
/* The serial owner calls this only outside provider/upload/playback calls.
 * One replacement per explicit service run bounds repeated hardware faults. */
static int restore_audio(struct vv_native_runtime *runtime,unsigned *restarts){
 int failed=vv_voice_session_health();if(!failed)return 0;
 if(atomic_load(&stopping)||atomic_load(&runtime->cancelled))return -ECANCELED;
 if(*restarts)return failed;
 ++*restarts;
 int rc=vv_voice_session_restart(runtime);
 printf("VOICE_SESSION recovery=%u cause=%d result=%d\n",*restarts,failed,rc);fflush(stdout);
 return rc;
}
#endif
static bool maintenance_cancelled(void *arg){
 struct vv_native_runtime *r=arg;
 return atomic_load(&stopping)||atomic_load(&r->cancelled);
}
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
/* The runtime result proves the failing operation was a PHOTO streaming cue,
 * and that it requested HOLD. Only the live physical owner can authorize a
 * fresh listen; never restart/forgive a failed audio pipeline here. */
static int photo_prompt_recovery(struct vv_native_runtime *r,unsigned previous){
 if(maintenance_cancelled(r))return -ECANCELED;
 if(previous==UINT_MAX||vv_native_mode_epoch(r->config.mode)!=previous+1u||
    r->pending||r->config.mode->assessment_active)return -ESTALE;
 if(!r->config.ops.ready(r->config.ops.arg))return -ENETDOWN;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 struct vv_native_mode_ops *o=&r->config.mode->ops;
 uint64_t began=o->now_ms(o->arg),last=began;
 for(unsigned i=0;i<=300;i++){
  if(maintenance_cancelled(r))return -ECANCELED;
  if(vv_native_mode_epoch(r->config.mode)!=previous+1u||r->pending||r->config.mode->assessment_active)return -ESTALE;
  if(!r->config.ops.ready(r->config.ops.arg))return -ENETDOWN;
  int health=vv_voice_session_health();if(health)return health;
  uint64_t now=o->now_ms(o->arg);
  if(now<last||now<began||now-began>=3000)return -ETIMEDOUT;
  last=now;
  int ack=o->status(o->arg,0);
  if(ack!=-EAGAIN){
   if(ack)return ack;
   if(maintenance_cancelled(r))return -ECANCELED;
   if(vv_native_mode_epoch(r->config.mode)!=previous+1u)return -ESTALE;
   return vv_voice_session_health();
  }
  if(i==300)return -ETIMEDOUT;
  o->sleep_ms(o->arg,10);
 }
 return -ETIMEDOUT;
#else
 return VV_HTTPS_EPROTO; /* No physical-health evidence: retain old failure. */
#endif
}
#endif
/* Only join an already finished worker, which has no remaining lock use. */
static int reap(void){if(joinable&&finished){int rc=pthread_join(thread,NULL);if(rc)return -rc;joinable=false;finished=false;}return 0;}
static bool valid(const struct vv_native_service_config *c){return c&&c->runtime&&c->missing&&c->claim&&c->release&&c->make_request&&c->release_request&&c->sleep_ms&&c->samples>=320&&c->samples<=VV_ASR_LISTEN_SAMPLES_MAX&&c->samples%320==0&&((c->listen_admit==NULL)==(c->listen_release==NULL));}
int vv_native_service_register(const struct vv_native_service_config *c){if(!valid(c))return -EINVAL;pthread_mutex_lock(&lock);int rc=running?-EBUSY:reap();if(!rc){config=*c;registered=true;last_error=0;}pthread_mutex_unlock(&lock);return rc;}
int vv_native_service_unregister(void){pthread_mutex_lock(&lock);int rc=running?-EBUSY:reap();if(!rc){memset(&config,0,sizeof(config));registered=false;}pthread_mutex_unlock(&lock);return rc;}
int vv_native_service_status(struct vv_native_service_status *out){if(!out)return -EINVAL;pthread_mutex_lock(&lock);*out=(struct vv_native_service_status){.configured=registered,.running=running,.stopping=atomic_load(&stopping),.missing=registered?config.missing(config.arg):VV_NATIVE_CONFIG,.last_error=last_error,.report_available=report_available,.report_error=report_error,.assessment=last_assessment,.audio=last_audio};pthread_mutex_unlock(&lock);return 0;}
/* A finalized in-wait spoken STOP cancels only its admitted report. It is
 * not a generic cancellation recovery and must never clear global stop state,
 * restart audio, resume TRACK or reconstruct/replay a released request. */
static int report_voice_stop_result(struct vv_native_runtime *runtime,unsigned expected,int result){
#if K7_REAL_SHUIGUANG_PROFILE
 if(result!=VV_REAL_WAIT_VOICE_STOP||expected==UINT_MAX)return result;
 if(!runtime||atomic_load(&stopping)||atomic_load(&runtime->cancelled)||
    runtime->pending||!runtime->config.mode||runtime->config.mode->assessment_active||
    !runtime->config.mode->ops.status)return result;
 struct vv_native_mode *mode=runtime->config.mode;
 if(vv_native_mode_epoch(mode)!=expected+1u)return result;
 if(mode->ops.status(mode->ops.arg,0))return result;
 /* The physical status read is not permission to swallow a concurrent stop. */
 if(atomic_load(&stopping)||atomic_load(&runtime->cancelled)||runtime->pending||
    runtime->config.mode!=mode||mode->assessment_active||vv_native_mode_epoch(mode)!=expected+1u)return result;
 printf("NATIVE_REPORT cancelled_report=1 listening_continue=1 upload=0 replay=0\n");fflush(stdout);
 return 0;
#else
 (void)runtime;(void)expected;return result;
#endif
}

static void *worker(void *unused){(void)unused;struct vv_native_service_config c=config;struct vv_assessment_request request={0};bool have_request=false;unsigned retries=0;int rc=vv_native_runtime_rearm(c.runtime);
#ifdef CONFIG_SMP
 printf("NATIVE_SERVICE configured_cpu=%u\n",(unsigned)VV_NATIVE_SERVICE_CPU);fflush(stdout);
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 unsigned audio_restarts=0;
 if(!rc)rc=vv_voice_session_start(c.runtime);
#if defined(K7_READY_CUE) && K7_READY_CUE
 /* start() waits for model and live RX ready. Never announce after failure. */
 if(!rc)rc=vv_voice_session_health();
 if(!rc&&!maintenance_cancelled(c.runtime))rc=vv_native_runtime_ready_cue(c.runtime);
 if(!rc&&maintenance_cancelled(c.runtime))rc=-ECANCELED;
 if(!rc)rc=vv_voice_session_health();
 printf("VOICE_READY admission_ready=%u rc=%d\n",(unsigned)(rc==0),rc);fflush(stdout);
#endif
#endif
 while(!rc&&!atomic_load(&stopping)){
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
  rc=restore_audio(c.runtime,&audio_restarts);if(rc)break;
#endif
  if(c.time_maintenance){
   struct vv_https_cancel token={maintenance_cancelled,c.runtime};
   bool safe=!have_request&&!c.runtime->pending&&
     !(c.runtime->config.mode&&c.runtime->config.mode->assessment_active);
   int time_rc=c.time_maintenance(c.arg,safe,&token);
   if(maintenance_cancelled(c.runtime)){rc=-ECANCELED;break;}
   if(time_rc<0){rc=time_rc;break;}
   if(time_rc>1){rc=-EPROTO;break;}
   if(time_rc){
    pthread_mutex_lock(&lock);last_error=-EAGAIN;pthread_mutex_unlock(&lock);
    c.sleep_ms(c.arg,100);continue;
   }
  }
  unsigned missing=c.missing(c.arg)&~VV_NATIVE_BUSINESS_AUTH;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PERSISTENT_LISTEN
  if(missing==VV_NATIVE_NETWORK){
#if K7_REAL_SHUIGUANG_PROFILE
   /* Only a previously admitted current round may finish with a local
    * failure. Runtime rechecks trust, STOP, photo epoch/mask and applied HOLD.
    * Partial captures remain pending; never start ASR/network in this branch. */
   if(have_request){
    if(maintenance_cancelled(c.runtime)){rc=-ECANCELED;break;}
    if(!vv_native_platform_local_failure_ready(c.runtime)){rc=-ENETDOWN;break;}
    struct vv_assessment_result result={0};struct vv_report_audio_result audio={0};
    unsigned report_epoch=vv_native_mode_epoch(c.runtime->config.mode);
    int r=vv_native_runtime_report(c.runtime,&request,&result,&audio);
    if(r!=-EAGAIN){
     pthread_mutex_lock(&lock);last_assessment=result;last_audio=audio;report_error=r;report_available=true;pthread_mutex_unlock(&lock);
     c.release_request(c.arg,&request);memset(&request,0,sizeof(request));have_request=false;r=report_voice_stop_result(c.runtime,report_epoch,r);
     if(r){rc=r;break;}
    }
   }
#endif
   /* No network operation or implicit mode change while offline. Local
    * control and capture ownership are handled independently. */
   pthread_mutex_lock(&lock);last_error=-ENETDOWN;pthread_mutex_unlock(&lock);
   c.sleep_ms(c.arg,100);continue;
  }
#endif
  if(missing){rc=-ENETDOWN;break;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  if(c.maintenance&&!have_request&&!c.runtime->pending&&
     !(c.runtime->config.mode&&c.runtime->config.mode->assessment_active)){
   struct vv_https_cancel token={maintenance_cancelled,c.runtime};
   if(maintenance_cancelled(c.runtime)){rc=-ECANCELED;break;}
   int maintenance=c.maintenance(c.arg,&token);
   if(maintenance){pthread_mutex_lock(&lock);last_error=maintenance;pthread_mutex_unlock(&lock);}
   if(maintenance_cancelled(c.runtime)){rc=-ECANCELED;break;}
  }
#endif
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
  if(have_request){
   struct vv_assessment_result result={0};struct vv_report_audio_result audio={0};
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
   unsigned progress_epoch=vv_native_mode_epoch(c.runtime->config.mode);
#endif
   int r=vv_native_runtime_photo_progress(c.runtime);
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
   bool prompt_failed=r==VV_NATIVE_PHOTO_PROMPT_TRUNCATED;
#else
   bool prompt_failed=false;
#endif
   unsigned report_epoch=vv_native_mode_epoch(c.runtime->config.mode);if(!r)r=vv_native_runtime_report(c.runtime,&request,&result,&audio);
   if(r!=-EAGAIN){
    pthread_mutex_lock(&lock);last_assessment=result;last_audio=audio;
    report_error=prompt_failed?VV_HTTPS_EPROTO:r;report_available=true;pthread_mutex_unlock(&lock);
    c.release_request(c.arg,&request);memset(&request,0,sizeof(request));have_request=false;r=report_voice_stop_result(c.runtime,report_epoch,r);
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
    if(prompt_failed){
     rc=photo_prompt_recovery(c.runtime,progress_epoch);if(rc)break;
     pthread_mutex_lock(&lock);last_error=VV_HTTPS_EPROTO;pthread_mutex_unlock(&lock);
     printf("NATIVE_PHOTO command_failed=1 rc=%d listening_continue=1 upload=0 replay=0\n",VV_HTTPS_EPROTO);fflush(stdout);
     retries=0;c.sleep_ms(c.arg,100);continue;
    }
#endif
    if(r){rc=r;break;}
   }
  }
#else
  if(have_request){struct vv_assessment_result result={0};struct vv_report_audio_result audio={0};int r=vv_native_runtime_photo_progress(c.runtime);unsigned report_epoch=vv_native_mode_epoch(c.runtime->config.mode);if(!r)r=vv_native_runtime_report(c.runtime,&request,&result,&audio);if(r!=-EAGAIN){pthread_mutex_lock(&lock);last_assessment=result;last_audio=audio;report_error=r;report_available=true;pthread_mutex_unlock(&lock);c.release_request(c.arg,&request);memset(&request,0,sizeof(request));have_request=false;r=report_voice_stop_result(c.runtime,report_epoch,r);if(r){rc=r;break;}}}
#endif
  if(atomic_load(&stopping))break;
  bool admitted=false;
  if(c.listen_admit){
   struct vv_https_cancel token={maintenance_cancelled,c.runtime};
   if(maintenance_cancelled(c.runtime)){rc=-ECANCELED;break;}
   int admission=c.listen_admit(c.arg,&token);
   if(admission<0){rc=admission;break;}
   if(admission>1){rc=-EPROTO;break;}
   if(!admission){if(maintenance_cancelled(c.runtime)){rc=-ECANCELED;break;}c.sleep_ms(c.arg,20);continue;}
   admitted=true;
   if(maintenance_cancelled(c.runtime)){c.listen_release(c.arg);rc=-ECANCELED;break;}
  }
  unsigned listen_epoch=vv_native_mode_epoch(c.runtime->config.mode);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  /* This service owns dialogue/history updates. Snapshot only this operation;
   * generic interrupts change generation and never use the fast handoff. */
  struct vv_dialogue *listened_dialogue=c.runtime->config.dialogue;
  unsigned interrupted_before=listened_dialogue?listened_dialogue->interruptions:0;
  unsigned generation_before=listened_dialogue?atomic_load(&listened_dialogue->generation):0;
  uint64_t reply_before=listened_dialogue?listened_dialogue->reply_sequence:0;
#endif
  enum k7_voice_intent intent=K7_VOICE_NONE;rc=vv_native_runtime_listen(c.runtime,c.samples,&intent);
  if(admitted)c.listen_release(c.arg);
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
  if(rc==VV_NATIVE_PHOTO_PROMPT_TRUNCATED){
   rc=photo_prompt_recovery(c.runtime,listen_epoch);if(rc)break;
   pthread_mutex_lock(&lock);last_error=VV_HTTPS_EPROTO;pthread_mutex_unlock(&lock);
   printf("NATIVE_PHOTO command_failed=1 rc=%d listening_continue=1 upload=0 replay=0\n",VV_HTTPS_EPROTO);fflush(stdout);
   retries=0;c.sleep_ms(c.arg,100);continue;
  }
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
  if(rc&&!atomic_load(&stopping)&&!atomic_load(&c.runtime->cancelled)&&vv_voice_session_health()){
   /* Only a failed, retryable ASR window is retried after recovery. Once a
    * command/prompt/upload ran, its failure retains the existing semantics;
    * neither that action nor a failed trust/authentication check is replayed. */
   if(!c.runtime->listen_window_failed||
      (!c.runtime->listen_retryable&&!c.runtime->listen_owner_failed))break;
   int restored=restore_audio(c.runtime,&audio_restarts);
   if(restored){rc=restored;break;}
   rc=0;continue;
  }
#endif
  if(rc==VV_NATIVE_TRACK_ACK_TIMEOUT&&intent==K7_VOICE_TRACK){
   /* The physical-owner check above retains priority. This command failed;
    * release its admission and begin a fresh capture without replaying it. */
   if(atomic_load(&stopping)||atomic_load(&c.runtime->cancelled)||
      vv_native_mode_epoch(c.runtime->config.mode)!=listen_epoch+1u){rc=-ECANCELED;break;}
   pthread_mutex_lock(&lock);last_error=-ETIMEDOUT;pthread_mutex_unlock(&lock);
   printf("NATIVE_COMMAND command_timeout=1 listening_continue=1 rc=%d\n",-ETIMEDOUT);fflush(stdout);
   retries=0;rc=0;c.sleep_ms(c.arg,100);continue;
  }
  /* PHOTO/duplicate wake while an assessment is active is a harmless busy
   * command; capture and network -EBUSY must still terminate (intent NONE). */
  if(rc==-EBUSY&&(intent==K7_VOICE_PHOTO||intent==K7_VOICE_TRACK))rc=0;
  if(rc==-EACCES&&intent==K7_VOICE_PHOTO){puts("K7CLOUD native photo_rejected=business_auth_not_ready");pthread_mutex_lock(&lock);last_error=rc;pthread_mutex_unlock(&lock);rc=0;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_PERSISTENT_LISTEN
  /* Link can disappear between the loop's readiness snapshot and listen's
   * admission check. Recheck the precise missing bit; do not swallow trust,
   * configuration or explicit cancellation errors as a network retry. */
  if(rc==-ENETDOWN&&!atomic_load(&stopping)&&!atomic_load(&c.runtime->cancelled)){
   unsigned fresh=c.missing(c.arg)&~VV_NATIVE_BUSINESS_AUTH;
   if(!fresh||fresh==VV_NATIVE_NETWORK){
    pthread_mutex_lock(&lock);last_error=rc;pthread_mutex_unlock(&lock);
    rc=0;c.sleep_ms(c.arg,100);continue;
   }
  }
#endif
  if(rc&&rc!=-EBUSY&&c.runtime->listen_retryable&&
#ifndef CONFIG_EXAMPLES_K7CLOUD_PERSISTENT_LISTEN
     retries<3&&
#endif
     !atomic_load(&stopping)){
#ifdef CONFIG_EXAMPLES_K7CLOUD_PERSISTENT_LISTEN
   int failed=rc;unsigned delay=1000u<<(retries<3?retries:3);
   if(retries<1000000u)++retries;
   rc=0;
   pthread_mutex_lock(&lock);last_error=failed;pthread_mutex_unlock(&lock);
   if(retries<=3||retries%10==0)
    printf("NATIVE_RETRY fresh_capture=1 attempt=%u rc=%d delay_ms=%u preserve_mode=1\n",retries,failed,delay);
   for(unsigned waited=0;waited<delay&&!atomic_load(&stopping);waited+=100)c.sleep_ms(c.arg,100);
   if(atomic_load(&stopping)){rc=-ECANCELED;break;}
   /* Provider returned after retiring only that failed ASR task. Keep the
    * runtime mode, photo request and outer service lifetime intact. */
   continue;
#else
   int failed=rc;unsigned delay=1000u<<retries++;
   rc=vv_native_runtime_stop(c.runtime);
   if(have_request){c.release_request(c.arg,&request);memset(&request,0,sizeof(request));have_request=false;}
   pthread_mutex_lock(&lock);last_error=failed;pthread_mutex_unlock(&lock);
   printf("NATIVE_RETRY attempt=%u rc=%d delay_ms=%u\n",retries,failed,delay);fflush(stdout);
   for(unsigned waited=0;!rc&&waited<delay&&!atomic_load(&stopping);waited+=100)c.sleep_ms(c.arg,100);
   if(atomic_load(&stopping)){rc=-ECANCELED;break;}
   if(!rc)rc=vv_native_runtime_rearm(c.runtime);
   if(rc)break;
   continue;
#endif
  }
  if(rc)break;
  retries=0;
  pthread_mutex_lock(&lock);last_error=0;pthread_mutex_unlock(&lock);
  if(intent==K7_VOICE_STOP&&have_request){c.release_request(c.arg,&request);memset(&request,0,sizeof(request));have_request=false;}
  if(c.runtime->pending&&!have_request){rc=c.make_request(c.arg,&request);if(rc){c.release_request(c.arg,&request);memset(&request,0,sizeof(request));break;}have_request=true;}
  bool fast_reply_handoff=false;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  /* Skip only the optional inter-window pause after this reply was cancelled.
   * The next iteration still checks readiness, physical owner and admission;
   * a counter change is never used as permission to claim buffered audio. */
  fast_reply_handoff=!rc&&intent==K7_VOICE_NONE&&listened_dialogue&&
    listened_dialogue==c.runtime->config.dialogue&&
    listened_dialogue->interruptions!=interrupted_before&&
    listened_dialogue->reply_sequence>reply_before&&
    atomic_load(&listened_dialogue->generation)==generation_before&&
    listened_dialogue->awake&&!atomic_load(&listened_dialogue->sleep_requested)&&
    (listened_dialogue->last_error==-ECANCELED||
     listened_dialogue->last_error==VV_HTTPS_ECANCELLED)&&
    !atomic_load(&c.runtime->cancelled)&&
    vv_native_mode_epoch(c.runtime->config.mode)==listen_epoch;
#endif
  if(!rc&&!atomic_load(&stopping)&&!fast_reply_handoff)c.sleep_ms(c.arg,100);
 }
 if(have_request)c.release_request(c.arg,&request);
 (void)vv_native_runtime_stop(c.runtime);
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) && \
    defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
 /* Retire the idle ADC owner before releasing provider/runtime resources. */
 int audio_cleanup=vv_paraformer_k7_close();
 if(audio_cleanup)rc=audio_cleanup;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 /* Every synchronous provider has returned before the borrowed owner dies. */
 int session_cleanup=vv_voice_session_stop();
 if(session_cleanup)rc=session_cleanup;
#endif
 c.release(c.arg);
 pthread_mutex_lock(&lock);last_error=rc;running=false;finished=true;pthread_mutex_unlock(&lock);return NULL;
}
int vv_native_service_run(void){pthread_mutex_lock(&lock);int rc=running?-EBUSY:reap();if(!rc&&!registered)rc=-ENODEV;if(!rc&&(config.missing(config.arg)&~VV_NATIVE_BUSINESS_AUTH))rc=-EAGAIN;
 if(!rc)rc=config.claim(config.arg);
 if(!rc){pthread_attr_t attr;int err=pthread_attr_init(&attr);bool have_attr=err==0;
  if(!err)err=pthread_attr_setstacksize(&attr,65536);
#ifdef CONFIG_SMP
  /* Keep ASR, dialogue and the TTS caller/feeder off the boot/network CPU.
   * Smaller SMP targets retain a valid CPU0 mask; non-SMP is unchanged. */
  if(!err){cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(VV_NATIVE_SERVICE_CPU,&cpus);
   err=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);}
#endif
  atomic_store(&stopping,false);last_error=0;running=true;finished=false;
  if(!err)err=pthread_create(&thread,&attr,worker,NULL);
  if(have_attr)pthread_attr_destroy(&attr);
  if(err){running=false;last_error=-err;config.release(config.arg);rc=-err;}else joinable=true;
 }
 pthread_mutex_unlock(&lock);return rc;
}
int vv_native_service_stop(void){pthread_mutex_lock(&lock);if(running){atomic_store(&stopping,true);vv_native_runtime_cancel(config.runtime);}int rc=running?0:reap();pthread_mutex_unlock(&lock);return rc;}
