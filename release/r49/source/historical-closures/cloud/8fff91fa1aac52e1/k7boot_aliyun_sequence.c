#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "k7_apm21_product_startup.h"
#if !defined(K7_NEAR_BOOT_PREPARE) || !K7_NEAR_BOOT_PREPARE
#error nominal product startup requires explicit near model boot preparation
#endif
#endif
#line 1
#include "k7boot_aliyun_sequence.h"
#include "../k7agent/cloud/include/vv_local_assessment.h"
#include "native_boot_sequence.h"
#include "../k7agent/cloud/include/vv_speech_owner.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#if defined(K7_NEAR_BOOT_PREPARE) && K7_NEAR_BOOT_PREPARE
#include "../k7sound/raw_vad_owner.h"
#endif
static atomic_bool attempted,cancelled,running;
static atomic_int boot_stage,boot_result;
static const struct k7boot_aliyun_config *config;
static struct vv_aliyun_provider_owner *provider;
static struct vv_native_platform *platform;
static const char network_prompt_owner;
static bool stopped(void*a){(void)a;return atomic_load(&cancelled);}
static uint64_t now(void*a){struct timespec t;(void)a;if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0)return UINT64_MAX;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static void wait_ms(void*a,unsigned ms){(void)a;usleep(ms*1000u);}
#ifdef CONFIG_EXAMPLES_K7SOUND_VAD_PROBE
/* Diagnostic-only admission:0 idle,1 manual probe,2 prepare has begun.
 * No heap/thread/manager; compiled out of the normal19V path. */
