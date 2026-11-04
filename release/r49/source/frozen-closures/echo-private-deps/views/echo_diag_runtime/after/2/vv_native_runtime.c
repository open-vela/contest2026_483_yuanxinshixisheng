#include "vv_native_runtime.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_voice_prompts.inc"
#define JPEG_CAP (1024u*1024u)
static bool cancelled(void *arg){struct vv_native_runtime *s=arg;return atomic_load(&s->cancelled);}
static int enter(struct vv_native_runtime *s){bool no=false;return atomic_compare_exchange_strong(&s->busy,&no,true)?0:-EBUSY;}
static void leave(struct vv_native_runtime *s){atomic_store(&s->busy,false);}
static void hold(struct vv_native_runtime *s){s->pending=false;(void)vv_native_mode_apply(s->config.mode,K7_VOICE_STOP,vv_native_mode_epoch(s->config.mode));}
struct photo_confirmation {struct vv_native_runtime *s;uint32_t photo;unsigned done;bool advanced;};
static bool photo_cancelled(void *arg){
 struct photo_confirmation *p=arg;struct vv_native_runtime*s=p->s;
 if(cancelled(s)||!s->pending||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)||!s->config.ops.ready(s->config.ops.arg))return true;
 uint32_t epoch=0;unsigned done=0;
 if(s->config.ops.photo_status(s->config.ops.arg,&epoch,&done)||epoch!=p->photo)return true;
 if(done!=p->done){p->advanced=true;return true;}
 return false;
}
static const char *photo_text(const char *key){
 if(!strcmp(key,"capture_retained"))return "三视图拍摄完成，正在处理，请稍候。";
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
 const char *keys[4];int count=k7_photo_prompt_plan(&s->photo_prompt,epoch,done,keys);
 if(count<0){hold(s);return count;}
 struct photo_confirmation p={s,epoch,done,false};struct vv_https_cancel token={photo_cancelled,&p};
 for(int i=0;i<count;i++){
  unsigned saved=!strcmp(keys[i],"front_saved")?1u:!strcmp(keys[i],"left_saved")?2u:!strcmp(keys[i],"right_saved")?4u:0u;
  if(saved&&s->photo_saved.epoch==epoch&&(s->photo_saved.done&saved))continue;
  size_t bytes=0;const char *text=photo_text(keys[i]);
  if(!text){rc=-EINVAL;break;}
  if(photo_cancelled(&p)){rc=-ECANCELED;break;}
  if(vv_native_client_streaming_tts(s->config.client))rc=vv_native_client_speak(s->config.client,text,&token);
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
  if((rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&p.advanced&&!cancelled(s)&&s->pending_epoch==vv_native_mode_epoch(s->config.mode)){
   uint32_t fresh=0;unsigned mask=0;
   if(!s->config.ops.photo_status(s->config.ops.arg,&fresh,&mask)&&fresh==epoch&&s->config.ops.ready(s->config.ops.arg))return -EAGAIN;
  }
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
static int confirm(struct vv_native_runtime *s,enum k7_voice_intent intent,unsigned epoch){
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
 const char *text=intent==K7_VOICE_TRACK?"OpenVela 云台已启动。":"云台跟踪已停止。";
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
  uint32_t before=0;unsigned mask=0;
  if(*intent==K7_VOICE_PHOTO&&s->config.assessment_ready&&!s->config.assessment_ready(s->config.ops.arg))rc=-EACCES;
  if(!rc&&*intent==K7_VOICE_PHOTO)rc=s->config.ops.photo_status(s->config.ops.arg,&before,&mask);
  if(!rc&&s->config.command_admit){
   struct confirmation guard={s,epoch};
   struct vv_https_cancel token={confirmation_cancelled,&guard};
   rc=s->config.command_admit(s->config.command_admit_arg,*intent,epoch,&token);
   /* A concurrent stop/epoch change outranks even a harmless busy rejection. */
   if(cancelled(s)||epoch!=vv_native_mode_epoch(s->config.mode))rc=-ECANCELED;
  }
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
   rc=confirm(s,*intent,epoch+(*intent==K7_VOICE_STOP?1u:0u));
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
 vv_https_secure_zero(&result,sizeof(result));
 if(rc==-ECANCELED||cancelled(s)){s->listen_retryable=false;s->listen_owner_failed=false;hold(s);}
 done:printf("NATIVE_LISTEN stage=%d rc=%d intent=%d\n",stage,rc,(int)*intent);fflush(stdout);leave(s);return rc;
}
struct report_guard {struct vv_native_runtime *s;unsigned epoch,revision;uint32_t photo;atomic_bool reported;};
static int report_prompt(struct vv_native_runtime *s,const char *text,const struct vv_https_cancel *cancel){
 int rc=0;size_t bytes=0;
 if(cancel->is_cancelled(cancel->arg))return -ECANCELED;
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
static bool report_cancelled(void *arg){
 struct report_guard *g=arg;struct vv_native_runtime *s=g->s;
 uint32_t photo=0;unsigned mask=0,revision=0,reason=0;int photo_rc=0,hold_rc=0;
 /* Preserve short-circuit order; report only the first failing predicate. */
 if(cancelled(s))reason=1;
 else if(g->epoch!=vv_native_mode_epoch(s->config.mode))reason=2;
 else if(!s->config.ops.ready(s->config.ops.arg))reason=4;
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
int vv_native_runtime_report(struct vv_native_runtime *s,const struct vv_assessment_request *q,struct vv_assessment_result *out,struct vv_report_audio_result *audio){
 if(!s||!q||!out||!audio)return -EINVAL;
 int rc=enter(s);if(rc)return rc;
 if(!s->pending){rc=-ENOENT;goto done;}
 if(cancelled(s)||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)){hold(s);rc=-ECANCELED;goto done;}
 if(!s->config.ops.ready(s->config.ops.arg)){rc=-ENETDOWN;goto done;}
 if(s->config.assessment_ready&&!s->config.assessment_ready(s->config.ops.arg)){rc=-EACCES;goto done;}
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
 if(!rc){memset(out,0,sizeof(*out));memset(audio,0,sizeof(*audio));
  enum vv_assessment_outcome result=vv_native_assessment_upload_only(s->config.assessment,&request,&cancel,out);
  out->outcome=result;if(result!=VV_ASSESSMENT_UPLOADED)rc=out->backend_error?out->backend_error:-EIO;}
 vv_https_secure_zero(images,3u*JPEG_CAP);free(images);
 if(!rc&&report_cancelled(&guard))rc=-ECANCELED;
 if(!rc){
  if(!s->config.ops.play&&!vv_native_client_streaming_tts(s->config.client))rc=-ENOTSUP;
  else rc=report_prompt(s,"皮肤检测报告已完成。",&cancel);
  audio->callback_error=rc;
  printf("NATIVE_UPLOAD_COMPLETE prompt_rc=%d uploaded=%u\n",rc,(unsigned)out->upload_attempted);fflush(stdout);
 }
 if(!rc&&report_cancelled(&guard))rc=-ECANCELED;
 if(!rc)rc=vv_native_mode_assessment_done(s->config.mode,s->pending_epoch);
 if(!rc)rc=s->config.ops.resume(s->config.ops.arg,revision);
 /* A queued resume is not yet tracking. Await the worker acknowledgement;
  * never replay the assessment if this transition fails or is interrupted. */
 if(!rc){
  struct vv_native_mode_ops *o=&s->config.mode->ops;
  struct confirmation p={s,guard.epoch};uint64_t began=o->now_ms(o->arg);
  for(unsigned i=0;i<=300;i++){
   if(confirmation_cancelled(&p)){rc=-ECANCELED;break;}
   uint64_t now=o->now_ms(o->arg);
   if(now<began||now-began>=3000){rc=-ETIMEDOUT;break;}
   rc=o->status(o->arg,1);if(rc!=-EAGAIN)break;
   if(i==300){rc=-ETIMEDOUT;break;}o->sleep_ms(o->arg,10);
  }
  uint64_t finished=o->now_ms(o->arg);
  if(finished<began||finished-began>=3000)rc=-ETIMEDOUT;
  if(confirmation_cancelled(&p))rc=-ECANCELED;
 }
 if(!rc&&(cancelled(s)||s->pending_epoch!=vv_native_mode_epoch(s->config.mode)))rc=-ECANCELED;
 if(rc)hold(s);
 done:leave(s);return rc;
}
