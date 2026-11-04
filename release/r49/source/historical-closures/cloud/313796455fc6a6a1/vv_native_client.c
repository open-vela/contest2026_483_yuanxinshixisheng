#include "vv_real_shuiguang.h"
#if K7_REAL_SHUIGUANG_PROFILE
#include "vv_real_business_ca.h"
#endif
#include "vv_native_client.h"
#include "vv_real_report_speech.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_DEV_LAN_HTTP
#include "vv_http_development.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

struct vv_native_client {
  struct vv_https_mbedtls *tls;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DEV_LAN_HTTP
  struct vv_http_development plain;
#endif
  struct vv_https_client https;
  struct vv_backend_client backend;
  const char *asr_path, *tts_path;
  bool tts_device_bearer;
  struct vv_speech_provider speech;
};
static bool path_ok(const char *s, bool base) {
  if (!s || strlen(s)>255 || strpbrk(s,"\r\n?#")) return false;
  if (!*s) return base;
  for (const unsigned char *p=(const unsigned char *)s; *p; ++p)
    if (*p<=32 || *p>=127) return false;
  return *s=='/' && (!base || s[strlen(s)-1]!='/');
}
static bool host_ok(const char *s) {
  if (!s || !*s || strlen(s)>128) return false;
  for (; *s; ++s) {
    unsigned char c=(unsigned char)*s;
    if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
          (c>='0' && c<='9') || c=='.' || c=='-')) return false;
  }
  return true;
}
struct vv_native_client *vv_native_client_create(const struct vv_native_client_config *c) {
  if (!c || (c->speech && (!c->speech->asr || !c->speech->tts)) || !host_ok(c->host) || !path_ok(c->base_path,true) ||
      !path_ok(c->asr_path,false) || !path_ok(c->tts_path,false) || !c->utc_now ||
      !c->limits.max_request_header_bytes || !c->limits.max_response_header_bytes ||
      !c->limits.max_response_body_bytes || !c->limits.max_chunk_bytes ||
      !c->limits.total_timeout_ms) {printf("NATIVE_CLIENT create_stage=1 result=%d\n",-EINVAL);return NULL;}
#if K7_REAL_SHUIGUANG_PROFILE
  if(!c->speech||!c->speech->asr||!c->speech->tts||!vv_real_business_target(c->host,c->port,c->base_path,c->development_lan_http))return NULL;
#endif
  if(c->development_lan_http) {
#ifndef CONFIG_EXAMPLES_K7CLOUD_DEV_LAN_HTTP
    return NULL;
#else
    if(strcmp(c->host,"10.3.3.170")||c->port!=18085||*c->base_path||
       !c->speech||!c->tls.tcp_connect||!c->tls.now_ms)return NULL;
#endif
  }
  uint64_t now=0;
  if (!c->utc_now(c->clock_arg,&now) || !now) {printf("NATIVE_CLIENT create_stage=2 result=%d\n",-EACCES);return NULL;}
  struct vv_native_client *n=calloc(1,sizeof(*n));
  if (!n) {printf("NATIVE_CLIENT create_stage=3 result=%d\n",-ENOMEM);return NULL;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_DEV_LAN_HTTP
  n->plain.fd=-1;
  if(c->development_lan_http){
    n->plain.config=c->tls;
    n->https=(struct vv_https_client){.transport=vv_http_development_ops(),
      .transport_ctx=&n->plain,.now_ms=c->tls.now_ms,.now_arg=c->tls.now_arg,.limits=c->limits};
  } else
#endif
  {
#if K7_REAL_SHUIGUANG_PROFILE
  /* Only the business connection changes roots; speech owns its TLS config. */
  struct vv_https_mbedtls_config business_tls=c->tls;
  business_tls.ca_pem=vv_real_business_ca_pem;
  business_tls.ca_pem_length=sizeof(vv_real_business_ca_pem);
  n->tls=vv_https_mbedtls_create(&business_tls);
#else
  n->tls=vv_https_mbedtls_create(&c->tls);
#endif
  if (!n->tls) {printf("NATIVE_CLIENT create_stage=4 result=%d\n",-EIO);vv_https_secure_zero(n,sizeof(*n));free(n);return NULL;}
  n->https=(struct vv_https_client){.transport=vv_https_mbedtls_ops(),
    .transport_ctx=n->tls,.now_ms=c->tls.now_ms,.now_arg=c->tls.now_arg,.limits=c->limits};
  }
  n->backend=(struct vv_backend_client){.https=&n->https,.host=c->host,
    .port=c->port,.base_path=c->base_path,.utc_now=c->utc_now,.clock_arg=c->clock_arg,.development_lan_http=c->development_lan_http};
  n->asr_path=c->asr_path;n->tts_path=c->tts_path;n->tts_device_bearer=c->tts_device_bearer;
  if(c->speech)n->speech=*c->speech;
  return n;
}
void vv_native_client_destroy(struct vv_native_client *n) {
  if (!n) return;
  vv_backend_client_clear_session(&n->backend);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DEV_LAN_HTTP
  vv_http_development_close(&n->plain);
#endif
  vv_https_mbedtls_destroy(n->tls);
  vv_https_secure_zero(n,sizeof(*n));free(n);
}
struct vv_backend_client *vv_native_client_backend(struct vv_native_client *n) {
  return n?&n->backend:NULL;
}
int vv_native_client_asr(struct vv_native_client *n,const unsigned char *pcm,size_t size,
  const struct vv_https_cancel *cancel,struct vv_asr_batch_result *out) {
  if (out) memset(out,0,sizeof(*out));
  if (!n || !out || !pcm || !size || size%2 || size>VV_ASR_BATCH_PCM_MAX)
    return VV_ASR_BATCH_INVALID;
  if (cancel && cancel->is_cancelled && cancel->is_cancelled(cancel->arg))
    return -ECANCELED;
  if(n->speech.asr)return n->speech.asr(n->speech.arg,pcm,size,cancel,out);
  unsigned char random[16]={0};char boundary[33]={0};
  int rc=vv_https_mbedtls_random(n->tls,random,sizeof(random));
  if (!rc) {
    static const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<16;++i) {boundary[2*i]=hex[random[i]>>4];boundary[2*i+1]=hex[random[i]&15];}
    struct vv_asr_batch_request q={.host=n->backend.host,.port=n->backend.port,
      .path=n->asr_path,.boundary_hex=boundary,.pcm=pcm,.pcm_bytes=size,
      .sample_rate=16000,.channels=1,.bits_per_sample=16};
    rc=vv_asr_batch_perform(&n->https,&q,cancel,out);
  }
  vv_https_secure_zero(random,sizeof(random));vv_https_secure_zero(boundary,sizeof(boundary));
  return rc;
}
int vv_native_client_tts(struct vv_native_client *n,const char *text,unsigned char *pcm,
  size_t capacity,size_t *size,const struct vv_https_cancel *cancel) {
  if (!n) {if(size)*size=0;if(pcm)vv_https_secure_zero(pcm,capacity);return VV_BACKEND_CLIENT_INVALID;}
  if(n->speech.tts)return n->speech.tts(n->speech.arg,text,pcm,capacity,size,cancel);
  struct vv_backend_endpoint e={n->backend.host,n->backend.port,n->tts_path};
  return vv_backend_client_tts(&n->backend,&e,n->tts_device_bearer,text,pcm,capacity,size,cancel);
}

