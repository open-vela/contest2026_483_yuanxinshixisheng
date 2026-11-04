#if defined(K7_MICROCRYSTAL) && K7_MICROCRYSTAL
#include "vv_micro_voice_audio_assets.inc"
#endif
#if defined(K7_MICROCRYSTAL) && K7_MICROCRYSTAL
#include "vv_microcrystal_cue_assets.inc"
#endif
#if defined(K7_REAL_SHUIGUANG_PROFILE) && K7_REAL_SHUIGUANG_PROFILE
#include "vv_real_failure_assets.inc"
#include "vv_report_wait_assets.inc"
#endif
#if defined(K7_LOCAL_ASSESSMENT) && K7_LOCAL_ASSESSMENT
#include "vv_capture_success_assets.inc"
#endif
#if defined(K7_REWAKE_PROMPT) && K7_REWAKE_PROMPT
#include "vv_rewake_assets.inc"
#endif
#include "vv_ready_assets.inc"
#include "vv_control_ack_assets.inc"
#include "vv_aliyun_provider.h"
#include "vv_real_report_speech.h"
#include "vv_chat_stream.h"
#include "vv_local_report_assets.inc"
#include "vv_aliyun_endpoint.h"
#include "vv_aliyun_client.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
#include "vv_aliyun_chat.h"
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
#include "vv_reply_pipeline.h"
#include "vv_tts_ws.h"
#include "vv_wss_client.h"
#include <time.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_STREAM
#include "vv_paraformer_k7.h"
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
#include "vv_stream_speaker.h"
#include "vv_prompt_cache.h"
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "vv_firefly_prompt_assets.inc"
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_tts_segments.h"
#endif
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
struct vv_aliyun_provider_owner {
 struct vv_https_mbedtls *tls;
 struct vv_https_client https;
 struct vv_aliyun_client client;
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
 /* The whole reply owns two independently seeded, verified TLS contexts. */
 struct vv_https_mbedtls *chat_tls;
 struct vv_https_client chat_https;
#endif
 struct vv_speech_provider inner,exposed;
 atomic_bool active;
 vv_https_now_ms_fn now;void *clock_arg;
 char key[2049],asr_model[129],tts_model[129],voice[129],speech_host[129];
 char default_voice[129];bool firefly_voice;unsigned volume;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
 struct vv_prompt_cache prompts;
 const struct vv_prompt_pcm_asset *default_prompt_assets;
 size_t default_prompt_asset_count;
 unsigned warmup_attempted;
#endif
};
/* Playback percentage is applied exactly once, before the original sink
 * publishes PCM into the spool/TX/render reference. Cached/borrowed PCM stays
 * immutable; it is always stored at its original synthesis level. Default50
 * takes the exact original pointer/bytes path, including block boundaries. */

static int volume_sample(unsigned char lo,unsigned char hi,unsigned volume){
 int value=(int)((unsigned)lo|((unsigned)hi<<8));
 if(value>=32768)value-=65536;
 value=value*(int)volume/50;
 if(value>32767)value=32767;else if(value< -32768)value=-32768;
 return value;
}
static void volume_pcm(unsigned char *pcm,size_t bytes,unsigned volume){
 for(size_t at=0;at<bytes;at+=2){
  unsigned value=(unsigned)volume_sample(pcm[at],pcm[at+1],volume)&65535u;
  pcm[at]=(unsigned char)value;pcm[at+1]=(unsigned char)(value>>8);
 }
}
#if defined(CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM) || defined(CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM)
struct volume_sink {struct vv_aliyun_provider_owner *owner;vv_stream_pcm_sink sink;void *arg;};
static int volume_sink_pcm(void *arg,const unsigned char *pcm,size_t bytes){
 struct volume_sink *v=arg;
 if(!v||!v->owner||!v->sink||!pcm||!bytes||bytes%2)return -EINVAL;
 unsigned volume=v->owner->volume;
 if(volume==50)return v->sink(v->arg,pcm,bytes);
 unsigned char scratch[1024];
 for(size_t at=0;at<bytes;){
  size_t n=bytes-at;if(n>sizeof(scratch))n=sizeof(scratch);
  memcpy(scratch,pcm+at,n);volume_pcm(scratch,n,volume);
  int rc=v->sink(v->arg,scratch,n);if(rc)return rc;at+=n;
 }
 return 0;
}
#endif
static int close_capture(void){
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) &&     defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
 return vv_paraformer_k7_close();
#else
 return 0;
