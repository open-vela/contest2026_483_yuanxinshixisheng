#include "../k7agent/cloud/include/vv_real_shuiguang.h"
#include "k7boot_aliyun_production.h"
#include "k7boot_aliyun_sequence.h"
#include "k7boot_aliyun_production_authority.h"
#include "../k7agent/cloud/include/vv_speech_only_callbacks.h"
#include "k7boot_business.h"
#include "../k7agent/cloud/include/vv_stream_speaker.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "../k7agent/cloud/include/vv_session_pcm.h"
#endif
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_EXAMPLES_K7CLOUD_ROUGHTIME
#include "../k7agent/cloud/include/vv_roughtime_k7.h"
#include "../k7radio/prov_phone_time.h"
#include <time.h>
#include <unistd.h>
static bool phone_window_done;
static bool time_cancelled(const struct vv_https_cancel*c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static uint64_t time_mono_ms(void){
 struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||(uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static int finish_time_selection(struct vv_trusted_time_sample *out,int rc){
 phone_window_done=true;k7radio_close_phone_time();
 if(rc)memset(out,0,sizeof(*out));
 return rc;
}
static int take_phone_sample(const struct vv_https_cancel *cancel,
 struct vv_trusted_time_sample *out){
 if(time_cancelled(cancel))return -ECANCELED;
 uint64_t utc=0,received=0;int rc=k7radio_take_phone_time(&utc,&received);
 if(rc)return rc;
 if(time_cancelled(cancel))return -ECANCELED;
 *out=(struct vv_trusted_time_sample){.present=true,.authorized=true,
  .utc_seconds=utc,.received_mono_ms=received};
 if(cancel){out->cancelled=cancel->is_cancelled;out->cancel_arg=cancel->arg;}
 puts("VVALIYUN time_source=user_approved_phone timezone=UTC+08:00");
 return 0;
}
static int take_network_time(void *arg,const struct vv_https_cancel *cancel,
 struct vv_trusted_time_sample *out){
 if(!out)return -EINVAL;
 memset(out,0,sizeof(*out));
 if(!phone_window_done){
  uint64_t start=time_mono_ms(),last=start;
  if(start==UINT64_MAX||start>UINT64_MAX-5000)return finish_time_selection(out,-EIO);
  puts("VVALIYUN time_source=phone_wait window_ms=5000 timezone=UTC+08:00");
  for(;;){
   if(time_cancelled(cancel))return finish_time_selection(out,-ECANCELED);
   uint64_t current=time_mono_ms();
   if(current==UINT64_MAX||current<last)return finish_time_selection(out,-EIO);
   last=current;
   if(current-start>=5000)break;
   int rc=take_phone_sample(cancel,out);
   if(!rc)return finish_time_selection(out,0);
   if(time_cancelled(cancel))return finish_time_selection(out,-ECANCELED);
   if(rc==-ECANCELED)break;
   if(rc!=-EAGAIN&&rc!=-ETIMEDOUT)return finish_time_selection(out,rc);
   unsigned pause=(unsigned)(5000-(current-start));if(pause>50)pause=50;
   usleep(pause*1000u);
  }
  /* The priority window ends, not the BLE time mailbox. A legitimate phone
   * sample may arrive while authenticated network time is being fetched. */
  phone_window_done=true;
 }
 if(time_cancelled(cancel))return finish_time_selection(out,-ECANCELED);
 int phone=take_phone_sample(cancel,out);
 if(!phone)return finish_time_selection(out,0);
 if(time_cancelled(cancel))return finish_time_selection(out,-ECANCELED);
 if(phone!=-EAGAIN&&phone!=-ETIMEDOUT&&phone!=-ECANCELED)return finish_time_selection(out,phone);
 uint64_t began=time_mono_ms();
 if(began==UINT64_MAX)return finish_time_selection(out,-EIO);
 int rc=vv_roughtime_k7_fetch(arg,cancel,out);
 if(time_cancelled(cancel)||rc==-ECANCELED)return finish_time_selection(out,-ECANCELED);
 uint64_t finished=time_mono_ms();
 if(finished==UINT64_MAX||finished<began)return finish_time_selection(out,-EIO);
 struct vv_trusted_time_sample late={0};
 phone=take_phone_sample(cancel,&late);
 if(time_cancelled(cancel))return finish_time_selection(out,-ECANCELED);
 /* Phone has explicit priority even if both sources have completed. The
  * mailbox enforces session continuity, freshness and elapsed compensation. */
 if(!phone){*out=late;return finish_time_selection(out,0);}
 if(phone!=-EAGAIN&&phone!=-ETIMEDOUT&&phone!=-ECANCELED)return finish_time_selection(out,phone);
 if(!rc){puts("VVALIYUN time_source=authenticated_network");return finish_time_selection(out,0);}
 /* Only the sequence's existing bounded EAGAIN retry keeps the mailbox open.
  * The production worker closes it when the outer sequence finally exits. */
 if(rc!=-EAGAIN)return finish_time_selection(out,rc);
 memset(out,0,sizeof(*out));
 return rc;
}

#endif
/* Private build input: static arrays and optional endpoint string macro, never trust flags.
 * Firmware-contained development credentials remain extractable. */
#ifdef K7BOOT_ALIYUN_PUBLIC_TEST
#ifdef __NuttX__
#error "Public test credentials are forbidden in a board build"
#endif
#include "fixtures/aliyun_boot_config.inc"
#else
#include "private/aliyun_boot_config.inc"
#endif
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_PROMPTS
#ifdef K7BOOT_ALIYUN_PUBLIC_TEST
#include "fixtures/voice_prompt_pcm.inc"
#else
#include "private/voice_prompt_pcm.inc"
#endif
#endif
static struct k7boot_aliyun_config config;
static struct vv_speech_only_callbacks callbacks={vv_speech_only_k7_preview_alive,0};
static atomic_bool attempted;
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_PROMPTS
static int prompt_cancelled(void *arg){
 const struct vv_https_cancel *cancel=arg;
 return cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg);
}
#ifndef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
static int fetch_network_prompt(void *arg,vv_stream_pcm_sink sink,void *sink_arg,
 const struct vv_https_cancel *cancel){
 const struct vv_prompt_pcm_asset *asset=arg;
 for(size_t at=0;at<asset->bytes;){
  if(prompt_cancelled((void*)cancel))return -ECANCELED;
  size_t n=asset->bytes-at;if(n>8192)n=8192;
  int rc=sink(sink_arg,asset->pcm+at,n);if(rc)return rc;at+=n;
 }
 return prompt_cancelled((void*)cancel)?-ECANCELED:0;
}
#endif
static int play_network_prompt(void *arg,const struct vv_https_cancel *cancel){
 const struct vv_prompt_pcm_asset *asset=arg;
 if(!asset||!asset->pcm||!asset->bytes||asset->bytes%2||
    asset->bytes>VV_PROMPT_CACHE_ITEM_MAX)return -EINVAL;
 if(prompt_cancelled((void*)cancel))return -ECANCELED;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 /* Network-ready precedes service/manager startup. This explicit bootstrap
  * completes the one-shot CPU1 owner before any resident audio is created. */
 return vv_bootstrap_pcm_play(asset->pcm,asset->bytes,cancel);
#else
 /* Existing default path; the immutable asset requires neither TLS nor a
  * provider instance. No session-error fallback is attempted. */
 return vv_stream_speaker_run(fetch_network_prompt,arg,cancel);
#endif
}
#endif
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_BUSINESS
static struct k7boot_business_config business;
static struct k7boot_business_callbacks business_callbacks;
static bool business_configured;
#ifdef K7BOOT_ALIYUN_PUBLIC_TEST
#include "fixtures/business_boot_config.inc"
#elif K7_REAL_SHUIGUANG_PROFILE
#include "private/real_shuiguang_business_boot_config.inc"
#elif defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
/* Explicit new origin: absence of its private credentials is a build error. */
#include "private/business_cloud_boot_config.inc"
#else
#include "private/business_boot_config.inc"
#endif
static int load_business(void *arg,struct vv_k7_secure_login *out){
 (void)arg;if(!out)return -EINVAL;memset(out,0,sizeof(*out));
 const char *src[]={vv_private_business_credential,vv_private_business_version,vv_private_business_proof};
 const size_t sizes[]={sizeof(vv_private_business_credential),sizeof(vv_private_business_version),sizeof(vv_private_business_proof)};
 char *dst[]={out->credential,out->version,out->proof};
 const size_t caps[]={sizeof(out->credential),sizeof(out->version),sizeof(out->proof)};
 for(unsigned i=0;i<3;i++)if(sizes[i]<2||sizes[i]>caps[i]||memchr(src[i],0,sizes[i]-1)||src[i][sizes[i]-1])return -EINVAL;
 for(unsigned i=0;i<3;i++)memcpy(dst[i],src[i],sizes[i]);
 return 0;
}
int k7boot_aliyun_business_private_init(struct k7boot_business_config *c){
 if(!c)return -EINVAL;
#if K7_REAL_SHUIGUANG_PROFILE
#if defined(K7BOOT_ALIYUN_PUBLIC_TEST)
 const char *consent="dev-consent";
#else
 if(!vv_private_business_consent_ref[0])return -EINVAL;
 const char *consent=vv_private_business_consent_ref;
#endif
 *c=(struct k7boot_business_config){.load=load_business,.consent_ref=consent,.poll_interval_ms=1000,.max_polls=300,.total_timeout_ms=300000};return 0;
#elif defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
 *c=(struct k7boot_business_config){.load=load_business,.consent_ref="dev-consent",.poll_interval_ms=1000,.max_polls=30,.total_timeout_ms=30000};return 0;
#else
 *c=(struct k7boot_business_config){.load=load_business,.consent_ref="dev-consent",.poll_interval_ms=5000,.max_polls=60,.total_timeout_ms=300000};return 0;
#endif
}
static int prepare_business(struct vv_native_platform_config *pc,void *arg){
 (void)arg;
#if K7_REAL_SHUIGUANG_PROFILE
 /* Startup installs the speech provider after business configuration. */
 if(!pc)return -EINVAL;
 pc->development_lan_http=false;
 puts("REAL_BUSINESS transport=https host=" VV_REAL_BUSINESS_HOST " port=443 base=" VV_REAL_BUSINESS_BASE);
#elif defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
 pc->development_lan_http=false;
#elif defined(CONFIG_EXAMPLES_K7CLOUD_DEV_LAN_HTTP)
 pc->development_lan_http=true;
#else
 return -ENOTSUP;
#endif
 business.random_bytes=pc->tls.entropy_read;business.random_arg=pc->tls.entropy_arg;
 int rc=k7boot_business_apply(pc,&business_callbacks,&business);
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALWAYS_WAKE
 if(!rc){pc->always_wake=true;pc->scoped_dialogue=true;pc->confirmed_barge=false;}
#endif
 return rc;
}
#endif
int k7boot_aliyun_business_configure(const struct k7boot_business_config *c){
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_BUSINESS
 if(!c||!c->load||!c->consent_ref||!*c->consent_ref)return -EINVAL;
 if(atomic_load(&attempted)||business_configured)return -EBUSY;
 business=*c;business_configured=true;return 0;
#else
 (void)c;return -ENOTSUP;
#endif
}
static int load_key(void *arg,char *out,size_t cap){
 (void)arg;
 if(!out||!cap)return -EINVAL;
 out[0]=0;
 size_t n=sizeof(vv_private_aliyun_key)-1;
 if(!n||n>2048||cap<=n||memchr(vv_private_aliyun_key,0,n))return -EINVAL;
 memcpy(out,vv_private_aliyun_key,n+1);return 0;
}
int k7boot_aliyun_production_worker(int argc,char **argv){
 (void)argc;(void)argv;
 if(atomic_exchange(&attempted,true))return -EALREADY;
 config.platform.ca_pem=k7boot_aliyun_production_ca(&config.platform.ca_pem_length);
 int policy=k7boot_aliyun_production_authority(&config.platform.authority,&config.platform.ca_approved);
 if(policy){memset(&config.platform.authority,0,sizeof(config.platform.authority));config.platform.ca_approved=false;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 /* No source selection here; factory rejects any nonfixed authority/raw. */
 config.platform.renew_entropy_lease=true;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_ROUGHTIME
 config.platform.authority.rtc_trusted_provisioning_verified=false;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 config.platform.renew_trusted_time=true;
#endif
 config.take_trusted_time=take_network_time;
 config.trusted_time_arg=&config.platform.authority;
#endif
 config.platform.business.limits=(struct vv_https_limits){
  .max_request_header_bytes=4096,.max_response_header_bytes=8192,
  .max_response_body_bytes=1048576,.max_chunk_bytes=65536,.total_timeout_ms=60000};
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
 config.platform.business.samples=160000;
#else
 config.platform.business.samples=48000;
#endif
 config.platform.business.pcm_capacity=960000;
 int rc=vv_speech_only_callbacks_apply(&config.platform.business,&callbacks);
 if(rc)return rc;
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_BUSINESS
 if(!business_configured){rc=k7boot_aliyun_business_private_init(&business);if(rc)return rc;}
 if(!business.load||!business.consent_ref||!*business.consent_ref)return -EINVAL;
 config.load_business=business.load;config.business_arg=business.load_arg;
 config.prepare_business=prepare_business;
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_BUSINESS_LOGIN_DEFERRED
 config.allow_business_login_deferred=true;
#endif
#endif
#ifdef VV_PRIVATE_ALIYUN_SPEECH_HOST
 config.speech_host=VV_PRIVATE_ALIYUN_SPEECH_HOST;
#endif
 config.load_key=load_key;config.asr_model=vv_private_aliyun_asr_model;
 config.tts_model=vv_private_aliyun_tts_model;config.voice=vv_private_aliyun_voice;
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_PROMPTS
 if(strcmp(vv_private_prompt_model,config.tts_model)||strcmp(vv_private_prompt_voice,config.voice))return -EINVAL;
 config.prompt_assets=vv_private_prompt_assets;
 config.prompt_asset_count=sizeof(vv_private_prompt_assets)/sizeof(vv_private_prompt_assets[0]);
 bool has_network_prompt=false;
 for(size_t i=0;i<config.prompt_asset_count;i++)
  if(config.prompt_assets[i].text&&!strcmp(config.prompt_assets[i].text,"配网成功。")){
   has_network_prompt=true;
   config.play_network_prompt=play_network_prompt;
   config.network_prompt_arg=(void*)&config.prompt_assets[i];
  }
 if(!has_network_prompt)return -EINVAL;
 config.announce_network_ready=true;
#endif
 /* Exact BJ compatibility was established by the existing HTTPS WAV evidence. */
 config.allow_proven_bj_compat=true;
 printf("VVALIYUN boot policy_result=%d private_config=1 business_enabled=%d\n",policy,config.load_business!=NULL);
 rc=k7boot_aliyun_run(&config);
#ifdef CONFIG_EXAMPLES_K7CLOUD_ROUGHTIME
 /* Also seal when preview/network admission or the final EAGAIN retry ends
  * the sequence before another time callback. No implicit boot retry. */
 k7radio_close_phone_time();
#endif
 printf("VVALIYUN boot result=%d\n",rc);
 /* Static configuration survives any EBUSY retirement. No implicit restart. */
 return rc;
}