bool vv_native_client_streaming(const struct vv_native_client *n){return n&&n->speech.listen;}
bool vv_native_client_streaming_tts(const struct vv_native_client *n){return n&&n->speech.speak;}
int vv_native_client_speak(struct vv_native_client *n,const char *text,const struct vv_https_cancel *cancel){
 if(!n||!text)return -EINVAL;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 return n->speech.speak?n->speech.speak(n->speech.arg,text,cancel):-ENOSYS;
}
int vv_native_client_listen(struct vv_native_client *n,unsigned samples,const struct vv_https_cancel *cancel,struct vv_asr_batch_result *out){
 if(out)memset(out,0,sizeof(*out));
 if(!n||!out||!n->speech.listen||samples<320||samples>VV_ASR_LISTEN_SAMPLES_MAX||samples%320)return -EINVAL;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 return n->speech.listen(n->speech.arg,samples,cancel,out);
}

int vv_native_client_speak_report(struct vv_native_client *n,const char *text,const struct vv_https_cancel *cancel,size_t *attempted){
 if(attempted)*attempted=0;
 if(!n||!text||!attempted||!cancel||!cancel->is_cancelled)return -EINVAL;
#if K7_REAL_SHUIGUANG_PROFILE
 if(!vv_real_business_target(n->backend.host,n->backend.port,n->backend.base_path,n->backend.development_lan_http))return -EACCES;
 if(cancel->is_cancelled(cancel->arg))return -ECANCELED;
 return vv_aliyun_provider_speak_report(&n->speech,text,cancel,attempted);
#else
 return -ENOTSUP;
#endif
}