#endif
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
struct reply_transport {struct vv_https_mbedtls *tls;bool live;};
static ssize_t reply_read(void*a,void*p,size_t n){
 short wanted=0;ssize_t rc=vv_https_mbedtls_try_read(((struct reply_transport*)a)->tls,p,n,&wanted);
 return rc==-EAGAIN?VV_WSI_BLOCK:rc;
}
static ssize_t reply_write(void*a,const void*p,size_t n){
 short wanted=0;ssize_t rc=vv_https_mbedtls_try_write(((struct reply_transport*)a)->tls,p,n,&wanted);
 return rc==-EAGAIN?VV_WSI_BLOCK:rc;
}
static int reply_random(void*a,uint8_t*p,size_t n){return vv_https_mbedtls_random(((struct reply_transport*)a)->tls,p,n);}
static void reply_close(void*a){struct reply_transport*t=a;if(t->live){t->live=false;vv_https_mbedtls_ops()->close(t->tls);}}
static void reply_sleep(void*a,unsigned ms){(void)a;struct timespec t={ms/1000,(long)(ms%1000)*1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
static uint64_t reply_now(void*a){struct vv_aliyun_provider_owner*o=a;return o->now(o->clock_arg);}
static int reply_chat(void*a,const char*text,char*answer,size_t cap,const struct vv_https_cancel*cancel,
 vv_aliyun_chat_delta_fn delta,void*delta_arg){
 struct vv_aliyun_provider_owner*o=a;
 return vv_aliyun_chat_perform_stream_at_host(&o->chat_https,o->speech_host,o->key,text,answer,cap,cancel,delta,delta_arg);
}
/* Same pipeline producer contract; literal text is already authorized by
 * the serial caller, and still passes pipeline UTF-8/content/cancel checks. */
static int reply_literal(void*a,const char*text,char*answer,size_t cap,
 const struct vv_https_cancel*cancel,vv_aliyun_chat_delta_fn delta,void*delta_arg){
 (void)a;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 size_t n=0;while(n<=VV_REPLY_PIPELINE_TEXT_BYTES&&text[n])n++;
 if(!n||n>VV_REPLY_PIPELINE_TEXT_BYTES||n>=cap)return -E2BIG;
 memcpy(answer,text,n+1);return delta(delta_arg,answer,n);
}
static int reply_tts(void*a,vv_reply_next_text_fn next,void*next_arg,vv_stream_pcm_sink sink,void*sink_arg,
 const struct vv_https_cancel*cancel,struct vv_aliyun_stream_result*out){
 struct vv_aliyun_provider_owner*o=a;memset(out,0,sizeof(*out));
 struct reply_transport t={o->tls,false};char auth[2056];
 int length=snprintf(auth,sizeof(auth),"Bearer %s",o->key);
 if(length<0||(size_t)length>=sizeof(auth)){vv_https_secure_zero(auth,sizeof(auth));return -EINVAL;}
 struct vv_wss_client handshake={.io=vv_https_mbedtls_ops(),.io_arg=o->tls,.now=o->now,.now_arg=o->clock_arg,
  .random=vv_https_mbedtls_random,.random_arg=o->tls,.sha1=vv_https_mbedtls_sha1,.max_message=65536};
 uint64_t began=reply_now(o);int rc=-ETIMEDOUT;
 if(began<=UINT64_MAX-15000)
  rc=vv_wss_open(&handshake,o->speech_host,443,"/api-ws/v1/inference",auth,began+15000,cancel);
 vv_https_secure_zero(auth,sizeof(auth));
 if(!rc){
  t.live=true;handshake.open=false;
  struct vv_tts_ws_config c={.io={reply_read,reply_write,reply_random,reply_close,NULL,&t},
   .now_ms=o->now,.clock_arg=o->clock_arg,.sleep_ms=reply_sleep,.model=o->tts_model,.voice=o->voice};
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
  c.opus=true;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_WIRE_CANCEL
  c.wire_cancel=true;
#endif
  struct volume_sink volume={o,sink,sink_arg};
  rc=vv_tts_ws_run(&c,next,next_arg,volume_sink_pcm,&volume,cancel,out);
 }else out->http_result=rc;
 reply_close(&t);return rc;
}
static int chat_speak_common(void*a,const char*text,char*answer,size_t cap,const struct vv_https_cancel*cancel,
 const struct vv_reply_scope_token *scope,bool literal){
 struct vv_aliyun_provider_owner*o=a;bool idle=false;
 if(answer&&cap)vv_https_secure_zero(answer,cap);
 if(!text||!answer||!cap)return -EINVAL;
 if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=close_capture();
 if(!rc){
  struct vv_reply_pipeline_ops ops={.chat=literal?reply_literal:reply_chat,.now_ms=reply_now,.arg=o,.tts_stream=reply_tts};
  struct vv_reply_pipeline_request request={&ops,text,answer,cap};
  rc=scope?vv_stream_speaker_run_dialogue_scoped(vv_reply_pipeline_fetch,&request,cancel,scope):
    vv_stream_speaker_run_dialogue(vv_reply_pipeline_fetch,&request,cancel);
 }
 if(!rc&&cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))rc=-ECANCELED;
 if(rc)vv_https_secure_zero(answer,cap);
 atomic_store(&o->active,false);return rc;
}
static int chat_speak(void*a,const char*t,char*out,size_t n,const struct vv_https_cancel*c){
 return chat_speak_common(a,t,out,n,c,NULL,false);
}
static int chat_speak_scoped(void*a,const char*t,char*out,size_t n,const struct vv_https_cancel*c,
 const struct vv_reply_scope_token *scope){
 if(!scope){if(out&&n)vv_https_secure_zero(out,n);return -EINVAL;}
 return chat_speak_common(a,t,out,n,c,scope,false);
}
static int speak_literal(void*a,const char*t,char*out,size_t n,const struct vv_https_cancel*c,
 const struct vv_reply_scope_token *scope){
 return chat_speak_common(a,t,out,n,c,scope,true);
}
#endif
static const char *const downloads[]={"dashscope-result-bj.oss-cn-beijing.aliyuncs.com"};
static int setting(char *out,const char *in){
 if(!in||!*in)return -EINVAL;
 size_t n=0;for(;n<129&&in[n];++n){unsigned char c=(unsigned char)in[n];
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==':'))return -EINVAL;}
 if(n>128)return -EINVAL;
 memcpy(out,in,n+1);return 0;
}
static int asr(void *arg,const unsigned char *pcm,size_t size,const struct vv_https_cancel *cancel,struct vv_asr_batch_result *out){
 struct vv_aliyun_provider_owner *o=arg;bool idle=false;
 if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=o->inner.asr(o->inner.arg,pcm,size,cancel,out);
 atomic_store(&o->active,false);return rc;
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_STREAM
static int listen_stream(void *arg,unsigned samples,const struct vv_https_cancel *cancel,struct vv_asr_batch_result *out){
 struct vv_aliyun_provider_owner *o=arg;bool idle=false;
 if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=vv_paraformer_k7_listen_at_host(o->tls,o->speech_host,o->key,o->now,o->clock_arg,samples,cancel,out);
 atomic_store(&o->active,false);return rc;
}
#endif
static int tts(void *arg,const char *text,unsigned char *pcm,size_t cap,size_t *size,const struct vv_https_cancel *cancel){
 struct vv_aliyun_provider_owner *o=arg;bool idle=false;
 if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=close_capture();
 if(!rc)rc=o->inner.tts(o->inner.arg,text,pcm,cap,size,cancel);
 if(!rc&&o->volume!=50){
  if(!pcm||!size||!*size||*size>cap||*size%2){
   if(pcm&&cap)vv_https_secure_zero(pcm,cap);
   if(size)*size=0;
   rc=-EPROTO;
  }else volume_pcm(pcm,*size,o->volume);
 }
 atomic_store(&o->active,false);return rc;
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
static int chat(void *arg,const char *text,char *answer,size_t cap,const struct vv_https_cancel *cancel){
 struct vv_aliyun_provider_owner *o=arg;bool idle=false;
 if(answer&&cap)vv_https_secure_zero(answer,cap);
 if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=vv_aliyun_chat_perform_at_host(&o->https,o->speech_host,o->key,text,answer,cap,cancel);
 atomic_store(&o->active,false);return rc;
}
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
struct speak_request {
 struct vv_aliyun_provider_owner *owner;const char *text;
 const unsigned char *cached;size_t cached_bytes;
#if defined(K7_MICROCRYSTAL) && K7_MICROCRYSTAL
 const struct vv_micro_voice_clip *micro_clip;
#endif
 unsigned char *collect;size_t collected;bool overflow;
 vv_stream_pcm_sink sink;void *sink_arg;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 unsigned char *segment_pcm;
#endif
};
static bool speak_cancelled(void *arg){const struct vv_https_cancel*c=arg;return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static int collect_audio(void *arg,const unsigned char *pcm,size_t n){
 struct speak_request*r=arg;
 int rc=r->sink(r->sink_arg,pcm,n);if(rc)return rc;
 if(r->collect&&!r->overflow){
  if(n>VV_PROMPT_CACHE_ITEM_MAX-r->collected)r->overflow=true;
  else {memcpy(r->collect+r->collected,pcm,n);r->collected+=n;}
 }
 return 0;
}
static int fetch_audio(void *arg,vv_stream_pcm_sink sink,void *sink_arg,const struct vv_https_cancel *cancel){
 struct speak_request*r=arg;
 if(speak_cancelled((void*)cancel))return -ECANCELED;
 struct volume_sink volume={r->owner,sink,sink_arg};
#if defined(K7_MICROCRYSTAL) && K7_MICROCRYSTAL
 if(r->micro_clip){
  struct vv_micro_voice_decode state={0};unsigned char pcm[1280];
  while(state.position<r->micro_clip->samples){
   if(speak_cancelled((void*)cancel))return -ECANCELED;
   size_t used=0;int rc=vv_micro_voice_audio_next(r->micro_clip,vv_micro_voice_levels,&state,pcm,sizeof(pcm),&used);
   if(rc||!used)return rc?rc:-EPROTO;
   rc=volume_sink_pcm(&volume,pcm,used);if(rc)return rc;
  }
  return speak_cancelled((void*)cancel)?-ECANCELED:0;
 }
#endif
 if(r->cached){
  for(size_t at=0;at<r->cached_bytes;){
   if(speak_cancelled((void*)cancel))return -ECANCELED;
   size_t n=r->cached_bytes-at;if(n>8192)n=8192;
   int rc=volume_sink_pcm(&volume,r->cached+at,n);if(rc)return rc;at+=n;
  }
  return speak_cancelled((void*)cancel)?-ECANCELED:0;
 }
 r->sink=volume_sink_pcm;r->sink_arg=&volume;
 return vv_aliyun_client_stream(&r->owner->client,r->text,collect_audio,r,cancel);
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#define VERIFIED_PCM_MAX 960000u
#define VERIFIED_SEGMENT_SCALARS 32u
#define VERIFIED_LOG_MAX 8u
struct verified_batch {
 struct speak_request *request;const struct vv_https_cancel *cancel;
 size_t bytes,committed;uint64_t began,last;unsigned budget;
 int sink_error;bool clock_failed;
 /* Per-attempt observations reuse successful gate timestamps. They never
  * read the clock, change a deadline, or log from a PCM callback. */
 uint64_t first_raw_pcm,last_raw_pcm,max_raw_pcm_gap;unsigned raw_pcm_blocks;
};
static int verified_gate(struct verified_batch *b){
 if(speak_cancelled((void*)b->cancel))return -ECANCELED;
 uint64_t now=b->request->owner->now(b->request->owner->clock_arg);
 if(b->clock_failed||now==UINT64_MAX||now<b->last||now<b->began){
  b->clock_failed=true;return -ETIMEDOUT;
 }
 b->last=now;
 if(now-b->began>=b->budget){b->clock_failed=true;return -ETIMEDOUT;}
 return 0;
}
static bool verified_cancelled(void *arg){return verified_gate(arg)!=0;}
static int verified_collect(void *arg,const unsigned char *pcm,size_t bytes){
 struct verified_batch *b=arg;int rc=verified_gate(b);
 if(!rc&&(!pcm||bytes%2))rc=-EINVAL;
 if(!rc&&(bytes>VERIFIED_PCM_MAX-b->bytes||bytes>VERIFIED_PCM_MAX-b->committed-b->bytes))rc=-EOVERFLOW;
 if(rc){if(!b->sink_error)b->sink_error=rc;return rc;}
 if(bytes){
  memcpy(b->request->segment_pcm+b->bytes,pcm,bytes);
  if(!b->raw_pcm_blocks)b->first_raw_pcm=b->last;
  else if(b->last-b->last_raw_pcm>b->max_raw_pcm_gap)b->max_raw_pcm_gap=b->last-b->last_raw_pcm;
  b->last_raw_pcm=b->last;++b->raw_pcm_blocks;
 }
 b->bytes+=bytes;return 0;
}
/* One old dialogue batch remains one TX generation and at most 480000 samples.
 * Network callbacks only collect private PCM. A complete short segment is
 * published once; the next request overlaps playback through the existing
 * spool worker/caller, not through another TLS or hardware owner. */
static int fetch_verified_audio(struct speak_request *r,vv_stream_pcm_sink sink,void *sink_arg,
 const struct vv_https_cancel *cancel,struct vv_stream_fetch_result *result){
 if(!r->segment_pcm||!r->owner->now)return -EINVAL;
 size_t text_bytes=0;while(text_bytes<=VV_TTS_SEGMENT_BYTES&&r->text[text_bytes])++text_bytes;
 if(text_bytes>VV_TTS_SEGMENT_BYTES)return -E2BIG;
 struct vv_tts_segments cursor;int rc=vv_tts_segments_init(&cursor,r->text,text_bytes);
 if(rc)return -EINVAL;
 struct verified_batch b={.request=r,.cancel=cancel,.budget=r->owner->client.config.total_timeout_ms};
 if(!b.budget||b.budget>90000)b.budget=90000;
 b.began=b.last=r->owner->now(r->owner->clock_arg);
 if(b.began==UINT64_MAX)return -ETIMEDOUT;
 struct vv_https_cancel token={verified_cancelled,&b};
 char segment[VERIFIED_SEGMENT_SCALARS*4+1]={0};
 unsigned segments=0,attempts=0,retries=0,logged=0,lost=0;bool complete=false;
 for(;;){
  if((rc=verified_gate(&b)))break;
  size_t written=0;int next=vv_tts_segments_next_limit(&cursor,segment,sizeof(segment),&written,VERIFIED_SEGMENT_SCALARS);
  if(next==VV_TTS_SEGMENT_DONE){complete=true;rc=0;break;}
  if(next!=VV_TTS_SEGMENT_READY){rc=-EPROTO;break;}
  /* The segmenter already validated UTF-8; count leading bytes only. */
  unsigned scalars=0;for(size_t i=0;i<written;i++)if(((unsigned char)segment[i]&0xc0)!=0x80)++scalars;
  ++segments;
  for(unsigned attempt=0;attempt<2;attempt++){
   if((rc=verified_gate(&b)))break;
   b.bytes=0;b.sink_error=0;uint64_t began=b.last;struct vv_aliyun_stream_result checked={0};
   b.first_raw_pcm=b.last_raw_pcm=b.max_raw_pcm_gap=0;b.raw_pcm_blocks=0;
   uint64_t client_done=0,publication_enter=0;bool client_done_valid=false,publication_entered=false,published=false;
   ++attempts;
   rc=vv_aliyun_client_stream_ex(&r->owner->client,segment,verified_collect,&b,&token,&checked);
   int gate=verified_gate(&b);
   if(!gate){client_done=b.last;client_done_valid=true;}
   if(gate)rc=gate;else if(b.sink_error)rc=b.sink_error;
   bool exact=b.bytes&&!(b.bytes%2)&&checked.pcm_bytes==b.bytes;
   bool good=!rc&&!checked.http_result&&checked.terminal_audio_complete&&exact;
   bool early=rc==VV_HTTPS_EPROTO&&checked.http_result==rc&&checked.validated_prefix&&exact&&!b.sink_error;
   if(!rc&&!good)rc=-EPROTO;
   bool retry=early&&!attempt;
   if(good){
    /* Cancellation/time is rechecked immediately before publication. The
     * append sink copies synchronously, so this private buffer can be reused. */
    rc=verified_gate(&b);
    if(!rc){publication_enter=b.last;publication_entered=true;rc=sink(sink_arg,r->segment_pcm,b.bytes);}
    if(!rc){b.committed+=b.bytes;published=true;}
    retry=false;
   }
   result->http_result=checked.http_result;
   result->pcm_bytes=b.committed;
   /* Only the existing proven transport-EOF policy can drain old committed
    * segments on failure. Other errors retain spool cancellation semantics.
    * This flag never validates bytes from the failed, unpublished segment. */
   result->validated_prefix=early&&rc==VV_HTTPS_EPROTO&&b.committed>0;
   if(logged<VERIFIED_LOG_MAX){
    printf("TTS_SEGMENT index=%u attempt=%u bytes=%zu committed=%zu elapsed_ms=%llu rc=%d http_rc=%d terminal=%u retry=%u\n",
     segments,attempt+1,b.bytes,b.committed,(unsigned long long)(b.last>=began?b.last-began:0),
     rc,checked.http_result,(unsigned)checked.terminal_audio_complete,(unsigned)retry);
    /* Milliseconds are relative to this batch. Raw PCM means decoded, even
     * PCM admitted to private scratch, not TLS/socket bytes. client_done is
     * observed by the existing post-return gate; cancellation/bad clocks can
     * leave it unavailable. Publication is sink entry, not sink return/DAC.
     * pcm_ms is floor(samples/16) for fixed 16kHz mono S16 PCM. */
    printf("TTS_SEGMENT_TIMING index=%u attempt=%u scalars=%u request_begin_ms=%llu first_raw_pcm_ms=%llu last_raw_pcm_ms=%llu raw_pcm_blocks=%u max_raw_pcm_gap_ms=%llu client_done_ms=%llu client_done_valid=%u publication_enter_ms=%llu publication_entered=%u published=%u pcm_samples=%zu pcm_ms=%zu\n",
     segments,attempt+1,scalars,(unsigned long long)(began-b.began),
     (unsigned long long)(b.raw_pcm_blocks?b.first_raw_pcm-b.began:0),
     (unsigned long long)(b.raw_pcm_blocks?b.last_raw_pcm-b.began:0),b.raw_pcm_blocks,
     (unsigned long long)b.max_raw_pcm_gap,(unsigned long long)(client_done_valid?client_done-b.began:0),
     (unsigned)client_done_valid,(unsigned long long)(publication_entered?publication_enter-b.began:0),
     (unsigned)publication_entered,(unsigned)published,b.bytes/2,b.bytes/32);++logged;
   }else ++lost;
   vv_https_secure_zero(r->segment_pcm,b.bytes);b.bytes=0;
   if(retry){++retries;continue;}
   break;
  }
  vv_https_secure_zero(segment,sizeof(segment));
  if(rc)break;
 }
 if(!rc)rc=verified_gate(&b);
 result->terminal_audio_complete=complete&&!rc&&b.committed>0;
 if(!rc){result->http_result=0;result->validated_prefix=false;}
 else if(rc!=VV_HTTPS_EPROTO)result->validated_prefix=false;
 result->pcm_bytes=b.committed;
 vv_https_secure_zero(segment,sizeof(segment));
 printf("TTS_SEGMENTS segments=%u attempts=%u retries=%u committed=%zu elapsed_ms=%llu logs_lost=%u rc=%d complete=%u\n",
  segments,attempts,retries,b.committed,(unsigned long long)(b.last>=b.began?b.last-b.began:0),lost,rc,
  (unsigned)result->terminal_audio_complete);
 return rc;
}
#endif
static int fetch_audio_ex(void *arg,vv_stream_pcm_sink sink,void *sink_arg,
 const struct vv_https_cancel *cancel,struct vv_stream_fetch_result *result){
 memset(result,0,sizeof(*result));struct speak_request*r=arg;
 if(speak_cancelled((void*)cancel))return -ECANCELED;
 /* Only the real-cloud entry can reach this adapter. No cache/prompt may
  * borrow a previous request's integrity flag or consume the capture arm. */
 if(r->cached||vv_prompt_cache_eligible(r->text))return -EINVAL;
 struct volume_sink volume={r->owner,sink,sink_arg};
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 return fetch_verified_audio(r,volume_sink_pcm,&volume,cancel,result);
#else
 r->sink=volume_sink_pcm;r->sink_arg=&volume;struct vv_aliyun_stream_result checked={0};
 int rc=vv_aliyun_client_stream_ex(&r->owner->client,r->text,collect_audio,r,cancel,&checked);
 result->http_result=checked.http_result;result->terminal_audio_complete=checked.terminal_audio_complete;
 result->validated_prefix=checked.validated_prefix;
 result->pcm_bytes=checked.pcm_bytes;return rc;
#endif
}
/* Fixed control acknowledgements are immutable local PCM in both voices.
 * Their existing cached-speaker path still applies volume exactly once. */
static int control_ack_view(bool firefly,const char *text,
 const unsigned char **pcm,size_t *bytes){
 const struct vv_control_ack_asset *assets=firefly?vv_control_ack_firefly_assets:vv_control_ack_default_assets;
 size_t count=firefly?sizeof(vv_control_ack_firefly_assets)/sizeof(*assets):sizeof(vv_control_ack_default_assets)/sizeof(*assets);
 for(size_t i=0;i<count;i++)if(!strcmp(text,assets[i].text)){
  *pcm=assets[i].pcm;*bytes=assets[i].bytes;return 1;
 }
 return 0;
}
#if defined(K7_REAL_SHUIGUANG_PROFILE) && K7_REAL_SHUIGUANG_PROFILE
/* Explicit failures use immutable PCM, so an unavailable network cannot
 * prevent the failure notice. Unknown text keeps the normal TTS path. */
static int real_failure_view(bool firefly,const char *text,
 const unsigned char **pcm,size_t *bytes){
 const struct vv_real_failure_asset *assets=firefly?vv_real_failure_firefly_assets:vv_real_failure_default_assets;
 size_t count=firefly?sizeof(vv_real_failure_firefly_assets)/sizeof(*assets):sizeof(vv_real_failure_default_assets)/sizeof(*assets);
 for(size_t i=0;i<count;i++)if(!strcmp(text,assets[i].text)){
  *pcm=assets[i].pcm;*bytes=assets[i].bytes;return 1;
 }
 return 0;
}
#endif
static int speak(void *arg,const char *text,const struct vv_https_cancel *cancel){
 struct vv_aliyun_provider_owner*o=arg;bool idle=false;
 if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 struct speak_request request={.owner=o,.text=text};
 int rc=close_capture(),hit=0,stored=0;
 if(rc)goto done;
 if(!text){rc=-EINVAL;goto done;}
 if(speak_cancelled((void*)cancel)){rc=-ECANCELED;goto done;}
#if defined(K7_LOCAL_ASSESSMENT) && K7_LOCAL_ASSESSMENT
#if defined(K7_MICROCRYSTAL) && K7_MICROCRYSTAL
 const struct vv_micro_voice_clip *clips=o->firefly_voice?vv_micro_voice_firefly_clips:vv_micro_voice_default_clips;
 for(unsigned i=0;i<4;i++)if(!strcmp(text,clips[i].text)){request.micro_clip=&clips[i];break;}
 if(request.micro_clip){
  request.cached_bytes=request.micro_clip->samples*4;hit=1;
  printf("MICRO_MANUAL cue_local voice=%u text=%s bytes=%zu network=0\n",(unsigned)o->firefly_voice,text,request.cached_bytes);
 }else
 if(!strcmp(text,"正在准备护理方案，请稍候。")){
  const struct vv_microcrystal_cue_asset *asset=o->firefly_voice?vv_microcrystal_cue_firefly_assets:vv_microcrystal_cue_default_assets;
  request.cached=asset[0].pcm;request.cached_bytes=asset[0].bytes;hit=1;
  printf("MICRO cue_local voice=%u bytes=%zu network=0\n",(unsigned)o->firefly_voice,request.cached_bytes);
 }else
#endif
 if(!strcmp(text,"三视图采集成功")){
  const struct vv_capture_success_asset *asset=o->firefly_voice?vv_capture_success_firefly_assets:vv_capture_success_default_assets;
  request.cached=asset[0].pcm;request.cached_bytes=asset[0].bytes;
  hit=1;
  printf("CAPTURE_SUCCESS_LOCAL voice=%u bytes=%zu network=0\n",(unsigned)o->firefly_voice,request.cached_bytes);
 }else
#endif
#if defined(K7_REAL_SHUIGUANG_PROFILE) && K7_REAL_SHUIGUANG_PROFILE
 if(!strcmp(text,"正在生成报告，请稍候。")){
  const struct vv_report_wait_asset *asset=o->firefly_voice?vv_report_wait_firefly_assets:vv_report_wait_default_assets;
  request.cached=asset[0].pcm;request.cached_bytes=asset[0].bytes;hit=1;
  printf("REPORT_WAIT_LOCAL voice=%u bytes=%zu network=0\n",(unsigned)o->firefly_voice,request.cached_bytes);
 }else
 if(real_failure_view(o->firefly_voice,text,&request.cached,&request.cached_bytes)){
  hit=1;
  printf("REAL_FAILURE_LOCAL voice=%u bytes=%zu network=0\n",(unsigned)o->firefly_voice,request.cached_bytes);
 }else
#endif
 if(control_ack_view(o->firefly_voice,text,&request.cached,&request.cached_bytes)){
  hit=1;
  printf("CONTROL_ACK_LOCAL voice=%u bytes=%zu network=0\n",(unsigned)o->firefly_voice,request.cached_bytes);
 }else
#if defined(K7_REWAKE_PROMPT) && K7_REWAKE_PROMPT
 if(!strcmp(text,VV_REWAKE_TEXT)){
  request.cached=o->firefly_voice?vv_rewake_firefly:vv_rewake_default;
  request.cached_bytes=o->firefly_voice?sizeof(vv_rewake_firefly):sizeof(vv_rewake_default);
  hit=1;
  printf("LOCAL_REWAKE voice=%u bytes=%zu network=0\n",(unsigned)o->firefly_voice,request.cached_bytes);
 }else
#endif
#if defined(K7_READY_CUE) && K7_READY_CUE
 if(!strcmp(text,VV_READY_TEXT)){
  request.cached=o->firefly_voice?vv_ready_firefly:vv_ready_default;
  request.cached_bytes=o->firefly_voice?sizeof(vv_ready_firefly):sizeof(vv_ready_default);
  hit=1;
 }else
#endif
 if(!strcmp(text,VV_LOCAL_REPORT_TEXT)){
  request.cached=o->firefly_voice?vv_local_report_firefly:vv_local_report_default;
  request.cached_bytes=o->firefly_voice?sizeof(vv_local_report_firefly):sizeof(vv_local_report_default);
  hit=1;
  printf("LOCAL_REPORT voice=%u bytes=%zu network=0\n",(unsigned)o->firefly_voice,request.cached_bytes);
 }else hit=vv_prompt_cache_view(&o->prompts,text,&request.cached,&request.cached_bytes);
 if(hit<0){rc=hit;goto done;}
 if(!hit&&vv_prompt_cache_eligible(text))request.collect=malloc(VV_PROMPT_CACHE_ITEM_MAX);
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 if(!hit&&!vv_prompt_cache_eligible(text)){
  request.segment_pcm=malloc(VERIFIED_PCM_MAX);
  if(!request.segment_pcm){rc=-ENOMEM;goto done;}
 }
#endif
 rc=(!hit&&!vv_prompt_cache_eligible(text))?
  vv_stream_speaker_run_real_tts_ex(fetch_audio_ex,&request,cancel):
  vv_stream_speaker_run(fetch_audio,&request,cancel);
 if(!rc&&speak_cancelled((void*)cancel))rc=-ECANCELED;
 if(!rc&&request.collect&&!request.overflow&&request.collected){
  stored=vv_prompt_cache_put(&o->prompts,text,request.collect,request.collected,speak_cancelled,(void*)cancel);
  if(stored<0)rc=stored;
 }
done:
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 if(request.segment_pcm){vv_https_secure_zero(request.segment_pcm,VERIFIED_PCM_MAX);free(request.segment_pcm);}
#endif
 if(request.collect){vv_https_secure_zero(request.collect,VV_PROMPT_CACHE_ITEM_MAX);free(request.collect);}
 printf("TTS_PROMPT_CACHE hit=%d stored=%d bytes=%zu result=%d\n",hit,stored,hit?request.cached_bytes:request.collected,rc);
 atomic_store(&o->active,false);return rc;
}
#endif
#if defined(K7_REAL_SHUIGUANG_PROFILE) && K7_REAL_SHUIGUANG_PROFILE && defined(CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM) && defined(CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
struct report_wss_request {
 struct vv_aliyun_provider_owner *owner;
 const char *text; size_t bytes,offset,attempted,accepted;
 vv_stream_pcm_sink sink; void *sink_arg;
 uint64_t began,last,first_attempt,first_accepted;
 int clock_error;
};
static int report_wss_clock(struct report_wss_request *r,uint64_t *value){
 uint64_t now=r->owner->now(r->owner->clock_arg);
 if(!now||now==UINT64_MAX||now<r->last||now-r->began>=90000u)r->clock_error=-ETIMEDOUT;
 if(r->clock_error)return r->clock_error;
 r->last=now;if(value)*value=now;return 0;
}
static int report_wss_next(void *arg,char *out,size_t cap,size_t *bytes){
 struct report_wss_request *r=arg;
 if(bytes)*bytes=0;
 if(!out||!bytes||cap<2)return -EINVAL;
 out[0]=0;int rc=report_wss_clock(r,NULL);if(rc)return rc;
 if(r->offset==r->bytes)return VV_REPLY_TEXT_DONE;
 size_t n=r->bytes-r->offset;
 if(n>VV_REPLY_PIPELINE_SEGMENT_BYTES)n=VV_REPLY_PIPELINE_SEGMENT_BYTES;
 if(n>=cap)n=cap-1;
 if(n<r->bytes-r->offset){while(n&&(((unsigned char)r->text[r->offset+n]&0xc0u)==0x80u))--n;}
 if(!n)return -EOVERFLOW;
 memcpy(out,r->text+r->offset,n);out[n]=0;r->offset+=n;*bytes=n;
 return VV_REPLY_TEXT_READY;
}
static int report_wss_pcm(void *arg,const unsigned char *pcm,size_t bytes){
 struct report_wss_request *r=arg;
 if(!pcm||!bytes||bytes%2)return -EPROTO;
 uint64_t now=0;int rc=report_wss_clock(r,&now);if(rc)return rc;
 if(bytes>960000u-r->attempted)return -EOVERFLOW;
 /* Count before sink entry: even a failing sink may partially publish. */
 if(!r->attempted)r->first_attempt=now;
 r->attempted+=bytes;
 rc=r->sink(r->sink_arg,pcm,bytes);
 if(!rc){if(!r->accepted)r->first_accepted=now;r->accepted+=bytes;}
 return rc;
}
static int report_wss_fetch(void *arg,vv_stream_pcm_sink sink,void *sink_arg,const struct vv_https_cancel *cancel,struct vv_stream_fetch_result *out){
 struct report_wss_request *r=arg;memset(out,0,sizeof(*out));r->sink=sink;r->sink_arg=sink_arg;
 struct vv_aliyun_stream_result checked={0};
 int rc=report_wss_clock(r,NULL);
 if(!rc)rc=reply_tts(r->owner,report_wss_next,r,report_wss_pcm,r,cancel,&checked);
 int clock_rc=report_wss_clock(r,NULL);if(clock_rc)rc=clock_rc;
 if(!rc&&(r->offset!=r->bytes||!r->accepted||r->accepted!=r->attempted||checked.pcm_bytes!=r->accepted||!checked.terminal_audio_complete||checked.http_result))rc=-EPROTO;
 out->http_result=rc?rc:checked.http_result;
 out->pcm_bytes=r->accepted;
 out->validated_prefix=false;
 out->terminal_audio_complete=!rc&&checked.terminal_audio_complete;
 return rc;
}
#endif
int vv_aliyun_provider_speak_report(const struct vv_speech_provider *s,const char *text,const struct vv_https_cancel *cancel,size_t *attempted){
 if(attempted)*attempted=0;
 if(!s||!text||!attempted||!cancel||!cancel->is_cancelled)return -EINVAL;
#if defined(K7_REAL_SHUIGUANG_PROFILE) && K7_REAL_SHUIGUANG_PROFILE && defined(CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM) && defined(CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM) && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 if(s->asr!=asr||s->tts!=tts||s->speak!=speak||!s->arg)return -ENOTSUP;
 struct vv_aliyun_provider_owner *o=s->arg;
 if(o->exposed.arg!=o||o->exposed.asr!=s->asr||o->exposed.tts!=s->tts||o->exposed.speak!=s->speak||!o->now)return -EACCES;
 size_t bytes=0;bool content=false;
 int rc=vv_chat_text_measure(text,512u,&bytes,NULL,&content);
 if(rc)return rc;
 if(!bytes||!content)return -EINVAL;
 if(cancel->is_cancelled(cancel->arg))return -ECANCELED;
 bool idle=false;if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 struct report_wss_request r={.owner=o,.text=text,.bytes=bytes};
 r.began=r.last=o->now(o->clock_arg);
 if(!r.began||r.began==UINT64_MAX)rc=-ETIMEDOUT;
 if(!rc)rc=close_capture();
 if(!rc)rc=vv_stream_speaker_run_real_tts_ex(report_wss_fetch,&r,cancel);
 /* The synchronous speaker retires its borrowed fetch before returning. */
 *attempted=r.attempted;
 if(!rc&&cancel->is_cancelled(cancel->arg))rc=-ECANCELED;
 if(!rc&&(!r.accepted||r.accepted!=r.attempted))rc=-EPROTO;
 printf("REPORT_WSS start_tick_ms=%llu first_pcm_tick_ms=%llu rc=%d attempted=%zu accepted=%zu first_attempt_ms=%llu first_accepted_ms=%llu raw_timing_available=0\n",(unsigned long long)r.began,(unsigned long long)r.first_attempt,rc,r.attempted,r.accepted,(unsigned long long)(r.first_attempt?r.first_attempt-r.began:0),(unsigned long long)(r.first_accepted?r.first_accepted-r.began:0));
 atomic_store(&o->active,false);return rc;
#else
 return -ENOTSUP;
#endif
}

int vv_aliyun_provider_warmup_next(struct vv_aliyun_provider_owner *o,
 const struct vv_https_cancel *cancel,unsigned *attempted){
 if(!o||!attempted)return -EINVAL;
#ifndef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
 (void)cancel;*attempted=0;return -ENOSYS;
#else
 static const char *const phrases[]={"配网成功。","三视图上传成功，正在生成报告。",
  "OpenVela 云台已启动。","开始测肤，请正对镜头。",
  "请向您的左侧缓慢转头，角度合适时会自动拍照。",
  "请向您的右侧缓慢转头，角度合适时会自动拍照。"};
 bool idle=false;if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=0;unsigned char *pcm=NULL;size_t bytes=0;const unsigned char *cached=NULL;
 *attempted=o->warmup_attempted;
 if(speak_cancelled((void*)cancel)){rc=-ECANCELED;goto done;}
 if(o->warmup_attempted>=sizeof(phrases)/sizeof(*phrases)){rc=1;goto done;}
 const char *text=phrases[o->warmup_attempted++];*attempted=o->warmup_attempted;
 if(vv_prompt_cache_view(&o->prompts,text,&cached,&bytes)==1)goto done;
 pcm=malloc(VV_PROMPT_CACHE_ITEM_MAX);if(!pcm){rc=-ENOMEM;goto done;}
 rc=o->inner.tts(o->inner.arg,text,pcm,VV_PROMPT_CACHE_ITEM_MAX,&bytes,cancel);
 if(!rc&&speak_cancelled((void*)cancel))rc=-ECANCELED;
 if(!rc&&(!bytes||bytes>VV_PROMPT_CACHE_ITEM_MAX||bytes%2))rc=-EPROTO;
 if(!rc){int saved=vv_prompt_cache_put(&o->prompts,text,pcm,bytes,speak_cancelled,(void*)cancel);if(saved<0)rc=saved;else if(!saved)rc=-ENOSPC;}
done:
 if(pcm){vv_https_secure_zero(pcm,VV_PROMPT_CACHE_ITEM_MAX);free(pcm);}
 printf("TTS_PROMPT_WARMUP attempted=%u bytes=%zu result=%d\n",*attempted,bytes,rc);
 atomic_store(&o->active,false);return rc;
#endif
}
int vv_aliyun_provider_create(const struct vv_aliyun_provider_config *c,struct vv_aliyun_provider_owner **out){
 if(!out){printf("ALIYUN_PROVIDER create_stage=1 result=%d\n",-EINVAL);return -EINVAL;}
 *out=NULL;
 if(!c||!c->load_key||!c->tls.security_ready||!c->tls.now_ms||
  !c->limits.max_request_header_bytes||!c->limits.max_response_header_bytes||
  !c->limits.max_response_body_bytes||!c->limits.max_chunk_bytes||
  !c->limits.total_timeout_ms||c->limits.total_timeout_ms>120000){printf("ALIYUN_PROVIDER create_stage=1 result=%d\n",-EINVAL);return -EINVAL;}
 const char *host=c->speech_host?c->speech_host:VV_ALIYUN_LEGACY_HOST;
 if(!vv_aliyun_speech_host_valid(host)){printf("ALIYUN_PROVIDER create_stage=1 result=%d\n",-EINVAL);return -EINVAL;}
 if(!c->tls.security_ready(c->tls.security_arg)){printf("ALIYUN_PROVIDER create_stage=2 result=%d\n",-EACCES);return -EACCES;}
 struct vv_aliyun_provider_owner *o=calloc(1,sizeof(*o));if(!o){printf("ALIYUN_PROVIDER create_stage=3 result=%d\n",-ENOMEM);return -ENOMEM;}
 atomic_init(&o->active,false);
 memcpy(o->speech_host,host,strlen(host)+1);
 int stage=4;int rc=setting(o->asr_model,c->asr_model);if(!rc)rc=setting(o->tts_model,c->tts_model);if(!rc)rc=setting(o->voice,c->voice);if(rc)goto fail;
 memcpy(o->default_voice,o->voice,strlen(o->voice)+1);o->volume=50;
 stage=5;o->tls=vv_https_mbedtls_create(&c->tls);if(!o->tls){rc=-EACCES;goto fail;}
 o->https=(struct vv_https_client){.transport=vv_https_mbedtls_ops(),.transport_ctx=o->tls,
  .now_ms=c->tls.now_ms,.now_arg=c->tls.now_arg,.limits=c->limits};
 stage=6;rc=c->load_key(c->key_arg,o->key,sizeof(o->key));if(rc){if(rc>0)rc=-EIO;goto fail;}
 if(!memchr(o->key,0,sizeof(o->key))){rc=-EINVAL;goto fail;}
 struct vv_aliyun_config q={.https=&o->https,.host=o->speech_host,
  .asr_path="/compatible-mode/v1/chat/completions",
  .tts_path="/api/v1/services/audio/tts/SpeechSynthesizer",.api_key=o->key,
  .asr_model=o->asr_model,.tts_model=o->tts_model,.voice=o->voice,
  .download_hosts=downloads,.download_host_count=1,
  .total_timeout_ms=c->limits.total_timeout_ms,.allow_proven_bj_compat=c->allow_proven_bj_compat};
 stage=7;rc=vv_aliyun_client_init(&o->client,&q);if(rc)goto fail;
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
 stage=9;o->chat_tls=vv_https_mbedtls_create(&c->tls);if(!o->chat_tls){rc=-EACCES;goto fail;}
 o->chat_https=o->https;o->chat_https.transport_ctx=o->chat_tls;
#endif
 stage=8;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
 rc=vv_prompt_cache_seed(&o->prompts,c->prompt_assets,c->prompt_asset_count);if(rc)goto fail;
 o->default_prompt_assets=c->prompt_assets;o->default_prompt_asset_count=c->prompt_asset_count;
#else
 if(c->prompt_asset_count){rc=-ENOTSUP;goto fail;}
#endif
 o->inner=vv_aliyun_client_provider(&o->client);
 o->exposed=(struct vv_speech_provider){.asr=asr,.tts=tts,.arg=o};
#ifdef CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT
 o->exposed.chat=chat;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
 o->exposed.chat_speak=chat_speak;
 o->exposed.chat_speak_scoped=chat_speak_scoped;
 o->exposed.speak_literal=speak_literal;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
 o->exposed.speak=speak;
#endif
 o->now=c->tls.now_ms;o->clock_arg=c->tls.now_arg;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_STREAM
 o->exposed.listen=listen_stream;
#endif
 *out=o;return 0;
fail:
 printf("ALIYUN_PROVIDER create_stage=%d result=%d\n",stage,rc);
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
 if(o->chat_tls)vv_https_mbedtls_destroy(o->chat_tls);
#endif
 if(o->tls)vv_https_mbedtls_destroy(o->tls);
 vv_https_secure_zero(o,sizeof(*o));free(o);return rc;
}
/* Product controls accept only this exact live descriptor. The platform
 * retains its borrowed lifetime and serializes settings with completed input.
 * No generic provider descriptor/ABI or in-flight TTS settings are mutated. */
static struct vv_aliyun_provider_owner *settings_owner(const struct vv_speech_provider *s){
 if(!s||s->asr!=asr||s->tts!=tts||!s->arg)return NULL;
 struct vv_aliyun_provider_owner *o=s->arg;
 return s==&o->exposed?o:NULL;
}
static bool setting_cancelled(const struct vv_https_cancel *c){
 return c&&c->is_cancelled&&c->is_cancelled(c->arg);
}
int vv_aliyun_provider_set_volume(const struct vv_speech_provider *s,int value,
 bool relative,const struct vv_https_cancel *cancel,unsigned *applied){
 if(applied)*applied=0;
 if(!applied||(!relative&&(value<1||value>100))||
    (relative&&(value< -100||value>100)))return -EINVAL;
 struct vv_aliyun_provider_owner *o=settings_owner(s);if(!o)return -ENOTSUP;
 if(setting_cancelled(cancel))return -ECANCELED;
 bool idle=false;if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=0;
 if(setting_cancelled(cancel)){rc=-ECANCELED;goto done;}
 int wanted=relative?(int)o->volume+value:value;
 if(wanted<1)wanted=1;else if(wanted>100)wanted=100;
 o->volume=(unsigned)wanted;*applied=o->volume;
 printf("SPEECH_SETTING volume=%u result=0\n",o->volume);
done:
 atomic_store(&o->active,false);return rc;
}
static int select_voice(const struct vv_speech_provider *s,int mode,
 const struct vv_https_cancel *cancel,bool *applied){
 if(applied)*applied=false;
 struct vv_aliyun_provider_owner *o=settings_owner(s);if(!o)return -ENOTSUP;
 if(setting_cancelled(cancel))return -ECANCELED;
 bool idle=false;if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int rc=0;
 if(setting_cancelled(cancel)){rc=-ECANCELED;goto done;}
 bool firefly=mode<0?!o->firefly_voice:mode!=0;
 if(firefly&&strcmp(o->tts_model,"qwen-audio-3.0-tts-flash")){rc=-ENOTSUP;goto done;}
 if(o->firefly_voice==firefly){if(applied)*applied=firefly;goto done;}
 const char *voice=firefly?"qwen-audio-3.0-tts-flash-fireflycn-ec57cdbd4ff849029347050720cf97d4":o->default_voice;
 /* Validate the selected immutable bank before committing any state.
  * Both banks are borrowed for the owner lifetime; only old dynamic entries
  * are released. Switching back restores the original seeded PCM exactly. */
 char selected_voice[sizeof(o->voice)]={0};
 rc=setting(selected_voice,voice);if(rc)goto done;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
 struct vv_prompt_cache next={0};size_t assets=0;
 if(!firefly){
  assets=o->default_prompt_asset_count;
  rc=vv_prompt_cache_seed(&next,o->default_prompt_assets,assets);
 }
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 else {
  if(strcmp(o->tts_model,vv_firefly_prompt_model)||strcmp(selected_voice,vv_firefly_prompt_voice)){rc=-EINVAL;goto done;}
  assets=sizeof(vv_firefly_prompt_assets)/sizeof(vv_firefly_prompt_assets[0]);
  rc=vv_prompt_cache_seed(&next,vv_firefly_prompt_assets,assets);
 }
#endif
 if(rc)goto done;
#endif
 if(setting_cancelled(cancel)){rc=-ECANCELED;goto done;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
 struct vv_prompt_cache old=o->prompts;o->prompts=next;
 vv_prompt_cache_clear(&old);
 o->warmup_attempted=0;
 printf("TTS_PROMPT_BANK voice=%s local_assets=%zu bytes=%zu\n",firefly?"firefly":"default",assets,o->prompts.bytes);
#endif
 memcpy(o->voice,selected_voice,sizeof(o->voice));
 o->firefly_voice=firefly;if(applied)*applied=firefly;
 printf("SPEECH_SETTING voice=%s result=0\n",firefly?"firefly":"default");
done:
 atomic_store(&o->active,false);return rc;
}
int vv_aliyun_provider_set_voice(const struct vv_speech_provider *s,bool firefly,
 const struct vv_https_cancel *cancel){
 return select_voice(s,firefly?1:0,cancel,NULL);
}
int vv_aliyun_provider_toggle_voice(const struct vv_speech_provider *s,
 const struct vv_https_cancel *cancel,bool *applied){
 if(!applied)return -EINVAL;
 return select_voice(s,-1,cancel,applied);
}

const struct vv_speech_provider *vv_aliyun_provider_speech(struct vv_aliyun_provider_owner *o){return o?&o->exposed:NULL;}
int vv_aliyun_provider_destroy(struct vv_aliyun_provider_owner *o){
 if(!o)return 0;
 bool idle=false;if(!atomic_compare_exchange_strong(&o->active,&idle,true))return -EBUSY;
 int cleanup=close_capture();
 if(cleanup){atomic_store(&o->active,false);return cleanup;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
 vv_prompt_cache_clear(&o->prompts);
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
 vv_https_mbedtls_destroy(o->chat_tls);
#endif
 vv_https_mbedtls_destroy(o->tls);vv_https_secure_zero(o,sizeof(*o));free(o);return 0;
}