static atomic_uint vad_probe_admission;
int k7boot_aliyun_vad_probe_acquire(void){
 unsigned expected=0;
 if(atomic_load(&boot_stage)>1)return -EBUSY;
 if(!atomic_compare_exchange_strong(&vad_probe_admission,&expected,1))return -EBUSY;
 if(atomic_load(&boot_stage)>1){atomic_store(&vad_probe_admission,0);return -EBUSY;}
 return 0;
}
int k7boot_aliyun_vad_probe_release(void){
 unsigned expected=1;
 return atomic_compare_exchange_strong(&vad_probe_admission,&expected,0)?0:-EINVAL;
}
static int vad_probe_prepare_admit(void){
 uint64_t start=now(0),last=start;unsigned waits=0;
 if(start==UINT64_MAX)return -EIO;
 for(;;){
  unsigned expected=0;
  if(stopped(0))return -ECANCELED;
  if(atomic_compare_exchange_strong(&vad_probe_admission,&expected,2))return 0;
  if(expected!=1)return -EBUSY;
  uint64_t t=now(0);if(t==UINT64_MAX||t<last)return -EIO;
  if(t-start>=30000u||++waits>1200u)return -ETIMEDOUT;
  last=t;wait_ms(0,25);
 }
}
#endif
void k7boot_aliyun_status_print(void){
 printf("VVALIYUN_STATUS attempted=%u running=%u stage=%d result=%d\n",
  (unsigned)atomic_load(&attempted),(unsigned)atomic_load(&running),
  atomic_load(&boot_stage),atomic_load(&boot_result));
}
static int local_network_prompt(void){
 if(!config->announce_network_ready||!config->play_network_prompt)return 0;
#if defined(K7_READY_CUE) && K7_READY_CUE
 /* Skip early success audio; the service emits the cue after live RX startup.
  * Preserve provisioning events and all clock/authentication checks. */
 return 0;
#endif
 int rc=vv_native_boot_network_ready(0);if(rc)return rc;
 rc=vv_speech_owner_native_claim(&network_prompt_owner);if(rc)return rc;
 struct vv_https_cancel cancel={stopped,0};
 uint64_t start=now(0);
 rc=config->play_network_prompt(config->network_prompt_arg,&cancel);
 vv_speech_owner_native_release(&network_prompt_owner);
 uint64_t end=now(0);
 printf("VVALIYUN stage=local_network_prompt rc=%d elapsed_ms=%llu\n",rc,
  (unsigned long long)(end>=start?end-start:0));
 return rc;
}
static int prepare(void*a){
#ifdef CONFIG_EXAMPLES_K7SOUND_VAD_PROBE
 int admission=vad_probe_prepare_admit();if(admission)return admission;
#endif
 struct vv_native_platform_config pc;unsigned missing;
 struct vv_k7_platform_boot_config boot=config->platform;
#if defined(K7_READY_CUE) && K7_READY_CUE
 printf("VOICE_STARTUP stage=network_ready monotonic_ms=%llu\n",(unsigned long long)now(0));fflush(stdout);
#endif
 atomic_store(&boot_stage,2);
 int rc=local_network_prompt();if(rc)return rc;
 if(config->take_trusted_time){
  unsigned attempts=0;
  struct vv_https_cancel time_cancel={stopped,0};
  atomic_store(&boot_stage,3);
  puts("VVALIYUN stage=trusted_time_wait source=selected_by_callback");
  for(;;){
   if(stopped(0))return -ECANCELED;
   rc=vv_native_boot_preview_ready(0);if(rc)return rc;
   rc=vv_native_boot_network_ready(0);
   if(rc&&rc!=-EAGAIN)return rc;
   if(!rc){
    ++attempts;
    rc=config->take_trusted_time(config->trusted_time_arg,&time_cancel,&boot.trusted_time);
    if(!rc)break;
    if(rc!=-EAGAIN||attempts>=3)return rc;
   }
   wait_ms(0,100);
  }
  boot.trusted_time.cancelled=stopped;boot.trusted_time.cancel_arg=0;
  printf("VVALIYUN stage=trusted_time_received source=selected_by_callback\n");
 }
 if(stopped(0))return -ECANCELED;
 uint64_t start=now(a);
 if(start>UINT64_MAX-30000)return -EOVERFLOW;
#if defined(K7_READY_CUE) && K7_READY_CUE
 printf("VOICE_STARTUP stage=trusted_time_ready monotonic_ms=%llu\n",(unsigned long long)now(0));fflush(stdout);
#endif
 atomic_store(&boot_stage,4);
 rc=vv_k7_platform_factory_prepare(&boot,start+30000,&pc,&missing);
 printf("VVALIYUN stage=factory rc=%d missing=0x%x\n",rc,missing);
 if(rc)return rc;
 if(stopped(0))return -ECANCELED;
 if(config->prepare_business){
  rc=config->prepare_business(&pc,config->prepare_business_arg);
  printf("VVALIYUN stage=business_prepare rc=%d\n",rc);
  if(rc)return rc;
 }
 struct vv_aliyun_provider_config ac={.tls=pc.tls,.limits=pc.limits,
 .load_key=config->load_key,.key_arg=config->key_arg,.asr_model=config->asr_model,
 .tts_model=config->tts_model,.voice=config->voice,.allow_proven_bj_compat=config->allow_proven_bj_compat,
 .prompt_assets=config->prompt_assets,.prompt_asset_count=config->prompt_asset_count,
 .speech_host=config->speech_host};
 rc=vv_aliyun_provider_create(&ac,&provider);
 printf("VVALIYUN stage=provider_create rc=%d\n",rc);if(rc)return rc;
 pc.speech=vv_aliyun_provider_speech(provider);
 if(!pc.speech||!pc.speech->asr||!pc.speech->tts){printf("VVALIYUN stage=provider_speech rc=%d\n",-EACCES);return -EACCES;}
 if(stopped(0))return -ECANCELED;
#if !defined(K7_READY_CUE) || !K7_READY_CUE
 if(config->announce_network_ready&&!config->play_network_prompt){
  rc=vv_native_boot_network_ready(0);
  printf("VVALIYUN stage=network_prompt_ready rc=%d\n",rc);
  if(rc)return rc;
  if(!pc.speech->speak)return -ENOTSUP;
  rc=vv_speech_owner_native_claim(&network_prompt_owner);
  if(rc)return rc;
  struct vv_https_cancel cancel={stopped,0};
  uint64_t prompt_start=now(0);
  rc=pc.speech->speak(pc.speech->arg,"配网成功。",&cancel);
  vv_speech_owner_native_release(&network_prompt_owner);
  uint64_t prompt_end=now(0);
  printf("VVALIYUN stage=network_prompt rc=%d elapsed_ms=%llu\n",rc,
   (unsigned long long)(prompt_end>=prompt_start?prompt_end-prompt_start:0));
  if(rc)return rc;
  if(stopped(0))return -ECANCELED;
 }
#endif
#if defined(K7_READY_CUE) && K7_READY_CUE
 printf("VOICE_STARTUP stage=provider_ready monotonic_ms=%llu\n",(unsigned long long)now(0));fflush(stdout);
#endif
 platform=vv_native_platform_create(&pc);
 printf("VVALIYUN stage=platform_create rc=%d\n",platform?0:-EIO);if(!platform)return -EIO;
 if(config->load_business){
  struct vv_https_cancel cancel={stopped,0};
  rc=vv_k7_platform_factory_login(platform,config->load_business,config->business_arg,&cancel);
  printf("VVALIYUN stage=business_login rc=%d\n",rc);
  if(rc){
#if K7_LOCAL_ASSESSMENT
   /* Separate speech remains mandatory; transport/HTTP/auth business loss is soft. */
   if(!pc.speech||!vv_local_business_unavailable(rc))return rc;
   if(stopped(0))return -ECANCELED;
   printf("VVALIYUN stage=business_unavailable rc=%d local_capture=1\n",rc);
#elif defined(CONFIG_EXAMPLES_K7CLOUD_DIALOGUE)
   /* Optional business availability must not disable the external speech
    * provider. PHOTO stays gated and the existing idle login uses backoff.
    * Only availability failures defer: trust, clock, configuration, memory,
    * protocol and explicit cancellation still abort this startup. */
   if(!config->allow_business_login_deferred||
      (rc!=VV_BACKEND_CLIENT_HTTP&&rc!=VV_BACKEND_CLIENT_AUTH&&
       rc!=VV_HTTPS_EIO&&rc!=VV_HTTPS_ETIMEOUT))return rc;
   if(stopped(0))return -ECANCELED;
   printf("VVALIYUN stage=business_login_deferred rc=%d photo_ready=0\n",rc);
#else
   return rc;
#endif
  }
  if(stopped(0))return -ECANCELED;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  rc=vv_native_platform_set_business_loader(platform,config->load_business,config->business_arg);
  if(rc)return rc;
#endif
 }
#if defined(K7_READY_CUE) && K7_READY_CUE
 printf("VOICE_STARTUP stage=login_ready monotonic_ms=%llu\n",(unsigned long long)now(0));fflush(stdout);
#endif
 rc=vv_native_platform_bind(platform);
 printf("VVALIYUN stage=platform_bind rc=%d\n",rc);return rc;
}
static int service(void*a){
 int rc;
 if(stopped(a))return -ECANCELED;
#if defined(K7_READY_CUE) && K7_READY_CUE
 printf("VOICE_STARTUP stage=service_launch monotonic_ms=%llu\n",(unsigned long long)now(0));fflush(stdout);
#endif
 atomic_store(&boot_stage,5);
 rc=vv_native_service_run();printf("VVALIYUN stage=service_start rc=%d\n",rc);if(rc)return rc;
 /* run starts a worker; it does not join it. Keep borrowed configuration and
  * provider alive until status confirms worker cleanup/owner release ended.
  * Repeat cancellation after startup: run resets the service stopping token. */
 for(;;){
  struct vv_native_service_status status;
  if(stopped(a))(void)vv_native_service_stop();
  rc=vv_native_service_status(&status);
  if(rc){printf("VVALIYUN stage=service_status rc=%d\n",rc);(void)vv_native_service_stop();return rc;}
  if(!status.running){rc=stopped(a)?-ECANCELED:status.last_error;printf("VVALIYUN stage=service_exit rc=%d\n",rc);return rc;}
  wait_ms(a,100);
 }
}
void k7boot_aliyun_cancel(void){atomic_store(&cancelled,true);vv_native_service_stop();}
#if defined(K7_APM21_RECORDED_QUIET) && K7_APM21_RECORDED_QUIET
#ifndef CONFIG_EXAMPLES_K7SOUND_VAD_PROBE
#error Recorded replay requires the existing VAD admission state
#endif
/* Serialize only cleanup and this private post-stop loan. No waits or heap.
 * Admission 3 is private and cannot be obtained by the old manual probe.
 * Pointer state is read under the same lock as cleanup, after stage6 release.
 * A wrong owner or failed release keeps the loan and blocks cleanup. */
