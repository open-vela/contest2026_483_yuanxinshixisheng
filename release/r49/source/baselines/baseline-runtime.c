#include "vv_real_upload_private.h"
#include "vv_real_shuiguang.h"
#include "vv_real_report_speech.h"
#include "vv_local_report.h"
#include "vv_local_assessment.h"
#if K7_REAL_SHUIGUANG_PROFILE && !K7_LOCAL_ASSESSMENT
#error Real profile requires source44 capture cue and exact-epoch discard
#endif
#if K7_REAL_SHUIGUANG_PROFILE && (!defined(K7_CLOUD_BUSINESS_PROFILE) || !K7_CLOUD_BUSINESS_PROFILE)
#error Real profile requires explicit cloud business DNS budget
#endif
#include "vv_native_runtime.h"
#include "vv_voice_settings.h"
#include "vv_prompt_admission.h"
#include "vv_photo_prompt_recovery.h"
#include <limits.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_voice_prompts.inc"
#define JPEG_CAP (1024u*1024u)
static int report_prompt(struct vv_native_runtime *,const char *,const struct vv_https_cancel *);
static bool cancelled(void *arg){struct vv_native_runtime *s=arg;return atomic_load(&s->cancelled);}
static int enter(struct vv_native_runtime *s){bool no=false;return atomic_compare_exchange_strong(&s->busy,&no,true)?0:-EBUSY;}
static void leave(struct vv_native_runtime *s){atomic_store(&s->busy,false);}
static void hold(struct vv_native_runtime *s){s->pending=false;(void)vv_native_mode_apply(s->config.mode,K7_VOICE_STOP,vv_native_mode_epoch(s->config.mode));}
struct photo_confirmation {struct vv_native_runtime *s;uint32_t photo;unsigned done;bool advanced;
#if K7_REAL_SHUIGUANG_PROFILE
 bool network_lost,held;unsigned revision;
#endif
};
#if K7_REAL_SHUIGUANG_PROFILE
static bool report_network_only(struct vv_native_runtime *s){
 return !s->config.ops.ready(s->config.ops.arg)&&vv_native_platform_local_failure_ready(s);
}
#endif
static bool photo_cancelled(void *arg){
 struct photo_confirmation *p=arg;struct vv_native_runtime*s=p->s;
 if(cancelled(s)||!s->pending||s->pending_epoch!=vv_native_mode_epoch(s->config.mode))return true;
 if(!s->config.ops.ready(s->config.ops.arg)){
#if K7_REAL_SHUIGUANG_PROFILE
  if(vv_native_platform_local_failure_ready(s))p->network_lost=true;
#endif
  return true;
 }
 uint32_t epoch=0;unsigned done=0;
 if(s->config.ops.photo_status(s->config.ops.arg,&epoch,&done)||epoch!=p->photo)return true;
 if(done!=p->done){p->advanced=true;return true;}
 return false;
}
static const char *photo_text(const char *key){
 if(!strcmp(key,"capture_retained"))return K7_LOCAL_ASSESSMENT?VV_LOCAL_CAPTURE_COMPLETE:"三视图拍摄完成，正在处理，请稍候。";
 for(unsigned i=0;i<sizeof(k7_voice_prompts)/sizeof(k7_voice_prompts[0]);i++)if(!strcmp(key,k7_voice_prompts[i].key))return k7_voice_prompts[i].text;
 return NULL;
}
static int photo_progress(struct vv_native_runtime*s){
 if(!s->pending||(!s->config.ops.play&&!vv_native_client_streaming_tts(s->config.client)))return 0;
 if(cancelled(s)||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)){hold(s);return -ECANCELED;}
 uint32_t epoch=0;unsigned done=0;int rc=s->config.ops.photo_status(s->config.ops.arg,&epoch,&done);
 if(rc){hold(s);return rc;}
 if(!epoch||epoch==s->previous_photo)return -EAGAIN;
 /* A reset during an already announced assessment invalidates this admission;
  * a new PHOTO command must establish a new pending mode epoch. */
 if((s->photo_prompt.epoch&&s->photo_prompt.epoch!=epoch)||(s->photo_saved.epoch&&s->photo_saved.epoch!=epoch)){hold(s);return -ECANCELED;}
#if K7_REAL_SHUIGUANG_PROFILE
 /* Only the online complete round defers. Report will freeze all three JPEGs,
  * consume admission once, and play this unchanged plan alongside one POST.
  * Leave both cursors untouched here. Partial/offline/auth paths stay original. */
 if(done==7&&s->config.ops.ready(s->config.ops.arg)&&
    (!s->config.assessment_ready||s->config.assessment_ready(s->config.ops.arg)))return 0;
#endif
 const char *keys[4];int count=k7_photo_prompt_plan(&s->photo_prompt,epoch,done,keys);
 if(count<0){hold(s);return count;}
 struct photo_confirmation p={.s=s,.photo=epoch,.done=done};
#if K7_REAL_SHUIGUANG_PROFILE
 /* Only the completed round can yield to offline failure handling. Never
  * announce capture success or keep a partial network cue alive offline. */
 if(done==7){
  p.held=!s->config.ops.hold_revision(s->config.ops.arg,&p.revision);
  if(p.held&&report_network_only(s))return 0;
 }
#endif
 struct vv_https_cancel token={photo_cancelled,&p};
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
 bool truncated_stream=false;
