#include "vv_aliyun_client.h"
#include "vv_aliyun_protocol.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
#include "vv_aliyun_tts_stream.h"
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ASR_BODY_CAP 129024u
#define WAV_MAX 3000000u
struct guard {struct vv_aliyun_client*c;const struct vv_https_cancel*outer;uint64_t start,last;bool user,timeout;};
static bool stop(void*a){struct guard*g=a;if(g->outer&&g->outer->is_cancelled&&g->outer->is_cancelled(g->outer->arg))g->user=true;uint64_t now=g->c->config.https->now_ms(g->c->config.https->now_arg);if(now<g->last||now-g->start>=g->c->config.total_timeout_ms)g->timeout=true;g->last=now;return g->user||g->timeout;}
static int gate(struct guard*g){stop(g);return g->user?-ECANCELED:g->timeout?-ETIMEDOUT:0;}
static bool host_ok(const char*s){if(!s||!*s||strlen(s)>128)return false;for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='.'||*s=='-'))return false;return true;}
static bool path_ok(const char*s){if(!s||*s!='/'||strlen(s)>255)return false;for(;*s;s++)if((unsigned char)*s<=32||(unsigned char)*s>=127||*s=='#'||*s=='?'||*s=='\\')return false;return true;}
static bool setting(const char*s){if(!s||!*s||strlen(s)>128)return false;for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'||*s=='.'||*s==':'))return false;return true;}
int vv_aliyun_client_init(struct vv_aliyun_client*c,const struct vv_aliyun_config*q){if(!c||!q||!q->https||!q->https->transport||!q->https->now_ms||!q->https->limits.max_request_header_bytes||!q->https->limits.max_response_header_bytes||!q->https->limits.max_response_body_bytes||!q->https->limits.max_chunk_bytes||!q->https->limits.total_timeout_ms||!host_ok(q->host)||!path_ok(q->asr_path)||!path_ok(q->tts_path)||!q->api_key||!*q->api_key||strlen(q->api_key)>2048||!setting(q->asr_model)||!setting(q->tts_model)||!setting(q->voice)||(q->download_host_count&&!q->download_hosts)||q->download_host_count>16||!q->total_timeout_ms||q->total_timeout_ms>120000)return -EINVAL;for(const char*p=q->api_key;*p;p++)if((unsigned char)*p<=32||(unsigned char)*p>=127)return -EINVAL;for(size_t i=0;i<q->download_host_count;i++)if(!host_ok(q->download_hosts[i]))return -EINVAL;memset(c,0,sizeof(*c));c->config=*q;atomic_init(&c->busy,false);return 0;}
struct bytes {unsigned char*p;size_t n,cap;};
static ssize_t read_body(void*a,size_t off,void*dst,size_t cap){struct bytes*b=a;if(off>b->n)return -1;size_t n=b->n-off;if(n>cap)n=cap;if(n)memcpy(dst,b->p+off,n);return (ssize_t)n;}
static int sink(void*a,const void*p,size_t n){struct bytes*b=a;if(n>b->cap-b->n)return -EOVERFLOW;if(n)memcpy(b->p+b->n,p,n);b->n+=n;return 0;}
static bool mime(const char*s,const char*want){while(*want){unsigned char a=(unsigned char)*s++,b=(unsigned char)*want++;if(a>='A'&&a<='Z')a+=32;if(a!=b)return false;}return !*s||*s==';';}
static int request(struct guard*g,const char*host,const char*path,struct bytes*body,struct bytes*response,struct vv_https_response*meta){int rc=gate(g);if(rc)return rc;char auth[2056]={0};struct vv_https_header h[2];struct vv_https_request r={.method=body?"POST":"GET",.host=host,.port=443,.path=path};if(body){size_t n=strlen(g->c->config.api_key);memcpy(auth,"Bearer ",7);memcpy(auth+7,g->c->config.api_key,n);h[0]=(struct vv_https_header){"Authorization",auth,true};h[1]=(struct vv_https_header){"Content-Type","application/json",false};r.headers=h;r.header_count=2;r.body_length=body->n;r.body_read=read_body;r.body_arg=body;}
 struct vv_https_client http=*g->c->config.https;if(http.limits.max_response_body_bytes>response->cap)http.limits.max_response_body_bytes=response->cap;uint64_t left=g->c->config.total_timeout_ms-(g->last-g->start);if(!http.limits.total_timeout_ms||http.limits.total_timeout_ms>left)http.limits.total_timeout_ms=(uint32_t)left;struct vv_https_cancel cancel={stop,g};rc=vv_https_perform(&http,&r,sink,response,&cancel,meta);vv_https_secure_zero(auth,sizeof(auth));int end=gate(g);if(end)return end;if(rc)return rc;if(meta->status_code!=200)return -EPROTO;return 0;}
static int enter(struct vv_aliyun_client*c,struct guard*g,const struct vv_https_cancel*cancel){if(!c)return -EINVAL;bool no=false;if(!atomic_compare_exchange_strong(&c->busy,&no,true))return -EBUSY;uint64_t now=c->config.https->now_ms(c->config.https->now_arg);if(now==UINT64_MAX){atomic_store(&c->busy,false);return -ETIMEDOUT;}*g=(struct guard){c,cancel,now,now,false,false};return 0;}
static int asr(void*a,const unsigned char*pcm,size_t n,const struct vv_https_cancel*cancel,struct vv_asr_batch_result*out){if(!out||!pcm||!n||n%2||n>VV_ALIYUN_ASR_PCM_MAX)return -EINVAL;struct vv_aliyun_client*c=a;struct guard g;int rc=enter(c,&g,cancel);if(rc)return rc;memset(out,0,sizeof(*out));unsigned char*body=NULL,*reply=NULL;size_t len=0;struct vv_https_response meta={0};if((rc=gate(&g)))goto done;body=malloc(ASR_BODY_CAP);reply=malloc(VV_ALIYUN_JSON_MAX);if(!body||!reply){rc=-ENOMEM;goto done;}rc=vv_aliyun_asr_json(c->config.asr_model,pcm,n,(char*)body,ASR_BODY_CAP,&len);if(rc)goto done;struct bytes b={body,len,ASR_BODY_CAP},r={reply,0,VV_ALIYUN_JSON_MAX};rc=request(&g,c->config.host,c->config.asr_path,&b,&r,&meta);out->http_status=meta.status_code;printf("ALIYUN_ASR_WIRE rc=%d http=%d json_mime=%d bytes=%lu\n",rc,meta.status_code,(int)mime(meta.content_type,"application/json"),(unsigned long)r.n);if(!rc&&!mime(meta.content_type,"application/json"))rc=-EPROTO;if(!rc)rc=vv_aliyun_asr_text((char*)reply,r.n,out->text,sizeof(out->text));done:if(body){vv_https_secure_zero(body,ASR_BODY_CAP);free(body);}if(reply){vv_https_secure_zero(reply,VV_ALIYUN_JSON_MAX);free(reply);}if(rc)memset(out->text,0,sizeof(out->text));atomic_store(&c->busy,false);return rc;}
static int tts(void*a,const char*text,unsigned char*pcm,size_t cap,size_t*size,const struct vv_https_cancel*cancel){if(!text||!pcm||!size||!cap||cap>960000||cap%2)return -EINVAL;struct vv_aliyun_client*c=a;struct guard g;int rc=enter(c,&g,cancel);if(rc)return rc;*size=0;vv_https_secure_zero(pcm,cap);if(!c->config.download_host_count){atomic_store(&c->busy,false);return -EACCES;}unsigned char*body=NULL,*reply=NULL,*wav=NULL;struct vv_aliyun_download url={0};size_t n=0;struct vv_https_response meta={0};if((rc=gate(&g)))goto done;body=malloc(VV_ALIYUN_JSON_MAX);reply=malloc(VV_ALIYUN_JSON_MAX);if(!body||!reply){rc=-ENOMEM;goto done;}rc=vv_aliyun_tts_json(c->config.tts_model,c->config.voice,text,(char*)body,VV_ALIYUN_JSON_MAX,&n);if(rc)goto done;struct bytes b={body,n,VV_ALIYUN_JSON_MAX},r={reply,0,VV_ALIYUN_JSON_MAX};rc=request(&g,c->config.host,c->config.tts_path,&b,&r,&meta);if(!rc&&!mime(meta.content_type,"application/json"))rc=-EPROTO;if(!rc)rc=vv_aliyun_tts_download_policy((char*)reply,r.n,c->config.download_hosts,c->config.download_host_count,c->config.allow_proven_bj_compat,&url);if(rc)goto done;wav=malloc(WAV_MAX);if(!wav){rc=-ENOMEM;goto done;}struct bytes audio={wav,0,WAV_MAX};memset(&meta,0,sizeof(meta));rc=request(&g,url.host,url.path,NULL,&audio,&meta);if(!rc&&!mime(meta.content_type,"audio/wav")&&!mime(meta.content_type,"audio/x-wav")&&!mime(meta.content_type,"application/octet-stream"))rc=-EPROTO;if(!rc)rc=vv_aliyun_wav_pcm_policy(wav,audio.n,pcm,cap,size,c->config.allow_proven_bj_compat&&!strcmp(url.host,"dashscope-result-bj.oss-cn-beijing.aliyuncs.com"));if(!rc)rc=gate(&g);done:vv_https_secure_zero(&url,sizeof(url));if(body){vv_https_secure_zero(body,VV_ALIYUN_JSON_MAX);free(body);}if(reply){vv_https_secure_zero(reply,VV_ALIYUN_JSON_MAX);free(reply);}if(wav){vv_https_secure_zero(wav,WAV_MAX);free(wav);}if(rc){vv_https_secure_zero(pcm,cap);*size=0;}atomic_store(&c->busy,false);return rc;}
struct vv_speech_provider vv_aliyun_client_provider(struct vv_aliyun_client*c){return (struct vv_speech_provider){.asr=asr,.tts=tts,.arg=c};}
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
struct stream_body {struct vv_aliyun_tts_stream *parser;struct vv_https_response *meta;int error;};
struct stream_pcm {int (*sink)(void*,const unsigned char*,size_t);void*arg;size_t accepted;};
static int stream_pcm_accept(void *arg,const unsigned char *data,size_t bytes){
 struct stream_pcm *p=arg;
 int rc=p->sink(p->arg,data,bytes);if(!rc)p->accepted+=bytes;return rc;
}
static int stream_sink(void *arg,const void *data,size_t bytes){
 struct stream_body*s=arg;
 /* HTTPS parser has validated the full header block before invoking a sink.
  * Full message completion is checked separately before success is reported. */
 if(s->meta->status_code!=200||!mime(s->meta->content_type,"text/event-stream"))return s->error=-EPROTO;
 return s->error=vv_aliyun_tts_stream_feed(s->parser,data,bytes);
}
int vv_aliyun_client_stream_ex(struct vv_aliyun_client*c,const char*text,
 int (*pcm)(void*,const unsigned char*,size_t),void*arg,const struct vv_https_cancel*cancel,
 struct vv_aliyun_stream_result *result){
 if(result)memset(result,0,sizeof(*result));
 if(!text||!pcm)return -EINVAL;
 struct guard g;int rc=enter(c,&g,cancel);if(rc)return rc;
 struct vv_aliyun_tts_stream *parser=calloc(1,sizeof(*parser));
 struct stream_pcm output={pcm,arg,0};
 char *body=calloc(1,VV_ALIYUN_JSON_MAX);char auth[2056]={0};size_t n=0;
 if(!parser||!body){rc=-ENOMEM;goto done;}
 rc=vv_aliyun_tts_stream_init(parser,960000,stream_pcm_accept,&output,stop,&g);if(rc)goto done;
 rc=vv_aliyun_tts_stream_json(c->config.tts_model,c->config.voice,text,body,VV_ALIYUN_JSON_MAX,&n);if(rc)goto done;
 snprintf(auth,sizeof(auth),"Bearer %s",c->config.api_key);
 struct vv_https_header h[]={{"Authorization",auth,true},{"Content-Type","application/json",false},{"X-DashScope-SSE","enable",false}};
 struct bytes b={(unsigned char*)body,n,VV_ALIYUN_JSON_MAX};
 struct vv_https_request q={.method="POST",.host=c->config.host,.port=443,.path=c->config.tts_path,.headers=h,.header_count=3,.body_length=n,.body_read=read_body,.body_arg=&b};
 struct vv_https_response meta={0};struct stream_body s={parser,&meta,0};
 struct vv_https_client http=*c->config.https;
 /* Bounded total SSE wire data, independent of the old JSON/file limits. */
 http.limits.max_response_body_bytes=2000000;http.limits.max_chunk_bytes=262144;
 struct vv_https_cancel token={stop,&g};
 rc=vv_https_perform(&http,&q,stream_sink,&s,&token,&meta);
 if(result){result->http_result=rc;result->pcm_bytes=output.accepted;}
 if(s.error)rc=s.error;
 if(!rc&&(meta.status_code!=200||!mime(meta.content_type,"text/event-stream")))rc=-EPROTO;
 /* A protocol-tail error remains an error. terminal_audio_complete requires
  * valid SSE finish with no pending tail. Separately, validated_prefix may
  * retain earlier accepted PCM after a proven transport EOF; the unfinished
  * tail is never played. Known parse/sink/error-event/cancel failures reject
  * both forms of admission. */
 if((!rc||(result&&rc==VV_HTTPS_EPROTO))&&!s.error&&
    meta.status_code==200&&mime(meta.content_type,"text/event-stream")){
  /* Save the feed-only prefix fact before finish rejects a truncated final
   * line/event/odd byte. None of that unfinished tail reaches the PCM sink.
   * Syntax errors, failed sink calls and negative transport results cannot
   * create this flag; the HTTP layer must have observed an actual early EOF. */
  bool prefix=result&&rc==VV_HTTPS_EPROTO&&meta.premature_eof&&
              !parser->error&&!parser->error_event&&output.accepted>0;
  int complete=vv_aliyun_tts_stream_finish(parser);
  int admitted=gate(&g);
  if(result){result->terminal_audio_complete=!complete&&!admitted;
             result->validated_prefix=prefix&&!admitted;}
  if(!rc)rc=complete?complete:admitted;
 }
 printf("TTS_SSE rc=%d http=%d audio_bytes=%lu events=%u terminal=%u http_body_bytes=%lu pending_line_bytes=%lu pending_event_bytes=%lu premature_eof=%u validated_prefix=%u\n",rc,meta.status_code,(unsigned long)parser->total,parser->audio_events,(unsigned)parser->stop,(unsigned long)meta.body_bytes,(unsigned long)parser->line_used,(unsigned long)parser->json_used,(unsigned)meta.premature_eof,(unsigned)(result&&result->validated_prefix));fflush(stdout);
done:
 vv_https_secure_zero(auth,sizeof(auth));
 if(parser){vv_https_secure_zero(parser,sizeof(*parser));free(parser);}
 if(body){vv_https_secure_zero(body,VV_ALIYUN_JSON_MAX);free(body);}
 atomic_store(&c->busy,false);return rc;
}
int vv_aliyun_client_stream(struct vv_aliyun_client*c,const char*text,
 int (*pcm)(void*,const unsigned char*,size_t),void*arg,const struct vv_https_cancel*cancel){
 return vv_aliyun_client_stream_ex(c,text,pcm,arg,cancel,NULL);
}
#endif
