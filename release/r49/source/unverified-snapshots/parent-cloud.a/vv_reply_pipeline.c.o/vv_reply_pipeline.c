#include "vv_reply_pipeline.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_SMP
#include <sched.h>
#endif

struct pipeline;
struct pipeline_guard { struct pipeline *p; uint64_t last; };
struct pipeline {
 struct vv_reply_pipeline_request request;
 vv_stream_pcm_sink sink;void *sink_arg;
 const struct vv_https_cancel *outer;
 atomic_bool stop,chat_done,content_present;
 atomic_int failure;
 atomic_size_t published;
 char text[VV_REPLY_PIPELINE_TEXT_BYTES+1];
 struct pipeline_guard producer,consumer;
 uint64_t began,first_delta,first_text,first_tts,first_pcm;
 bool delta_seen,text_seen,tts_seen,pcm_seen,eof_seen;
 size_t consumed,pcm_bytes;unsigned fragments;
 int chat_rc;
};
static void pause_ms(void){struct timespec t={0,1000000};(void)nanosleep(&t,NULL);}
static int reject(struct pipeline *p,int rc)
{
 if(rc>=0)rc=-EIO;
 int expected=0;
 (void)atomic_compare_exchange_strong(&p->failure,&expected,rc);
 atomic_store_explicit(&p->stop,true,memory_order_release);
 return atomic_load_explicit(&p->failure,memory_order_acquire);
}
static int check(struct pipeline_guard *g)
{
 struct pipeline *p=g->p;int rc=atomic_load_explicit(&p->failure,memory_order_acquire);
 if(rc)return rc;
 if(atomic_load_explicit(&p->stop,memory_order_acquire))return reject(p,-ECANCELED);
 if(p->outer&&p->outer->is_cancelled&&p->outer->is_cancelled(p->outer->arg))
  return reject(p,-ECANCELED);
 uint64_t now=p->request.ops->now_ms(p->request.ops->arg);
 /* last is private to this thread, so concurrent reads cannot make an older
  * sample look like the clock moved backwards on the other thread. */
 if(now==UINT64_MAX||now<g->last||now<p->began||
    now-p->began>=VV_REPLY_PIPELINE_TIMEOUT_MS)return reject(p,-ETIMEDOUT);
 g->last=now;return atomic_load_explicit(&p->failure,memory_order_acquire);
}
static bool cancelled(void *arg){return check(arg)!=0;}
static bool white(unsigned v)
{
 return (v>=9&&v<=13)||v==32||v==0x85||v==0xa0||v==0x1680||
  (v>=0x2000&&v<=0x200a)||v==0x2028||v==0x2029||v==0x202f||
  v==0x205f||v==0x3000;
}
static bool punctuation(unsigned v)
{
 return v=='.'||v==','||v=='!'||v=='?'||v==';'||v==':'||v=='\n'||v=='\r'||
  v==0x3002||v==0xff0c||v==0xff01||v==0xff1f||v==0xff1b||v==0xff1a||
  v==0x2028||v==0x2029;
}
/* Explicit length; never read the byte at a concurrently advancing boundary.
 * Deltas must end at a scalar boundary. Reject NUL and non-white controls. */