#endif
 for(int i=0;i<count;i++){
  unsigned saved=!strcmp(keys[i],"front_saved")?1u:!strcmp(keys[i],"left_saved")?2u:!strcmp(keys[i],"right_saved")?4u:0u;
  if(saved&&s->photo_saved.epoch==epoch&&(s->photo_saved.done&saved))continue;
  size_t bytes=0;const char *text=photo_text(keys[i]);
  if(!text){rc=-EINVAL;break;}
  if(photo_cancelled(&p)){rc=-ECANCELED;break;}
#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION
  rc=vv_native_platform_prompt_admit(s,&token);if(rc)break;
#endif
  if(vv_native_client_streaming_tts(s->config.client)){
   rc=vv_native_client_speak(s->config.client,text,&token);
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
   truncated_stream=rc==VV_HTTPS_EPROTO;
#endif
  }
  else{
   rc=vv_native_client_tts(s->config.client,text,s->config.pcm,s->config.pcm_capacity,&bytes,&token);
   if(photo_cancelled(&p)&&!rc)rc=-ECANCELED;
   if(!rc&&(!bytes||bytes>s->config.pcm_capacity||bytes%2))rc=-EPROTO;
   if(!rc)rc=s->config.ops.play(s->config.ops.arg,s->config.pcm,bytes,&token);
  }
  if(photo_cancelled(&p)&&!rc)rc=-ECANCELED;
  vv_https_secure_zero(s->config.pcm,s->config.pcm_capacity);
  if(rc)break;
  /* Commit only a fully played, still-current saved announcement. Keep the
   * whole-plan cursor separate so interrupted alignment/retained still runs. */
  if(saved){s->photo_saved.epoch=epoch;s->photo_saved.done|=saved;}
 }
 vv_https_secure_zero(s->config.pcm,s->config.pcm_capacity);
 if(rc){
#if K7_REAL_SHUIGUANG_PROFILE
  if((rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&p.network_lost&&p.done==7&&p.held&&
     !cancelled(s)&&s->pending&&s->pending_epoch==vv_native_mode_epoch(s->config.mode)&&
     vv_native_platform_local_failure_ready(s)){
   uint32_t fresh=0;unsigned mask=0,revision=0;
   if(!s->config.ops.photo_status(s->config.ops.arg,&fresh,&mask)&&fresh==p.photo&&mask==7&&
      !s->config.ops.hold_revision(s->config.ops.arg,&revision)&&revision==p.revision)
    return -EAGAIN; /* Retain one-shot admission for explicit failure/report. */
  }
#endif
  if((rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&p.advanced&&!cancelled(s)&&s->pending_epoch==vv_native_mode_epoch(s->config.mode)){
   uint32_t fresh=0;unsigned mask=0;
   if(!s->config.ops.photo_status(s->config.ops.arg,&fresh,&mask)&&fresh==epoch&&s->config.ops.ready(s->config.ops.arg))return -EAGAIN;
  }
#if defined(K7_PHOTO_PROMPT_RECOVERY) && K7_PHOTO_PROMPT_RECOVERY
  /* Only a failed streaming PHOTO cue may use command-local recovery. Keep
   * the provider error visible, and never turn stale/cancelled audio into a
   * successful prompt. The service must independently verify physical health. */
  if(truncated_stream){
   printf("NATIVE_PHOTO prompt_failed=1 rc=%d photo=%lu mask=%u\n",rc,(unsigned long)epoch,done);fflush(stdout);
   unsigned owner=s->pending_epoch;
   if(cancelled(s)||owner!=vv_native_mode_epoch(s->config.mode)){hold(s);return -ECANCELED;}
   if(owner!=UINT_MAX&&!photo_cancelled(&p)){
    s->pending=false;
    int stopped=vv_native_mode_apply(s->config.mode,K7_VOICE_STOP,owner);
    if(stopped)return stopped;
    if(cancelled(s)||vv_native_mode_epoch(s->config.mode)!=owner+1u)return -ECANCELED;
    if(s->config.mode->assessment_active)return -ESTALE;
    return VV_NATIVE_PHOTO_PROMPT_TRUNCATED;
   }
  }
#endif
  hold(s);return rc;
 }
 s->photo_prompt=(struct k7_photo_prompt_state){epoch,done};return 0;
}
int vv_native_runtime_photo_progress(struct vv_native_runtime*s){if(!s)return -EINVAL;int rc=enter(s);if(rc)return rc;rc=photo_progress(s);leave(s);return rc;}
struct confirmation {struct vv_native_runtime *runtime;unsigned epoch;};
static bool confirmation_cancelled(void *arg){
 struct confirmation *p=arg;struct vv_native_runtime *s=p->runtime;
 return cancelled(s)||p->epoch!=vv_native_mode_epoch(s->config.mode)||
   !s->config.ops.ready(s->config.ops.arg);
}
#if defined(K7_REWAKE_PROMPT) && K7_REWAKE_PROMPT
#include "vv_wake_ack.h"
/* Snapshot BEFORE mode_apply: active tracking + sleeping conversation is a
 * voice-only wake. HOLD/STOP always keeps the original tracking-start prompt.
 * A failed/unknown status never claims that tracking was already active. */
static bool voice_only_wake(struct vv_native_runtime *s,enum k7_voice_intent intent){
 return intent==K7_VOICE_TRACK&&s->config.mode->ops.status(s->config.mode->ops.arg,1)==0;
}
#endif
static int confirm(struct vv_native_runtime *s,enum k7_voice_intent intent,unsigned epoch,bool speech_wake){
 if((!s->config.ops.play&&!vv_native_client_streaming_tts(s->config.client))||(intent!=K7_VOICE_TRACK&&intent!=K7_VOICE_STOP))return 0;
 struct confirmation p={s,epoch};struct vv_https_cancel token={confirmation_cancelled,&p};
 int rc=0;size_t bytes=0;
 if(confirmation_cancelled(&p)){rc=-ECANCELED;goto done;}
 /* STOP requests hold before any network/audio, but its mode primitive does
  * not await the worker ACK. Do not announce success until hold is applied. */
 if(intent==K7_VOICE_STOP){
  struct vv_native_mode_ops *o=&s->config.mode->ops;
  uint64_t began=o->now_ms(o->arg);
  for(unsigned i=0;i<=300;i++){
   if(confirmation_cancelled(&p)){rc=-ECANCELED;break;}
   uint64_t now=o->now_ms(o->arg);
   if(now<began||now-began>=3000){rc=-ETIMEDOUT;break;}
   rc=o->status(o->arg,0);if(rc!=-EAGAIN)break;
   if(i==300){rc=-ETIMEDOUT;break;}o->sleep_ms(o->arg,10);
  }
  uint64_t finished=o->now_ms(o->arg);
  if(finished<began||finished-began>=3000)rc=-ETIMEDOUT;
  if(rc)goto done;
 }
 if(confirmation_cancelled(&p)){rc=-ECANCELED;goto done;}
#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION
 rc=vv_native_platform_prompt_admit(s,&token);if(rc)goto done;
#endif
 const char *text=intent==K7_VOICE_TRACK?"OpenVela 云台已启动。":"云台跟踪已停止。";
#if defined(K7_REWAKE_PROMPT) && K7_REWAKE_PROMPT
 if(intent==K7_VOICE_TRACK){
  text=speech_wake?VV_REWAKE_TEXT:VV_INITIAL_WAKE_TEXT;
  printf("WAKE_ACK voice_only=%u\n",(unsigned)speech_wake);fflush(stdout);
 }
#else
 (void)speech_wake;
#endif
 if(vv_native_client_streaming_tts(s->config.client))rc=vv_native_client_speak(s->config.client,text,&token);
 else{
  rc=vv_native_client_tts(s->config.client,text,s->config.pcm,s->config.pcm_capacity,&bytes,&token);
  if(confirmation_cancelled(&p))rc=-ECANCELED;
  if(!rc&&(!bytes||bytes>s->config.pcm_capacity||bytes%2))rc=-EPROTO;
  if(!rc)rc=s->config.ops.play(s->config.ops.arg,s->config.pcm,bytes,&token);
 }
 if(confirmation_cancelled(&p))rc=-ECANCELED;
done:
 vv_https_secure_zero(s->config.pcm,s->config.pcm_capacity);
 if(rc)hold(s); /* Never resume tracking after failed/stale confirmation. */
 return rc;
}
int vv_native_runtime_init(struct vv_native_runtime *s,const struct vv_native_runtime_config *c){
 if(!s||!c||!c->client||!c->mode||!c->mode->ops.request||!c->mode->ops.status||!c->mode->ops.halt||!c->mode->ops.cancel_work||!c->mode->ops.quiet||!c->mode->ops.sleep_ms||!c->mode->ops.now_ms||!c->assessment||!c->assessment->ops||!c->assessment->ops->upload||!c->assessment->now_ms||!c->pcm||!c->pcm_capacity||c->pcm_capacity>960000||c->pcm_capacity%2||(uintptr_t)c->pcm%2||!c->ops.ready||!c->ops.capture||!c->ops.photo_status||!c->ops.photo_copy||!c->ops.hold_revision||!c->ops.resume)return -EINVAL;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 if((c->scoped_listen_admit!=NULL)!=(c->scoped_reply!=NULL)||
    (c->scoped_reply&&(!c->dialogue||!c->scoped_arg)))return -EINVAL;
#endif
 memset(s,0,sizeof(*s));s->config=*c;atomic_init(&s->busy,false);atomic_init(&s->cancelled,false);return 0;
}
void vv_native_runtime_interrupt_reply(struct vv_native_runtime *s){
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 if(s&&s->config.dialogue)vv_dialogue_interrupt(s->config.dialogue);
#else
 (void)s;
#endif
}
void vv_native_runtime_cancel(struct vv_native_runtime *s){if(s){vv_native_runtime_interrupt_reply(s);atomic_store(&s->cancelled,true);vv_native_mode_invalidate(s->config.mode);}}
int vv_native_runtime_stop(struct vv_native_runtime *s){if(!s)return -EINVAL;vv_native_runtime_cancel(s);int rc=enter(s);if(rc)return rc;hold(s);leave(s);return 0;}
int vv_native_runtime_rearm(struct vv_native_runtime *s){if(!s)return -EINVAL;int rc=enter(s);if(rc)return rc;if(s->pending||s->config.mode->assessment_active)rc=-EBUSY;else rc=s->config.mode->ops.status(s->config.mode->ops.arg,0);if(!rc)atomic_store(&s->cancelled,false);leave(s);return rc;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
struct listen_progress {struct vv_native_runtime *s;unsigned mode,done;uint32_t photo;bool active,awaiting;atomic_bool yielded;atomic_flag checking;};
static bool listen_photo_advanced(const struct listen_progress *p,uint32_t photo,unsigned done){
 return p->awaiting?(photo&&photo!=p->s->previous_photo):
  (photo==p->photo&&(done&p->done)==p->done&&done!=p->done);
}
static bool listen_cancelled(void *arg){
 struct listen_progress *p=arg;struct vv_native_runtime *s=p->s;
 if(cancelled(s)||p->mode!=vv_native_mode_epoch(s->config.mode))return true;
 if(atomic_load(&p->yielded))return true;
 if(!p->active||atomic_flag_test_and_set(&p->checking))return false;
 uint32_t photo=0;unsigned done=0;
 int rc=s->config.ops.photo_status(s->config.ops.arg,&photo,&done);
 bool stale=!rc&&!p->awaiting&&photo!=p->photo;
 if(!rc&&listen_photo_advanced(p,photo,done))atomic_store(&p->yielded,true);
 atomic_flag_clear(&p->checking);return stale||atomic_load(&p->yielded);
}
#endif
int vv_native_runtime_listen(struct vv_native_runtime *s,unsigned samples,enum k7_voice_intent *intent){
 if(!s||!intent||samples<320||samples>VV_ASR_LISTEN_SAMPLES_MAX||samples%320)return -EINVAL;
 int rc=enter(s);if(rc)return rc;*intent=K7_VOICE_NONE;
 s->listen_retryable=false;s->listen_window_failed=false;s->listen_owner_failed=false;
 int stage=1;
 printf("NATIVE_LISTEN stage=1 rc=0\n");fflush(stdout);
 if(atomic_load(&s->cancelled)){hold(s);rc=-ECANCELED;goto done;}
 if(!s->config.ops.ready(s->config.ops.arg)){rc=-ENETDOWN;goto done;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 if(s->config.scoped_listen_admit){
  rc=s->config.scoped_listen_admit(s->config.scoped_arg);
  if(rc)goto done; /* No microphone or retry classification after unsafe retirement. */
 }
#endif
 stage=2;
 unsigned epoch=vv_native_mode_epoch(s->config.mode);
 struct vv_https_cancel cancel={cancelled,s};struct vv_asr_batch_result result={0};
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_STREAM
 if(vv_native_client_streaming(s->config.client)){
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
  struct listen_progress progress={.s=s,.mode=epoch,.checking=ATOMIC_FLAG_INIT};
  atomic_init(&progress.yielded,false);
  if(s->pending&&s->pending_epoch!=epoch){hold(s);rc=-ECANCELED;goto done;}
  if(s->pending&&s->pending_epoch==epoch){
   rc=s->config.ops.photo_status(s->config.ops.arg,&progress.photo,&progress.done);
   if(rc)goto done;
   if(s->photo_prompt.epoch&&s->photo_prompt.epoch!=progress.photo){hold(s);rc=-ECANCELED;goto done;}
   /* A prompt can itself be overtaken by capture. Give the service its next
    * progress pass before starting another long recording window. */
   if((s->config.ops.play||vv_native_client_streaming_tts(s->config.client))&&
      progress.photo&&progress.photo!=s->previous_photo&&
      (!s->photo_prompt.epoch||progress.done!=s->photo_prompt.done)){
    printf("NATIVE_ASR photo_prompt_pending=1 mask=%u\n",progress.done);rc=0;goto done;
   }
   progress.awaiting=!s->photo_prompt.epoch&&(!progress.photo||progress.photo==s->previous_photo);
   progress.active=progress.awaiting||(progress.photo&&progress.photo!=s->previous_photo);
  }
  cancel=(struct vv_https_cancel){listen_cancelled,&progress};
#endif
  stage=5;
  printf("NATIVE_ASR stage=1 rc=0 streaming=1\n");fflush(stdout);
  rc=vv_native_client_listen(s->config.client,samples,&cancel,&result);
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
  if((rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&atomic_load(&progress.yielded)&&
     !cancelled(s)&&s->pending&&s->pending_epoch==epoch&&epoch==vv_native_mode_epoch(s->config.mode)&&s->config.ops.ready(s->config.ops.arg)){
   uint32_t fresh=0;unsigned mask=0;
   if(!s->config.ops.photo_status(s->config.ops.arg,&fresh,&mask)&&listen_photo_advanced(&progress,fresh,mask)){
    vv_https_secure_zero(&result,sizeof(result));rc=0;
    printf("NATIVE_ASR photo_progress_yield=1 mask=%u\n",mask);goto done;
   }
  }
#endif
 }else
#endif
 {
  if(samples>48000){rc=-EINVAL;goto done;} /* Batch capture retains its 3s bound. */
  size_t cap=(size_t)samples*2,used=0;unsigned char *pcm=malloc(cap);
  if(!pcm){rc=-ENOMEM;goto done;}
  stage=3;rc=s->config.ops.capture(s->config.ops.arg,samples,pcm,cap,&used,&cancel);
  printf("NATIVE_CAPTURE rc=%d used=%lu cap=%lu\n",rc,(unsigned long)used,(unsigned long)cap);fflush(stdout);
  stage=4;
  if(!rc&&(cancelled(s)||epoch!=vv_native_mode_epoch(s->config.mode)))rc=-ECANCELED;
#ifdef CONFIG_EXAMPLES_K7CLOUD_NATIVE_ENDPOINT
  if(!rc&&(!used||used>cap||used%640u))rc=-EIO;
#else
  if(!rc&&used!=cap)rc=-EIO;
#endif
  if(!rc&&!s->config.ops.ready(s->config.ops.arg))rc=-ENETDOWN;
  if(!rc){stage=5;printf("NATIVE_ASR stage=1 rc=0\n");fflush(stdout);
   rc=vv_native_client_asr(s->config.client,pcm,used,&cancel,&result);}
  vv_https_secure_zero(pcm,cap);free(pcm);
 }
 if(stage==5){
  s->listen_window_failed=rc!=0;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
  /* Set only for capture's dedicated physical-owner failure, before any
   * command dispatch. A transport response/finalized command is not proof. */
  s->listen_owner_failed=rc==VV_ASR_OWNER_UNAVAILABLE&&!result.http_status&&
                         !result.command_finalized;
#endif
  /* Preserve authentication/configuration failures. Retry only a new capture,
   * never the previous PCM or any command that already reached mode_apply. */
  if(rc&&rc!=-ECANCELED&&rc!=VV_HTTPS_ECANCELLED&&
     rc!=VV_HTTPS_ETLS_VERIFY){
   int h=result.http_status;
   s->listen_retryable=(h==429||(h>=500&&h<=599)||
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
     (!h&&rc==VV_ASR_WINDOW_OVERRUN)||
#endif
     (h==200&&rc==-EPROTO)||
     (!h&&(rc==-EAGAIN||rc==-ETIMEDOUT||rc==-ECONNRESET||rc==-ECONNREFUSED||
           rc==-EPIPE||rc==VV_HTTPS_ETIMEOUT||rc==VV_HTTPS_EIO)));
  }
  printf("NATIVE_ASR stage=2 rc=%d http=%d\n",rc,result.http_status);fflush(stdout);
 }
 if(!rc&&(cancelled(s)||epoch!=vv_native_mode_epoch(s->config.mode)))rc=-ECANCELED;
 if(!rc&&!memchr(result.text,0,sizeof(result.text)))rc=-EPROTO;
 if(!rc){
  stage=6;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
  if(vv_native_client_streaming(s->config.client)){
   if(!result.command_finalized||result.command_intent>K7_VOICE_QUIET){rc=-EPROTO;goto done;}
   *intent=(enum k7_voice_intent)result.command_intent;
  }else
#endif
  *intent=k7_voice_completed_text(result.text,strlen(result.text),true,true);
  printf("NATIVE_INTENT value=%d\n",(int)*intent);fflush(stdout);
#if defined(K7_ADDRESSED_WINDOW) && K7_ADDRESSED_WINDOW && defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE)
  /* A local keyword is only a candidate. Reject unrelated/partial speech
   * before local settings, mode changes, photo upload or dialogue dispatch.
   * Explicit STOP retains priority, including while conversation is asleep. */
  if(s->config.dialogue&&!s->config.dialogue->awake&&!s->pending&&
     !s->config.mode->assessment_active&&*intent!=K7_VOICE_TRACK&&*intent!=K7_VOICE_STOP){
   printf("DIALOGUE_GATE ignored=1 reason=waiting_wake\n");
   *intent=K7_VOICE_NONE;goto completed_result;
  }
#endif
  uint32_t before=0;unsigned mask=0;
  if(!K7_LOCAL_ASSESSMENT&&*intent==K7_VOICE_PHOTO&&s->config.assessment_ready&&!s->config.assessment_ready(s->config.ops.arg))rc=-EACCES;
  if(!rc&&*intent==K7_VOICE_PHOTO)rc=s->config.ops.photo_status(s->config.ops.arg,&before,&mask);
  if(!rc&&s->config.command_admit){
   struct confirmation guard={s,epoch};
   struct vv_https_cancel token={confirmation_cancelled,&guard};
   rc=s->config.command_admit(s->config.command_admit_arg,*intent,epoch,&token);
   /* A concurrent stop/epoch change outranks even a harmless busy rejection. */
   if(cancelled(s)||epoch!=vv_native_mode_epoch(s->config.mode))rc=-ECANCELED;
  }
  if(!rc&&*intent==K7_VOICE_NONE){
   struct confirmation guard={s,epoch};
   struct vv_https_cancel token={confirmation_cancelled,&guard};char ack[128]={0};
   int setting_rc=confirmation_cancelled(&guard)?-ECANCELED:
    vv_native_platform_local_setting(s,result.text,ack,sizeof(ack),&token);
   if(cancelled(s)||epoch!=vv_native_mode_epoch(s->config.mode))setting_rc=-ECANCELED;
   if(setting_rc){
    /* A committed setting is never rolled back/replayed because its spoken
     * acknowledgement failed. PHOTO retains its own prompt/capture owner. */
    rc=setting_rc<0?setting_rc:0;
    if(setting_rc>0&&!s->pending&&ack[0]&&
       (s->config.ops.play||vv_native_client_streaming_tts(s->config.client))){
#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION
     int ack_rc=0;
     /* A committed setting survives an ACK network error; failure to retire
      * the old audio owner is a hard admission error, never suppressed. */
     ack_rc=vv_native_platform_prompt_admit(s,&token);if(ack_rc)rc=ack_rc;
     if(!ack_rc)ack_rc=report_prompt(s,ack,&token);
#else
     int ack_rc=report_prompt(s,ack,&token);
#endif
     printf("NATIVE_SETTING_ACK rc=%d\n",ack_rc);fflush(stdout);
     if(cancelled(s)||epoch!=vv_native_mode_epoch(s->config.mode))rc=-ECANCELED;
    }
    printf("NATIVE_SETTING committed=%u rejected=%u rc=%d\n",setting_rc==1,setting_rc==2,rc);fflush(stdout);
    goto completed_result;
   }
  }
  bool speech_wake=false;
#if defined(K7_REWAKE_PROMPT) && K7_REWAKE_PROMPT
  if(!rc)speech_wake=voice_only_wake(s,*intent);
#endif
  if(!rc){
   rc=vv_native_mode_apply(s->config.mode,*intent,epoch);
   if(rc==VV_NATIVE_TRACK_ACK_TIMEOUT){
    /* Mode's failed request already advanced exactly one epoch and held the
     * pipeline. A concurrent invalidation/cancel always outranks continuation. */
    if(*intent!=K7_VOICE_TRACK||cancelled(s)||
       vv_native_mode_epoch(s->config.mode)!=epoch+1u)rc=-ECANCELED;
    else {printf("NATIVE_COMMAND intent=1 command_timeout=1 rc=%d completed=0 hold_requested=1\n",-ETIMEDOUT);fflush(stdout);}
   }
  }
  if(!rc&&*intent==K7_VOICE_PHOTO){s->pending=true;s->previous_photo=before;s->pending_epoch=epoch;s->photo_prompt=(struct k7_photo_prompt_state){0};s->photo_saved=(struct k7_photo_prompt_state){0};}
  if(*intent==K7_VOICE_STOP)s->pending=false;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  /* Physical STOP already took priority. Retire conversation/history even if
   * its confirmation audio later fails or the network is lost. */
  if(s->config.dialogue&&*intent==K7_VOICE_STOP)(void)vv_dialogue_sleep(s->config.dialogue);
  if(s->config.dialogue&&*intent==K7_VOICE_QUIET)vv_dialogue_interrupt(s->config.dialogue);
#endif
  /* STOP advances the mode epoch itself. Do not sample a newer epoch here:
   * that could accidentally accept a concurrent external invalidation. */
  if(!rc){
   stage=7;
   rc=confirm(s,*intent,epoch+(*intent==K7_VOICE_STOP?1u:0u),speech_wake);
   if(!rc&&*intent==K7_VOICE_PHOTO){rc=photo_progress(s);if(rc==-EAGAIN)rc=0;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
   if(!rc&&s->config.dialogue){
    struct vv_dialogue *d=s->config.dialogue;
    if(*intent==K7_VOICE_TRACK)(void)vv_dialogue_wake(d);
    else if(*intent==K7_VOICE_NONE&&!s->pending&&result.text[0]){
     struct confirmation guard={s,epoch};
     struct vv_https_cancel token={confirmation_cancelled,&guard};
     int chat_rc;
     if(s->config.scoped_reply){
      chat_rc=(!result.command_finalized||!result.capture_identity.valid)?-EPROTO:
        s->config.scoped_reply(s->config.scoped_arg,&result,&token);
     }else chat_rc=vv_dialogue_reply(d,result.text,&token);
     printf("NATIVE_DIALOGUE rc=%d replies=%u interrupted=%u\n",chat_rc,d->replies,d->interruptions);
     /* A failed answer or reply-only interruption must not stop tracking or
      * retire the listening service. Actual global/mode cancellation retains
      * its existing stop semantics. No request is automatically replayed. */
     /* Network readiness cancels only this answer. An explicit stop or a
      * different mode epoch still invalidates the runtime operation. */
     if(cancelled(s)||guard.epoch!=vv_native_mode_epoch(s->config.mode))rc=-ECANCELED;
    }
   }
#endif
   /* Service treats TRACK/EBUSY as benign duplicate admission. A TTS/audio
    * EBUSY is a real failure, so do not let it enter that suppression path. */
   if(rc)*intent=K7_VOICE_NONE;
  }
 }
completed_result:
 vv_https_secure_zero(&result,sizeof(result));
 if(rc==-ECANCELED||cancelled(s)){s->listen_retryable=false;s->listen_owner_failed=false;hold(s);}
 done:printf("NATIVE_LISTEN stage=%d rc=%d intent=%d\n",stage,rc,(int)*intent);fflush(stdout);leave(s);return rc;
}
struct report_guard {struct vv_native_runtime *s;unsigned epoch,revision;uint32_t photo;atomic_bool reported;
#if K7_REAL_SHUIGUANG_PROFILE
 bool local_failure,network_abort,waiting,wait_prompted;
#endif
};
static int report_prompt(struct vv_native_runtime *s,const char *text,const struct vv_https_cancel *cancel){
 int rc=0;size_t bytes=0;
 if(cancel->is_cancelled(cancel->arg))return -ECANCELED;
#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION
 rc=vv_native_platform_prompt_admit(s,cancel);if(rc)return rc;
#endif
 if(vv_native_client_streaming_tts(s->config.client))rc=vv_native_client_speak(s->config.client,text,cancel);
 else{
  rc=vv_native_client_tts(s->config.client,text,s->config.pcm,s->config.pcm_capacity,&bytes,cancel);
  if(!rc&&cancel->is_cancelled(cancel->arg))rc=-ECANCELED;
  if(!rc&&(!bytes||bytes>s->config.pcm_capacity||bytes%2))rc=-EPROTO;
  if(!rc)rc=s->config.ops.play(s->config.ops.arg,s->config.pcm,bytes,cancel);
 }
 if(!rc&&cancel->is_cancelled(cancel->arg))rc=-ECANCELED;
 vv_https_secure_zero(s->config.pcm,s->config.pcm_capacity);return rc;
}
#if K7_REAL_SHUIGUANG_PROFILE
/* Main-thread immutable snapshot. Never reuse report_guard / assessment guard:
 * those mutable cancellation clocks belong exclusively to the POST worker. */
struct report_final_cues {
 struct vv_native_runtime *s;unsigned epoch,revision;uint32_t photo;
 bool *network_cancelled;
};
struct report_final_cue_guard {
 struct report_final_cues identity;
 const struct vv_https_cancel *main_cancel;
 uint64_t began,last;bool clock_seen,network_lost;int clock_error;
};
static bool report_final_cues_cancelled(void *arg){
 struct report_final_cue_guard *g=arg;const struct report_final_cues *identity=&g->identity;
 struct vv_native_runtime *s=identity->s;
 if(g->clock_error)return true;
 uint64_t now=s->config.mode->ops.now_ms(s->config.mode->ops.arg);
 if(!now||now==UINT64_MAX||(g->clock_seen&&now<g->last)){
  g->clock_error=VV_BACKEND_CLIENT_TIME;return true;
 }
 if(!g->clock_seen){g->began=now;g->clock_seen=true;}
 else if(now-g->began>=90000){g->clock_error=-ETIMEDOUT;return true;}
 g->last=now;
 if(g->main_cancel&&g->main_cancel->is_cancelled&&
    g->main_cancel->is_cancelled(g->main_cancel->arg))return true;
 if(cancelled(s)||vv_native_mode_epoch(s->config.mode)!=identity->epoch)return true;
 if(g->network_lost)return true;
 if(!s->config.ops.ready(s->config.ops.arg)){
  if(vv_native_platform_local_failure_ready(s))g->network_lost=true;
  return true;
 }
 uint32_t photo=0;unsigned mask=0,revision=0;
 return s->config.ops.photo_status(s->config.ops.arg,&photo,&mask)||
  photo!=identity->photo||mask!=7||s->config.ops.hold_revision(s->config.ops.arg,&revision)||
  revision!=identity->revision;
}
static int report_final_cues_result(struct report_final_cue_guard *g,int rc){
 if(g->clock_error)return g->clock_error;
 if(g->identity.network_cancelled)
  *g->identity.network_cancelled=(rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&g->network_lost;
 return rc;
}
static int report_final_cues_started(void *arg,const struct vv_https_cancel *main_cancel){
 struct report_final_cue_guard guard={.identity=*(const struct report_final_cues *)arg,.main_cancel=main_cancel};
 const struct report_final_cues *snapshot=&guard.identity;
 struct vv_native_runtime *s=snapshot->s;
 if(snapshot->network_cancelled)*snapshot->network_cancelled=false;
 const struct vv_https_cancel token={report_final_cues_cancelled,&guard};
 if(report_final_cues_cancelled(&guard))return report_final_cues_result(&guard,-ECANCELED);
 if((s->photo_prompt.epoch&&s->photo_prompt.epoch!=snapshot->photo)||
    (s->photo_saved.epoch&&s->photo_saved.epoch!=snapshot->photo))return -ECANCELED;
 const char *keys[4];int count=k7_photo_prompt_plan(&s->photo_prompt,snapshot->photo,7,keys);
 if(count<0)return count;
 for(int i=0;i<count;i++){
  unsigned saved=!strcmp(keys[i],"front_saved")?1u:!strcmp(keys[i],"left_saved")?2u:!strcmp(keys[i],"right_saved")?4u:0u;
  if(saved&&s->photo_saved.epoch==snapshot->photo&&(s->photo_saved.done&saved))continue;
  const char *text=photo_text(keys[i]);if(!text)return -EINVAL;
  if(report_final_cues_cancelled(&guard))return report_final_cues_result(&guard,-ECANCELED);
  /* Use the independent main-only guard's already sampled monotonic clock.
   * Telemetry adds no clock read and is not the physical audio-start event. */
  uint64_t begin=guard.last;
  printf("REAL_CAPTURE_CUE phase=call_begin key=%s photo=%lu epoch=%u t_ms=%llu valid=%u\n",
   keys[i],(unsigned long)snapshot->photo,snapshot->epoch,(unsigned long long)begin,(unsigned)(begin&&begin!=UINT64_MAX));fflush(stdout);
  int rc=report_prompt(s,text,&token);
  bool cancelled_now=report_final_cues_cancelled(&guard);
  if(!rc&&cancelled_now)rc=-ECANCELED;
  rc=report_final_cues_result(&guard,rc);
  uint64_t end=guard.last;
  bool valid=!guard.clock_error&&begin&&end&&begin!=UINT64_MAX&&end!=UINT64_MAX&&end>=begin;
  printf("REAL_CAPTURE_CUE phase=call_end key=%s photo=%lu epoch=%u t_ms=%llu valid=%u elapsed_ms=%llu rc=%d\n",
   keys[i],(unsigned long)snapshot->photo,snapshot->epoch,(unsigned long long)end,(unsigned)valid,
   (unsigned long long)(valid?end-begin:0),rc);fflush(stdout);
  if(rc)return rc;
  if(saved){s->photo_saved.epoch=snapshot->photo;s->photo_saved.done|=saved;}
 }
 s->photo_prompt=(struct k7_photo_prompt_state){snapshot->photo,7};return 0;
}
static int report_upload_task(void *arg,const struct vv_assessment_request *q,
 char accepted[37],const struct vv_https_cancel *cancel){
 const struct report_guard *g=arg;
 /* Copy only immutable identities before worker creation; no worker-mutated
  * report_guard field is read by the main completion-cue callback. */
 struct report_final_cues cues={.s=g->s,.epoch=g->epoch,.revision=g->revision,.photo=g->photo};
 return vv_native_platform_upload_task_progress(g->s,q,accepted,cancel,
  report_final_cues_started,&cues);
}
#endif

#if K7_REAL_SHUIGUANG_PROFILE
/* Only the already validated real report can use this streaming entry.
 * Prompt admission and the original report cancellation/HOLD guard remain. */
static int report_stream_prompt(struct vv_native_runtime *s,const char *text,
 const struct vv_https_cancel *cancel,size_t *attempted){
 *attempted=0;
 if(cancel->is_cancelled(cancel->arg))return -ECANCELED;
 int rc=0;
#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION
 rc=vv_native_platform_prompt_admit(s,cancel);if(rc)return rc;
#endif
 rc=vv_native_client_speak_report(s->config.client,text,cancel,attempted);
 if(!rc&&cancel->is_cancelled(cancel->arg))rc=-ECANCELED;
 vv_https_secure_zero(s->config.pcm,s->config.pcm_capacity);return rc;
}
#endif
#if K7_REAL_SHUIGUANG_PROFILE
/* Assessment calls this once, after HTTP202/task-id validation, under the
 * same total-deadline, STOP, mode epoch, photo epoch and applied HOLD guard. */
static int report_upload_accepted(void *arg,const struct vv_https_cancel *cancel){
 int rc=report_prompt(arg,"正在生成报告，请稍候。",cancel);
 printf("REAL_REPORT_PROGRESS upload_accepted=1 prompt_rc=%d\n",rc);fflush(stdout);
 return rc;
}
#endif
#if defined(K7_READY_CUE) && K7_READY_CUE
/* Serial service owner, after model/input startup, before any ASR admission.
 * Use the ordinary prompt owner and cancellation path; no motion or wake. */
int vv_native_runtime_ready_cue(struct vv_native_runtime *s){
 if(!s)return -EINVAL;
 if(cancelled(s))return -ECANCELED;
 if(!s->config.ops.ready(s->config.ops.arg))return -ENETDOWN;
 struct confirmation guard={s,vv_native_mode_epoch(s->config.mode)};
 struct vv_https_cancel token={confirmation_cancelled,&guard};
 int rc=report_prompt(s,"可以对话了。",&token);
 printf("VOICE_READY cue_result=%d local_prompt=1\n",rc);fflush(stdout);
 return rc;
}
#endif
static bool report_cancelled(void *arg){
 struct report_guard *g=arg;struct vv_native_runtime *s=g->s;
 uint32_t photo=0;unsigned mask=0,revision=0,reason=0;int photo_rc=0,hold_rc=0;
 /* Preserve short-circuit order; report only the first failing predicate. */
 if(cancelled(s))reason=1;
 else if(g->epoch!=vv_native_mode_epoch(s->config.mode))reason=2;
 else if(
#if K7_REAL_SHUIGUANG_PROFILE
         (g->local_failure||g->waiting)?!vv_native_platform_local_failure_ready(s):
#endif
         !s->config.ops.ready(s->config.ops.arg)){
  reason=4;
#if K7_REAL_SHUIGUANG_PROFILE
  if(!g->local_failure&&vv_native_platform_local_failure_ready(s))g->network_abort=true;
#endif
 }
 else if((photo_rc=s->config.ops.photo_status(s->config.ops.arg,&photo,&mask)))reason=8;
 else if(photo!=g->photo)reason=16;
 else if(mask!=7)reason=32;
 else if((hold_rc=s->config.ops.hold_revision(s->config.ops.arg,&revision)))reason=64;
 else if(revision!=g->revision)reason=128;
 if(reason&&!atomic_exchange(&g->reported,true)){
  printf("REPORT_CANCEL reason=%u photo_rc=%d hold_rc=%d mask=%u\n",reason,photo_rc,hold_rc,mask);fflush(stdout);
 }
 return reason!=0;
}

#if K7_REAL_SHUIGUANG_PROFILE
/* Only the read-only wait/retry phase may survive a transient link loss.
 * TLS trust, explicit STOP, mode/photo epochs, mask and HOLD remain mandatory.
 * Upload still uses the original link gate. No admission is replayed. */
static int report_wait_started(void *arg,const struct vv_https_cancel *cancel){
 struct report_guard *g=arg;g->wait_prompted=true;
 return report_upload_accepted(g->s,cancel);
}
static int report_wait_task(void *arg,const char *id,struct vv_backend_task *task,
 struct vv_backend_brief *brief,const struct vv_https_cancel *cancel){
 struct report_guard *g=arg;g->waiting=true;
 bool first=!g->wait_prompted;
 return vv_native_platform_wait_task_progress(g->s,id,task,brief,cancel,
   first?report_wait_started:NULL,g);
}
static bool network_abort_error(int rc){
 return !rc||rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED||rc==VV_HTTPS_EIO||rc==VV_HTTPS_ETIMEOUT;
}
static bool arm_local_failure(struct report_guard *g){
 g->local_failure=true;
 if(!report_cancelled(g))return true;
 g->local_failure=false;return false;
}
/* Photos have been discarded and resume changes the hold revision. The
 * original mode-status ACK still verifies the new applied physical revision. */
static bool report_resume_cancelled(struct report_guard *g){
 struct vv_native_runtime *s=g->s;
 return cancelled(s)||g->epoch!=vv_native_mode_epoch(s->config.mode)||
   ((g->local_failure||g->waiting)?!vv_native_platform_local_failure_ready(s):!s->config.ops.ready(s->config.ops.arg));
}
static const char *network_failure_notice(const struct vv_assessment_result *out){
 if(out->outcome==VV_ASSESSMENT_READY)return "播报失败，请重试。";
 return out->task_id[0]?"分析失败，请重拍。":"上传失败，请重试。";
}
#endif

#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE && !K7_LOCAL_ASSESSMENT && !K7_REAL_SHUIGUANG_PROFILE
#define CLOUD_REPORT_CAP 6144u
static int cloud_report_speech(char *text,size_t bytes) {
 static const char fixture[]="Mock 测试报告。三张照片已接收。这是用于接口联调的模拟报告，不包含真实皮肤分析结果。";
 static const char neutral[]="正面、左侧和右侧照片已接收，三视图记录已整理完成。";
 static const char prefix[]="报告结果。";
 static const char follow[]="您可以在对应任务中查看本次上传记录，并在相同光线和角度下留存后续照片，方便比较日常变化。平时可保持温和清洁，按自身需要做好保湿和防晒，避免频繁更换护理产品。再次采集时，请保持光线均匀、面部无遮挡，并按照提示转头。播报结束后，云台将恢复人脸跟随。";
 if(!text || !bytes || bytes>=4612 || strlen(text)!=bytes)return -EPROTO;
 if(!strcmp(text,fixture)){strcpy(text,neutral);bytes=strlen(text);}
 /* Reject malformed UTF-8 or text outside the existing provider limit. */
 size_t i=0,characters=0;
 while(i<bytes){
  unsigned v=(unsigned char)text[i++],more=0,min=0;
  if(v<128){if(v<32 || v==127)return -EPROTO;}
  else if(v>=0xc2&&v<=0xdf){more=1;min=0x80;v&=31;}
  else if(v>=0xe0&&v<=0xef){more=2;min=0x800;v&=15;}
  else if(v>=0xf0&&v<=0xf4){more=3;min=0x10000;v&=7;}
  else return -EPROTO;
  if(bytes-i<more)return -EPROTO;
  while(more--){unsigned b=(unsigned char)text[i++];if((b&0xc0)!=0x80)return -EPROTO;v=(v<<6)|(b&63);}
  if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return -EPROTO;
  ++characters;
 }
 size_t extra=characters<140?sizeof(follow)-1:0;
 if(characters>850 || bytes+sizeof(prefix)-1+extra>4000)return -EOVERFLOW;
 memmove(text+sizeof(prefix)-1,text,bytes+1);memcpy(text,prefix,sizeof(prefix)-1);
 if(extra)memcpy(text+sizeof(prefix)-1+bytes,follow,extra+1);
 return 0;
}
#endif
int vv_native_runtime_report(struct vv_native_runtime *s,const struct vv_assessment_request *q,struct vv_assessment_result *out,struct vv_report_audio_result *audio){
 if(!s||!q||!out||!audio)return -EINVAL;
 int rc=enter(s);if(rc)return rc;
 if(!s->pending){rc=-ENOENT;goto done;}
 if(cancelled(s)||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)){hold(s);rc=-ECANCELED;goto done;}
 if(!s->config.ops.ready(s->config.ops.arg)
#if K7_REAL_SHUIGUANG_PROFILE
    &&!vv_native_platform_local_failure_ready(s)
#endif
   ){rc=-ENETDOWN;goto done;}
 if(!K7_LOCAL_ASSESSMENT&&s->config.assessment_ready&&!s->config.assessment_ready(s->config.ops.arg)){rc=-EACCES;goto done;}
 uint32_t photo=0,current=0;unsigned mask=0,revision=0;
 rc=s->config.ops.photo_status(s->config.ops.arg,&photo,&mask);if(rc)goto done;
 if(!photo||photo==s->previous_photo||mask!=7){rc=-EAGAIN;goto done;}
 rc=s->config.ops.hold_revision(s->config.ops.arg,&revision);if(rc)goto done;
 unsigned char *images=malloc(3u*JPEG_CAP);if(!images){rc=-ENOMEM;goto done;}
 struct vv_assessment_request request=*q;
 for(unsigned i=0;i<3&&!rc;i++){
  size_t n=0;rc=s->config.ops.photo_copy(s->config.ops.arg,photo,1u<<i,images+i*JPEG_CAP,JPEG_CAP,&n);
  if(!rc&&(!n||n>JPEG_CAP))rc=-EIO;
  request.images[i]=(struct k7_upload_image){images+i*JPEG_CAP,n};
 }
 if(!rc)rc=s->config.ops.photo_status(s->config.ops.arg,&current,&mask);
 if(!rc&&(current!=photo||mask!=7||cancelled(s)||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)))rc=-ECANCELED;
 if(!rc){unsigned held=0;rc=s->config.ops.hold_revision(s->config.ops.arg,&held);if(!rc&&held!=revision)rc=-ECANCELED;}
 /* Consume the one-shot admission before any upload; failures never replay. */
 s->pending=false;struct report_guard guard={.s=s,.epoch=s->pending_epoch,.revision=revision,.photo=photo};atomic_init(&guard.reported,false);
 struct vv_https_cancel cancel={report_cancelled,&guard};
#if K7_REAL_SHUIGUANG_PROFILE
 char *real_text=NULL;size_t real_bytes=0;const char *real_notice=NULL;int speech_failure=0;bool generic_advice_played=false;
 if(!rc){real_text=calloc(1,513);if(!real_text)rc=-ENOMEM;}
 if(!rc){
  memset(out,0,sizeof(*out));memset(audio,0,sizeof(*audio));
  if(report_network_only(s)){
   guard.network_abort=true;out->outcome=VV_ASSESSMENT_BACKEND_ERROR;out->backend_error=VV_HTTPS_EIO;
   real_notice="上传失败，请重试。";
  }else if(s->config.assessment_ready&&!s->config.assessment_ready(s->config.ops.arg)){
   /* Auth may expire after deferral. Preserve the old truthful capture cues
    * synchronously before existing generic care; no POST or false acceptance. */
   bool cue_network_cancelled=false;
   struct report_final_cues cues={.s=s,.epoch=guard.epoch,.revision=guard.revision,.photo=guard.photo,.network_cancelled=&cue_network_cancelled};
   rc=report_final_cues_started(&cues,NULL);
   if((rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&cue_network_cancelled&&
      report_network_only(s)&&arm_local_failure(&guard)){
    rc=0;printf("REAL_CAPTURE_CUE auth_fallback_network=1 generic_only=1\n");fflush(stdout);
   }
   out->outcome=VV_ASSESSMENT_BACKEND_ERROR;out->backend_error=VV_BACKEND_CLIENT_AUTH;
   if(!rc)real_notice="上传失败，请重试。";
  }else{
   out->outcome=vv_native_assessment_prepare_wait_upload_progress(s->config.assessment,&request,real_text,513,&real_bytes,&cancel,out,NULL,NULL,report_wait_task,&guard,report_upload_task,&guard);
   guard.waiting=false;
   if(out->outcome==VV_ASSESSMENT_READY){
    if(!vv_real_report_text(real_text,513,true)||strlen(real_text)!=real_bytes)rc=-EPROTO;
   }else if(out->outcome==VV_ASSESSMENT_CANCELLED){
    /* Only an authenticated, finalized voice STOP in this exact wait is a
     * recoverable report cancellation. Never clear global/external cancel. */
    if(out->backend_error==VV_REAL_WAIT_VOICE_STOP&&!cancelled(s)&&
       vv_native_mode_epoch(s->config.mode)==guard.epoch+1u&&!s->pending&&
       !s->config.mode->assessment_active&&
       !s->config.mode->ops.status(s->config.mode->ops.arg,0))rc=VV_REAL_WAIT_VOICE_STOP;
    else if(guard.network_abort&&network_abort_error(out->backend_error)&&arm_local_failure(&guard)){
     out->outcome=VV_ASSESSMENT_BACKEND_ERROR;out->backend_error=VV_HTTPS_EIO;
     real_notice=network_failure_notice(out);
    }else rc=-ECANCELED;
   }
   else if(out->outcome==VV_ASSESSMENT_MEMORY)rc=-ENOMEM;
   else if(out->outcome==VV_ASSESSMENT_TIMEOUT||out->outcome==VV_ASSESSMENT_POLL_LIMIT){
    /* A deadline can coincide with a callback error. Never conceal a TLS,
     * clock, memory, protocol or audio fault behind a soft timeout outcome. */
    int e=out->backend_error;
    if(e&&!vv_local_business_unavailable(e)&&e!=VV_HTTPS_ECANCELLED&&e!=-ECANCELED)rc=e;
    else real_notice="分析超时，请重试。";
   }
   else if(out->outcome==VV_ASSESSMENT_FAILED)real_notice="分析失败，请重拍。";
   else if(out->outcome==VV_ASSESSMENT_NEEDS_RETAKE)real_notice="分析失败，请重拍。";
   else if(out->outcome==VV_ASSESSMENT_BACKEND_ERROR&&out->backend_error==VV_BACKEND_CLIENT_REPORT_MISSING)
    real_notice="报告缺测，请重拍。";
   else if(out->outcome==VV_ASSESSMENT_BACKEND_ERROR&&vv_local_business_unavailable(out->backend_error))
    real_notice=out->backend_error==VV_BACKEND_CLIENT_AUTH?"上传失败，请重试。":"上传失败，请重试。";
   else rc=out->backend_error?out->backend_error:-EPROTO;
  }
  printf("REAL_REPORT outcome=%u upload_attempted=%u backend_rc=%d task=%.36s report=%.36s checks=%u failure_code=%.128s\n",(unsigned)out->outcome,(unsigned)out->upload_attempted,out->backend_error,out->task_id,out->report_id,out->task_checks,out->failure_code);fflush(stdout);
 }
#elif K7_LOCAL_ASSESSMENT
 if(!rc){
  memset(out,0,sizeof(*out));memset(audio,0,sizeof(*audio));
  if(s->config.assessment_ready&&!s->config.assessment_ready(s->config.ops.arg)){
   out->outcome=VV_ASSESSMENT_BACKEND_ERROR;
   out->backend_error=VV_BACKEND_CLIENT_AUTH;
   printf("NATIVE_LOCAL_CARE upload_skipped=1 reason=business_auth\n");
  }else{
   request.total_timeout_ms=VV_LOCAL_UPLOAD_BUDGET_MS;
   out->outcome=vv_native_assessment_upload_only(s->config.assessment,&request,&cancel,out);
   rc=vv_local_upload_result(out);
   printf("NATIVE_LOCAL_CARE upload_attempted=%u accepted=%u result_unconfirmed=%u outcome=%u backend_rc=%d\n",
    (unsigned)out->upload_attempted,(unsigned)(out->outcome==VV_ASSESSMENT_UPLOADED),
    (unsigned)(out->upload_attempted&&out->outcome!=VV_ASSESSMENT_UPLOADED),
    (unsigned)out->outcome,out->backend_error);
  }
  fflush(stdout);
 }
#elif defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
 char *report_text=NULL;size_t report_bytes=0;
 if(!rc){report_text=calloc(1,CLOUD_REPORT_CAP);if(!report_text)rc=-ENOMEM;}
 if(!rc){memset(out,0,sizeof(*out));memset(audio,0,sizeof(*audio));
  enum vv_assessment_outcome result=vv_native_assessment_prepare(s->config.assessment,&request,report_text,4612,&report_bytes,&cancel,out);
  out->outcome=result;
  if(result==VV_ASSESSMENT_CANCELLED)rc=-ECANCELED;
  else if(result==VV_ASSESSMENT_TIMEOUT || result==VV_ASSESSMENT_POLL_LIMIT)rc=-ETIMEDOUT;
  else if(result==VV_ASSESSMENT_MEMORY)rc=-ENOMEM;
  else if(result!=VV_ASSESSMENT_READY)rc=out->backend_error?out->backend_error:-EPROTO;
  else rc=cloud_report_speech(report_text,report_bytes);
 }
#else
 if(!rc){memset(out,0,sizeof(*out));memset(audio,0,sizeof(*audio));
  enum vv_assessment_outcome result=vv_native_assessment_upload_only(s->config.assessment,&request,&cancel,out);
  out->outcome=result;if(result!=VV_ASSESSMENT_UPLOADED)rc=out->backend_error?out->backend_error:-EIO;}
#endif
 vv_https_secure_zero(images,3u*JPEG_CAP);free(images);
#if K7_REAL_SHUIGUANG_PROFILE
 if(!rc&&real_notice&&!arm_local_failure(&guard))rc=-ECANCELED;
 if(!rc&&!real_notice&&report_cancelled(&guard)){
  if(guard.network_abort&&arm_local_failure(&guard)){
   real_notice=network_failure_notice(out);
   if(out->outcome==VV_ASSESSMENT_READY)speech_failure=-ENETDOWN;
   else {out->outcome=VV_ASSESSMENT_BACKEND_ERROR;out->backend_error=VV_HTTPS_EIO;}
  }else rc=-ECANCELED;
 }
#endif
 if(!rc&&report_cancelled(&guard))rc=-ECANCELED;
 if(!rc){
  if(!s->config.ops.play&&!vv_native_client_streaming_tts(s->config.client))rc=-ENOTSUP;
#if K7_REAL_SHUIGUANG_PROFILE
  else {
   if(!real_notice){
    if(!real_text||!*real_text)rc=-EPROTO;
    else{
     printf("REAL_REPORT_SPEECH ready=1 notice=0 text=%s\n",real_text);fflush(stdout);
     size_t speech_attempted=0;
     rc=report_stream_prompt(s,real_text,&cancel,&speech_attempted);
     /* Only explicit transport/business availability failures or a proven
      * network cancellation may select local care. Bare audio -EIO, trust,
      * clock, memory, protocol, STOP and physical ownership stay errors. */
     if(rc&&!speech_attempted&&(vv_local_business_unavailable(rc)||(guard.network_abort&&network_abort_error(rc)))&&arm_local_failure(&guard)){
      speech_failure=rc;real_notice=network_failure_notice(out);rc=0;
     }
     if(!rc&&!real_notice){audio->segments_synthesized=1;audio->segments_played=1;}
    }
   }
   if(!rc&&real_notice){
    printf("REAL_REPORT_GENERIC ready=%u backend_rc=%d speech_rc=%d text=%s\n",(unsigned)(out->outcome==VV_ASSESSMENT_READY),out->backend_error,speech_failure,VV_LOCAL_REPORT_TEXT);fflush(stdout);
    rc=report_prompt(s,VV_LOCAL_REPORT_TEXT,&cancel);
    generic_advice_played=!rc;
   }
  }
#elif K7_LOCAL_ASSESSMENT || (defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE)
  else { rc=report_prompt(s,VV_LOCAL_REPORT_TEXT,&cancel); if(!rc){audio->segments_synthesized=1;audio->segments_played=1;} }
#else
  else rc=report_prompt(s,"皮肤检测报告已完成。",&cancel);
#endif
#if K7_REAL_SHUIGUANG_PROFILE
  audio->callback_error=speech_failure?speech_failure:rc;
#else
  audio->callback_error=rc;
#endif
#if K7_REAL_SHUIGUANG_PROFILE
  printf("REAL_REPORT_COMPLETE prompt_rc=%d ready=%u notice=%u summary_played=%u generic_advice_played=%u speech_rc=%d\n",rc,(unsigned)(out->outcome==VV_ASSESSMENT_READY),(unsigned)(real_notice!=NULL),(unsigned)(!rc&&!real_notice&&audio->segments_played==1),(unsigned)generic_advice_played,speech_failure);fflush(stdout);
#elif K7_LOCAL_ASSESSMENT
  printf("NATIVE_LOCAL_CARE prompt_rc=%d generic_advice=1 report_ready=0\n",rc);fflush(stdout);
#elif defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
  printf("NATIVE_REPORT_COMPLETE prompt_rc=%d ready=%u checks=%u\n",rc,(unsigned)(out->outcome==VV_ASSESSMENT_READY),out->task_checks);fflush(stdout);
#else
  printf("NATIVE_UPLOAD_COMPLETE prompt_rc=%d uploaded=%u\n",rc,(unsigned)out->upload_attempted);fflush(stdout);
#endif
 }
#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE && !K7_LOCAL_ASSESSMENT && !K7_REAL_SHUIGUANG_PROFILE
 if(report_text){vv_https_secure_zero(report_text,CLOUD_REPORT_CAP);free(report_text);}
#endif
#if K7_REAL_SHUIGUANG_PROFILE
 if(real_text){vv_https_secure_zero(real_text,513);free(real_text);}
#endif
 if(!rc&&report_cancelled(&guard))rc=-ECANCELED;
#if K7_LOCAL_ASSESSMENT
 /* The final photo guard above must run before discard changes its mask.
  * All following cancellation checks use owner epoch / applied revision. */
 if(!rc){rc=vv_native_runtime_k7_discard(photo);
  printf("NATIVE_LOCAL_CARE discard_rc=%d photo=%lu\n",rc,(unsigned long)photo);fflush(stdout);}
 if(!rc&&(cancelled(s)||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)))rc=-ECANCELED;
#endif
 if(!rc)rc=vv_native_mode_assessment_done(s->config.mode,s->pending_epoch);
 if(!rc)rc=s->config.ops.resume(s->config.ops.arg,revision);
 /* A queued resume is not yet tracking. Await the worker acknowledgement;
  * never replay the assessment if this transition fails or is interrupted. */
 if(!rc){
  struct vv_native_mode_ops *o=&s->config.mode->ops;
#if !K7_REAL_SHUIGUANG_PROFILE
  struct confirmation p={s,guard.epoch};
#endif
  uint64_t began=o->now_ms(o->arg);
  for(unsigned i=0;i<=300;i++){
   if(
#if K7_REAL_SHUIGUANG_PROFILE
      report_resume_cancelled(&guard)
#else
      confirmation_cancelled(&p)
#endif
     ){rc=-ECANCELED;break;}
   uint64_t now=o->now_ms(o->arg);
   if(now<began||now-began>=3000){rc=-ETIMEDOUT;break;}
   rc=o->status(o->arg,1);if(rc!=-EAGAIN)break;
   if(i==300){rc=-ETIMEDOUT;break;}o->sleep_ms(o->arg,10);
  }
  uint64_t finished=o->now_ms(o->arg);
  if(finished<began||finished-began>=3000)rc=-ETIMEDOUT;
  if(
#if K7_REAL_SHUIGUANG_PROFILE
      report_resume_cancelled(&guard)
#else
      confirmation_cancelled(&p)
#endif
     )rc=-ECANCELED;
 }
 if(!rc&&(cancelled(s)||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)))rc=-ECANCELED;
#if defined(K7_ADDRESSED_WINDOW) && K7_ADDRESSED_WINDOW && defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE)
 /* Successful report/resume gives a fresh follow-up window. Never wake a
  * stopped conversation, and never change the physical resume result. */
 if(!rc&&s->config.dialogue&&s->config.dialogue->awake)
  (void)vv_dialogue_wake(s->config.dialogue);
#endif
#if K7_REAL_SHUIGUANG_PROFILE
 if(rc==VV_REAL_WAIT_VOICE_STOP){
  /* The exact voice STOP is already physically acknowledged. Avoid a second
   * halt/revision which would invalidate its ACK and the next wake admission. */
  unsigned expected=guard.epoch+1u;
  bool safe=!cancelled(s)&&vv_native_mode_epoch(s->config.mode)==expected&&
    !s->pending&&!s->config.mode->assessment_active&&
    vv_native_platform_local_failure_ready(s);
  int held=safe?s->config.mode->ops.status(s->config.mode->ops.arg,0):-ECANCELED;
  safe=safe&&!held&&!cancelled(s)&&vv_native_mode_epoch(s->config.mode)==expected&&
    !s->pending&&!s->config.mode->assessment_active&&
    vv_native_platform_local_failure_ready(s);
  if(!safe){rc=held?held:-ECANCELED;hold(s);}
 }else
#endif
 if(rc)hold(s);
 done:leave(s);return rc;
}

#if K7_REAL_SHUIGUANG_PROFILE
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW) && defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE)
/* STOP already changed the epoch. Its ACK must not use the old wait/yield
 * token (which intentionally cancels), and never calls the HTTP guard. */
static int report_voice_stop_ack(struct vv_native_runtime *s,unsigned expected){
 struct vv_native_mode_ops *o=&s->config.mode->ops;
 uint64_t began=o->now_ms(o->arg),last=began;
 if(!began||began==UINT64_MAX)return -ETIMEDOUT;
 for(unsigned i=0;i<=300;i++){
  if(cancelled(s)||vv_native_mode_epoch(s->config.mode)!=expected)return -ECANCELED;
  if(!vv_native_platform_local_failure_ready(s))return -EACCES;
  uint64_t now=o->now_ms(o->arg);
  if(now==UINT64_MAX||now<last||now-began>=3000)return -ETIMEDOUT;
  last=now;int rc=o->status(o->arg,0);
  uint64_t after=o->now_ms(o->arg);
  if(cancelled(s)||vv_native_mode_epoch(s->config.mode)!=expected)return -ECANCELED;
  if(!vv_native_platform_local_failure_ready(s))return -EACCES;
  if(after==UINT64_MAX||after<last||after-began>=3000)return -ETIMEDOUT;
  last=after;
  if(rc!=-EAGAIN)return rc;
  if(i==300)return -ETIMEDOUT;
  o->sleep_ms(o->arg,10);
 }
 return -ETIMEDOUT;
}
#endif
/* Called only by the existing report/service owner while runtime.busy remains
 * held. The business worker owns its own transport and cancellation guard.
 * This token is the separate ASR-yield token; never call the business token. */
int vv_native_runtime_wait_control(struct vv_native_runtime *s,unsigned samples,
 const struct vv_https_cancel *yield){
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW) && defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE)
 if(!s||!yield||!yield->is_cancelled||samples<320||
    samples>VV_ASR_LISTEN_SAMPLES_MAX||samples%320)return -EINVAL;
 if(!atomic_load(&s->busy))return -EBUSY;
 if(cancelled(s)||yield->is_cancelled(yield->arg))return -ECANCELED;
 if(!s->config.ops.ready(s->config.ops.arg))return -ENETDOWN;
 if(!vv_native_client_streaming(s->config.client))return -ENOTSUP;
 unsigned epoch=vv_native_mode_epoch(s->config.mode);
 if(s->config.scoped_listen_admit){
  int admission=s->config.scoped_listen_admit(s->config.scoped_arg);
  if(admission)return admission;
 }
 struct vv_asr_batch_result result={0};
 int rc=vv_native_client_listen(s->config.client,samples,yield,&result);
 if(rc)goto done;
 if(cancelled(s)||epoch!=vv_native_mode_epoch(s->config.mode)){rc=-ECANCELED;goto done;}
 if(!memchr(result.text,0,sizeof(result.text))||!result.command_finalized||
    result.command_intent>K7_VOICE_QUIET){rc=-EPROTO;goto done;}
 enum k7_voice_intent intent=(enum k7_voice_intent)result.command_intent;
 /* An already finalized STOP wins even if HTTP completed during ASR return.
  * It must never become an internal yield or a successfully spoken report. */
 if(intent==K7_VOICE_STOP){
  rc=vv_native_mode_apply(s->config.mode,K7_VOICE_STOP,epoch);
  s->pending=false;
  if(s->config.dialogue)(void)vv_dialogue_sleep(s->config.dialogue);
  if(!rc)rc=report_voice_stop_ack(s,epoch+1u);
  printf("REAL_WAIT_CONTROL stop=1 rc=%d\n",rc);fflush(stdout);
  if(!rc)rc=VV_REAL_WAIT_VOICE_STOP;
  goto done;
 }
 if(yield->is_cancelled(yield->arg)){rc=-ECANCELED;goto done;}
 if(!s->config.ops.ready(s->config.ops.arg)){rc=-ENETDOWN;goto done;}
 if(intent==K7_VOICE_NONE){
  struct vv_voice_setting setting;
  int parsed=vv_voice_setting_parse(result.text,&setting);
  /* The wait window is control-only: no ordinary reply, new capture, voice
   * switch or mode request. Preserve the original exact volume grammar. */
  if(parsed>0&&(setting.kind==VV_SETTING_VOLUME||setting.kind==VV_SETTING_VOLUME_RELATIVE)){
   char ack[128]={0};
   int applied=vv_native_platform_local_setting(s,result.text,ack,sizeof(ack),yield);
   vv_https_secure_zero(ack,sizeof(ack));
   rc=applied<0?applied:0;
   printf("REAL_WAIT_CONTROL volume_committed=%u rc=%d\n",(unsigned)(applied==1),rc);fflush(stdout);
  }
 }else if(intent==K7_VOICE_TRACK){
  /* A confirmed wake is acknowledged in dialogue state only. HOLD remains
   * applied; no TRACK request or physical resume is issued during analysis. */
  if(s->config.dialogue&&!s->config.dialogue->awake)rc=vv_dialogue_wake(s->config.dialogue);
  printf("REAL_WAIT_CONTROL wake=1 hold_retained=1 rc=%d\n",rc);fflush(stdout);
 }
done:
 vv_https_secure_zero(&result,sizeof(result));return rc;
#else
 (void)s;(void)samples;(void)yield;return -ENOTSUP;
#endif
}


#endif