static atomic_flag recorded_lifecycle_lock=ATOMIC_FLAG_INIT;
static const void *recorded_loan_owner;
static unsigned recorded_previous_admission;
static int recorded_lifecycle_enter(void){
 return atomic_flag_test_and_set_explicit(&recorded_lifecycle_lock,memory_order_acquire)?-EBUSY:0;
}
static void recorded_lifecycle_leave(void){
 atomic_flag_clear_explicit(&recorded_lifecycle_lock,memory_order_release);
}
static bool recorded_poststop_ready(void){
 return atomic_load(&attempted)&&!atomic_load(&running)&&
        atomic_load(&boot_stage)==6&&!config&&!platform&&!provider;
}
int k7boot_aliyun_recorded_quiet_acquire(const void *owner){
 if(!owner)return -EINVAL;
 int rc=recorded_lifecycle_enter();if(rc)return rc;
 if(recorded_loan_owner||!recorded_poststop_ready()){rc=-EBUSY;goto done;}
 unsigned previous=atomic_load(&vad_probe_admission);
 if(previous!=0u&&previous!=2u){rc=-EBUSY;goto done;}
 bool cancel_snapshot=atomic_load(&cancelled);
 unsigned expected=previous;
 if(!atomic_compare_exchange_strong(&vad_probe_admission,&expected,3u)){rc=-EBUSY;goto done;}
 recorded_previous_admission=previous;recorded_loan_owner=owner;
 /* A naturally completed startup may leave cancelled false. This loan does
  * not change cancellation or treat its nonzero value as authorization. */
 if(!recorded_poststop_ready()||atomic_load(&cancelled)!=cancel_snapshot){
  expected=3u;
  if(!atomic_compare_exchange_strong(&vad_probe_admission,&expected,previous)){rc=-ESTALE;goto done;}
  recorded_loan_owner=0;recorded_previous_admission=0;rc=-EBUSY;
 }
done:
 recorded_lifecycle_leave();return rc;
}
int k7boot_aliyun_recorded_quiet_release(const void *owner){
 if(!owner)return -EINVAL;
 int rc=recorded_lifecycle_enter();if(rc)return rc;
 if(recorded_loan_owner!=owner){rc=-EPERM;goto done;}
 unsigned expected=3u;
 if(!atomic_compare_exchange_strong(&vad_probe_admission,&expected,recorded_previous_admission)){
  rc=-ESTALE;goto done;
 }
 recorded_loan_owner=0;recorded_previous_admission=0;
done:
 recorded_lifecycle_leave();return rc;
}
int k7boot_aliyun_cleanup(void){
 int rc=recorded_lifecycle_enter();if(rc)return rc;
 if(recorded_loan_owner||atomic_load(&running)){rc=-EBUSY;goto done;}
 if(platform){rc=vv_native_platform_destroy(platform);if(rc)goto done;platform=0;}
 if(provider){rc=vv_aliyun_provider_destroy(provider);if(rc)goto done;provider=0;}
 config=0;
done:
 recorded_lifecycle_leave();return rc;
}
#else
#line 183
int k7boot_aliyun_cleanup(void){
 if(atomic_load(&running))return -EBUSY;
 if(platform){int rc=vv_native_platform_destroy(platform);if(rc)return rc;platform=0;}
 if(provider){int rc=vv_aliyun_provider_destroy(provider);if(rc)return rc;provider=0;}
 config=0;return 0;
}
#endif
#line 189
#if defined(K7_NEAR_BOOT_PREPARE) && K7_NEAR_BOOT_PREPARE
static int prepare_near_model(void){
 if(stopped(0))return -ECANCELED;
 int rc=k7_raw_vad_owner_prepare();
 struct k7_raw_vad_owner_status s={0};
 int status=k7_raw_vad_owner_status(&s);
 if(!rc||rc==-EALREADY){
  rc=status?status:(s.phase!=K7_RAW_OWNER_READY||!s.model_generation||
      s.voice_owner||s.borrow_owner)?-EBUSY:0;
 }
 if(!rc&&stopped(0))rc=-ECANCELED;
 printf("NEAR_EXPERIMENT boot_model_prepare=1 rc=%d status_rc=%d phase=%u generation=%llu voice_owner=%llu borrow_owner=%llu before_network_wait=1\n",
   rc,status,s.phase,(unsigned long long)s.model_generation,
   (unsigned long long)s.voice_owner,(unsigned long long)s.borrow_owner);
 return rc;
}
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
static int wait_product_arm(uint64_t *token){
#if defined(K7_APM21_STANDALONE_STARTUP) && K7_APM21_STANDALONE_STARTUP
 if(stopped(0))return -ECANCELED;
 int armed=k7_raw_vad_owner_product_startup_arm();
 printf("APM21_PRODUCT standalone_startup=1 arm_result=%d model_prepare=0 actions=0\n",armed);
 if(armed)return armed;
#else
 puts("APM21_PRODUCT waiting_explicit_arm=1 model_prepare=0 actions=0");
#endif
 for(;;){
  if(stopped(0))return -ECANCELED;
  uint64_t current=0;int rc=k7_raw_vad_owner_product_startup_ready(&current);
  if(!rc){rc=k7_raw_vad_owner_product_startup_claim(current);if(!rc){*token=current;return 0;}}
  if(rc!=-EAGAIN&&rc!=-EBUSY&&rc!=-ESTALE)return rc;
  wait_ms(0,100);
 }
}
#endif
int k7boot_aliyun_run(const struct k7boot_aliyun_config*c){
 if(!c||!c->load_key||!c->asr_model||!c->tts_model||!c->voice||c->platform.business.speech)return -EINVAL;
 if(c->allow_business_login_deferred){
  if(!c->load_business)return -EINVAL;
#ifndef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  return -ENOTSUP;
#endif
 }
 if(atomic_exchange(&attempted,true))return -EALREADY;
 config=c;atomic_store(&running,true);
 atomic_store(&boot_stage,1);atomic_store(&boot_result,0);
 const struct vv_native_boot_ops o={vv_native_boot_preview_start,vv_native_boot_preview_ready,vv_native_boot_network_ready,prepare,service,vv_native_boot_stop_hold,stopped,now,wait_ms,0};
 #if defined(K7_NEAR_BOOT_PREPARE) && K7_NEAR_BOOT_PREPARE
 #if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 uint64_t product_token=0;
 int rc=wait_product_arm(&product_token);
 if(!rc)rc=prepare_near_model();
#else
 int rc=prepare_near_model();
#endif
 if(!rc)rc=vv_native_boot_run(&o);
 atomic_store(&running,false);
#else
 int rc=vv_native_boot_run(&o);atomic_store(&running,false);
#endif
 int cleanup=k7boot_aliyun_cleanup();
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(product_token&&!cleanup){int release=k7_raw_vad_owner_product_startup_release(product_token);if(release)cleanup=release;}
#endif
 atomic_store(&boot_result,cleanup?cleanup:rc);atomic_store(&boot_stage,6);
 printf("VVALIYUN stage=sequence_exit rc=%d cleanup_rc=%d\n",rc,cleanup);
 return cleanup?cleanup:rc;
}