static int scalar(const char *s,size_t n,size_t *at,unsigned *value)
{
 if(*at>=n)return -EPROTO;
 unsigned v=(unsigned char)s[(*at)++],more=0,min=0;
 if(v<128){}else if(v>=0xc2&&v<=0xdf){more=1;min=0x80;v&=31;}
 else if(v>=0xe0&&v<=0xef){more=2;min=0x800;v&=15;}
 else if(v>=0xf0&&v<=0xf4){more=3;min=0x10000;v&=7;}
 else return -EPROTO;
 if(n-*at<more)return -EPROTO;
 while(more--){unsigned b=(unsigned char)s[(*at)++];if((b&0xc0)!=0x80)return -EPROTO;v=(v<<6)|(b&63);}
 if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff)||!v||v==127||
    (v<32&&!white(v)))return -EPROTO;
 *value=v;return 0;
}
static int append_delta(void *arg,const char *text,size_t bytes)
{
 struct pipeline *p=arg;int rc=check(&p->producer);if(rc)return rc;
 if(!bytes)return 0;
 if(!text)return reject(p,-EINVAL);
 size_t have=atomic_load_explicit(&p->published,memory_order_relaxed);
 if(bytes>VV_REPLY_PIPELINE_TEXT_BYTES-have)return reject(p,-EOVERFLOW);
 bool content=false;
 for(size_t at=0;at<bytes;){unsigned v;rc=scalar(text,bytes,&at,&v);if(rc)return reject(p,rc);if(!white(v))content=true;}
 memcpy(p->text+have,text,bytes);
 if(!p->delta_seen){p->delta_seen=true;p->first_delta=p->producer.last-p->began;}
 /* Published bytes never change, including whitespace. No consumer accesses
  * the live terminator; the whole text is NUL-terminated only after join. */
 atomic_store_explicit(&p->published,have+bytes,memory_order_release);
 if(content)atomic_store_explicit(&p->content_present,true,memory_order_release);
 return check(&p->producer);
}
static void *chat_worker(void *arg)
{
 struct pipeline *p=arg;const struct vv_reply_pipeline_ops *ops=p->request.ops;
 struct vv_https_cancel token={cancelled,&p->producer};
 int rc=check(&p->producer);
 if(!rc)rc=ops->chat(ops->arg,p->request.question,p->request.answer,
  p->request.answer_capacity,&token,append_delta,p);
 if(rc)rc=reject(p,rc);
 if(!rc)rc=check(&p->producer);
 if(!rc){
  size_t n=0;while(n<p->request.answer_capacity&&p->request.answer[n])++n;
  size_t emitted=atomic_load_explicit(&p->published,memory_order_acquire);
  if(n==p->request.answer_capacity||n!=emitted||memcmp(p->request.answer,p->text,n))
   rc=reject(p,-EPROTO);
 }
 p->chat_rc=rc;
 /* Last shared access. Even a failed join must observe this before it can
  * retire caller-owned ops/answer; it may never detach then free a live worker. */
 atomic_store_explicit(&p->chat_done,true,memory_order_release);return NULL;
}
static int launch_chat(pthread_t *thread,struct pipeline *p)
{
 pthread_attr_t attr;int e=pthread_attr_init(&attr);if(e)return -e;
 e=pthread_attr_setstacksize(&attr,65536);
#ifdef CONFIG_SMP
 if(!e){cpu_set_t set;CPU_ZERO(&set);
#if defined(K7_CPU_ROLE_SPLIT) && CONFIG_SMP_NCPUS > 7
  CPU_SET(0,&set); /* Remote chat/JSON coordination, not model inference. */
#elif CONFIG_SMP_NCPUS > 7
  CPU_SET(7,&set);
#else
  CPU_SET(0,&set);
#endif
  e=pthread_attr_setaffinity_np(&attr,sizeof(set),&set);}
#endif
 if(!e)e=pthread_create(thread,&attr,chat_worker,p);
 (void)pthread_attr_destroy(&attr);return -e;
}
static int next_text(void *arg,char *out,size_t capacity,size_t *written)
{
 struct pipeline *p=arg;if(written)*written=0;if(out&&capacity)out[0]=0;
 if(!out||!capacity||!written)return reject(p,-EINVAL);
 int rc=check(&p->consumer);if(rc)return rc;
 /* Loading done before length includes every byte published before success. */
 bool done=atomic_load_explicit(&p->chat_done,memory_order_acquire);
 if(done&&(rc=check(&p->consumer)))return rc;
 size_t n=atomic_load_explicit(&p->published,memory_order_acquire),at=p->consumed,start=at;
 if(!atomic_load_explicit(&p->content_present,memory_order_acquire))
  return done?reject(p,-EPROTO):VV_REPLY_TEXT_AGAIN;
 unsigned count=0,limit=p->fragments?64u:24u,min=p->fragments?1u:8u;
 bool ready=false;
 while(at<n){
  unsigned v;rc=scalar(p->text,n,&at,&v);if(rc)return reject(p,rc);
  ++count;
  if((punctuation(v)&&count>=min)||count>=limit){ready=true;break;}
 }
 if(at==start){if(done){p->eof_seen=true;return VV_REPLY_TEXT_DONE;}return VV_REPLY_TEXT_AGAIN;}
 if(!ready&&!done)return VV_REPLY_TEXT_AGAIN;
 /* A duplex task concatenates successive inputs: whitespace is data here.
  * Preserve every byte, including leading/trailing spaces and newlines. */
 size_t bytes=at-start;
 if(!bytes||bytes>VV_REPLY_PIPELINE_SEGMENT_BYTES)return reject(p,-EOVERFLOW);
 if(bytes>=capacity)return reject(p,-ENOSPC);
 memcpy(out,p->text+start,bytes);out[bytes]=0;*written=bytes;
 p->consumed=at;++p->fragments;
 if(!p->text_seen){p->text_seen=true;p->first_text=p->consumer.last-p->began;}
 return VV_REPLY_TEXT_READY;
}
static int forward_pcm(void *arg,const unsigned char *pcm,size_t bytes)
{
 struct pipeline *p=arg;int rc=check(&p->consumer);if(rc)return rc;
 if(!p->text_seen)return reject(p,-EPROTO);
 if(!pcm||!bytes||bytes%2)return reject(p,-EINVAL);
 if(bytes>VV_REPLY_PIPELINE_PCM_BYTES-p->pcm_bytes)return reject(p,-EOVERFLOW);
 rc=p->sink(p->sink_arg,pcm,bytes);if(rc)return reject(p,rc);
 p->pcm_bytes+=bytes;
 if(!p->pcm_seen){p->pcm_seen=true;p->first_pcm=p->consumer.last-p->began;}
 return check(&p->consumer);
}
static int complete_tts(struct pipeline *p,int rc,const struct vv_aliyun_stream_result *r,size_t before)
{
 if(rc)return reject(p,rc);
 if((rc=check(&p->consumer)))return rc;
 if(r->http_result)return reject(p,r->http_result<0?r->http_result:-EPROTO);
 if(!r->terminal_audio_complete||!r->pcm_bytes||r->pcm_bytes%2||
    r->pcm_bytes!=p->pcm_bytes-before)return reject(p,-EPROTO);
 return 0;
}
static int synthesize(struct pipeline *p,int *http_result)
{
 const struct vv_reply_pipeline_ops *ops=p->request.ops;
 struct vv_https_cancel token={cancelled,&p->consumer};
 int rc=check(&p->consumer);if(rc)return rc;
 if(ops->tts_stream){
  struct vv_aliyun_stream_result r={0};p->tts_seen=true;p->first_tts=p->consumer.last-p->began;
  rc=ops->tts_stream(ops->arg,next_text,p,forward_pcm,p,&token,&r);
  *http_result=r.http_result;rc=complete_tts(p,rc,&r,0);
  if(!rc&&!p->eof_seen)rc=reject(p,-EPROTO);
  return rc;
 }
 for(;;){
  char fragment[VV_REPLY_PIPELINE_SEGMENT_BYTES+1];size_t bytes=0;
  rc=next_text(p,fragment,sizeof(fragment),&bytes);
  if(rc<0)return rc;
  if(rc==VV_REPLY_TEXT_DONE)return 0;
  if(rc==VV_REPLY_TEXT_AGAIN){pause_ms();continue;}
  struct vv_aliyun_stream_result r={0};size_t before=p->pcm_bytes;
  if(!p->tts_seen){p->tts_seen=true;p->first_tts=p->consumer.last-p->began;}
  rc=ops->tts(ops->arg,fragment,forward_pcm,p,&token,&r);
  vv_https_secure_zero(fragment,sizeof(fragment));*http_result=r.http_result;
  rc=complete_tts(p,rc,&r,before);if(rc)return rc;
 }
}
int vv_reply_pipeline_fetch(void *arg,vv_stream_pcm_sink sink,void *sink_arg,
 const struct vv_https_cancel *outer,struct vv_stream_fetch_result *result)
{
 if(result){
  memset(result,0,sizeof(*result));
  /* No child exists yet. Every early validation/allocation/launch failure
   * therefore has known no-child retirement, not inferred from an empty PCM. */
  result->retirement_proof_valid=true;result->nested_workers_joined=true;
 }
 struct vv_reply_pipeline_request *request=arg;
 if(!request||!request->ops||!request->ops->chat||!request->ops->now_ms||
    (!request->ops->tts&&!request->ops->tts_stream)||!request->question||
    !request->answer||!request->answer_capacity||request->answer_capacity>VV_CHAT_ANSWER_MAX+1||
    !sink||!result)return -EINVAL;
 vv_https_secure_zero(request->answer,request->answer_capacity);
 struct pipeline *p=calloc(1,sizeof(*p));if(!p)return -ENOMEM;
 p->request=*request;p->sink=sink;p->sink_arg=sink_arg;p->outer=outer;
 atomic_init(&p->stop,false);atomic_init(&p->chat_done,false);atomic_init(&p->content_present,false);
 atomic_init(&p->failure,0);atomic_init(&p->published,0);
 p->began=request->ops->now_ms(request->ops->arg);
 p->producer=(struct pipeline_guard){p,p->began};p->consumer=p->producer;
 int rc=p->began==UINT64_MAX?reject(p,-ETIMEDOUT):check(&p->consumer);
 bool joined=true;pthread_t thread;
 if(!rc)rc=launch_chat(&thread,p);
 if(!rc){
  joined=false;result->nested_workers_joined=false;
  rc=synthesize(p,&result->http_result);if(rc)(void)reject(p,rc);
  /* Keep the coordinator active until the producer exits even if a defective
   * TTS implementation returns early. Every network op must honor its token. */
  while(!atomic_load_explicit(&p->chat_done,memory_order_acquire)){
   (void)check(&p->consumer);pause_ms();
  }
  int e=pthread_join(thread,NULL);
  if(e){
   (void)reject(p,-e);
   /* No detach. A transient OS join failure gets one retry after last access.
    * A repeated failure quarantines the now inert allocation below; it is
    * never reported as successful cleanup or freed under a live thread. */
   e=pthread_join(thread,NULL);
  }
  joined=e==0;
  if(e)(void)reject(p,-e);
  rc=atomic_load_explicit(&p->failure,memory_order_acquire);
  if(!rc)rc=p->chat_rc;
  if(!rc)rc=check(&p->consumer);
  if(!rc&&(!p->eof_seen||!p->pcm_bytes))rc=reject(p,-EPROTO);
 }
 if(rc)(void)reject(p,rc);
 rc=atomic_load_explicit(&p->failure,memory_order_acquire);
 /* chat_done proves last shared access only. A persistent OS join failure
  * must propagate separately even though this fetch can now return safely. */
 result->retirement_proof_valid=true;result->nested_workers_joined=joined;
 result->pcm_bytes=p->pcm_bytes;result->terminal_audio_complete=!rc;
 /* Immediate audio cannot be revoked or safely retried. Unlike the old
  * verified-prefix route, any failure cancels the queued remainder. */
 result->validated_prefix=false;
 if(rc)vv_https_secure_zero(request->answer,request->answer_capacity);
 uint64_t ended=request->ops->now_ms(request->ops->arg);
 bool end_valid=ended!=UINT64_MAX&&ended>=p->began&&ended>=p->consumer.last&&ended>=p->producer.last;
 printf("REPLY_PIPELINE rc=%d chat_rc=%d duplex=%u fragments=%u text_bytes=%zu pcm_bytes=%zu delta_seen=%u first_delta_ms=%llu text_seen=%u first_text_ms=%llu tts_seen=%u first_tts_ms=%llu pcm_seen=%u first_pcm_ms=%llu end_ms=%llu end_valid=%u joined=%u\n",
  rc,p->chat_rc,request->ops->tts_stream?1u:0u,p->fragments,
  atomic_load_explicit(&p->published,memory_order_acquire),p->pcm_bytes,
  p->delta_seen?1u:0u,(unsigned long long)p->first_delta,p->text_seen?1u:0u,
  (unsigned long long)p->first_text,p->tts_seen?1u:0u,(unsigned long long)p->first_tts,
  p->pcm_seen?1u:0u,(unsigned long long)p->first_pcm,
  (unsigned long long)(end_valid?ended-p->began:0),end_valid?1u:0u,joined?1u:0u);
 fflush(stdout);
 if(joined){vv_https_secure_zero(p,sizeof(*p));free(p);}
 else {vv_https_secure_zero(p->text,sizeof(p->text));}
 return rc;
}
