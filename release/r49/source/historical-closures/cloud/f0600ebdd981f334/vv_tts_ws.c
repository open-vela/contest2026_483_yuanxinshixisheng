#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_WIRE_CANCEL
#include "vv_tts_ws.h"
#include "vv_reply_json.h"
#include "vv_chat_stream.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
#include "vv_opus_pcm.h"
#endif
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model Studio Qwen-Audio-TTS run/continue/finish duplex protocol:
 * https://help.aliyun.com/zh/model-studio/qwen-audio-tts-client-events
 * https://help.aliyun.com/zh/model-studio/qwen-audio-tts-server-events
 * One verified connection per answer; every text fragment shares its task.
 * This removes sentence reconnects. No implicit protocol/model fallback. */
#define RX_MAX 65536u
#define TOTAL_MS 90000u
#define PCM_MAX 2880000u
struct tts_ws {
 const struct vv_tts_ws_config *c;struct vv_wsi ws;
 vv_stream_pcm_sink sink;void *sink_arg;
 unsigned char rx[RX_MAX+1];char task[37];
 unsigned state,events,texts,blocks,wire_blocks;size_t pcm,wire_bytes;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 struct vv_opus_pcm *opus;
#endif
 bool odd,run_pending,run_sent,finish_pending,finish_sent;unsigned char low;
 const struct vv_https_cancel *cancel;
 bool canceling,cancel_pending,cancel_sent,cancel_ack;
 uint64_t began,ready,first_text,first_pcm;
};
static bool cancelled(const struct vv_https_cancel *c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static ssize_t read_io(void*a,void*p,size_t n){struct tts_ws*s=a;return s->c->io.read(s->c->io.arg,p,n);}
static ssize_t write_io(void*a,const void*p,size_t n){
 struct tts_ws*s=a;ssize_t rc=s->c->io.write(s->c->io.arg,p,n);
 /* WSI passes the remaining bytes of one owned frame on every short write.
  * Remember the application frame, independently of a later ping reply. */
 if(rc>0&&(size_t)rc==n){
  if(s->run_pending){s->run_pending=false;s->run_sent=true;}
  if(s->finish_pending){s->finish_pending=false;s->finish_sent=true;}
  if(s->cancel_pending){s->cancel_pending=false;s->cancel_sent=true;}
 }
 return rc;
}
static int random_io(void*a,uint8_t*p,size_t n){struct tts_ws*s=a;return s->c->io.random(s->c->io.arg,p,n);}
static void close_io(void*a){struct tts_ws*s=a;s->c->io.close(s->c->io.arg);}
static bool busy(const struct tts_ws*s){return s->ws.tx_length||s->ws.pending_opcode||s->ws.close_after||s->ws.tx_close;}
static cJSON *get(const cJSON *o,const char*k){return cJSON_GetObjectItemCaseSensitive(o,k);}
static bool equals(const cJSON *o,const char*v){return cJSON_IsString(o)&&o->valuestring&&!strcmp(o->valuestring,v);}
static bool unique(const cJSON *o,unsigned depth){
 if(depth>16)return false;
 for(const cJSON*a=o?o->child:NULL;a;a=a->next){
  if(cJSON_IsObject(o))for(const cJSON*b=a->next;b;b=b->next)
   if(!a->string||!b->string||!strcmp(a->string,b->string))return false;
  if(!unique(a,depth+1))return false;
 }
 return true;
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
static int opus_pcm(void*arg,const unsigned char*p,size_t n){
 struct tts_ws*s=arg;
 if(!p||!n||n%2)return -EPROTO;
 if(n>PCM_MAX-s->pcm)return -EOVERFLOW;
 int rc=s->sink(s->sink_arg,p,n);if(rc)return rc;
 if(!s->blocks)s->first_pcm=s->c->now_ms(s->c->clock_arg);
 s->pcm+=n;++s->blocks;return 0;
}
#endif
static int audio(struct tts_ws*s,const unsigned char*p,size_t n){
 if(!s->state||s->state>=3||!s->texts)return -EPROTO;
 if(n>PCM_MAX-s->wire_bytes)return -EOVERFLOW;
 s->wire_bytes+=n;if(n)++s->wire_blocks;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 if(s->c->opus)return vv_opus_pcm_feed(s->opus,p,n);
#endif
 if(n>PCM_MAX-s->pcm-(s->odd?1u:0u))return -EOVERFLOW;
 int rc=0;size_t at=0;
 if(s->odd&&n){unsigned char pair[2]={s->low,p[0]};
  rc=s->sink(s->sink_arg,pair,2);if(rc)return rc;s->pcm+=2;s->odd=false;at=1;
 }
 size_t even=(n-at)&~(size_t)1;
 if(even){rc=s->sink(s->sink_arg,p+at,even);if(rc)return rc;s->pcm+=even;at+=even;}
 if(at<n){s->low=p[at];s->odd=true;}
 if(n){if(!s->blocks)s->first_pcm=s->c->now_ms(s->c->clock_arg);++s->blocks;}
 return 0;
}
static int event(struct tts_ws*s,const void*data,size_t n){
 if(!data||!n||n>RX_MAX||memchr(data,0,n)||s->state>=3||++s->events>4096)return -EPROTO;
 const char*raw=data;bool quoted=false,escaped=false;unsigned depth=0;
 for(size_t i=0;i<n;i++){
  unsigned char c=(unsigned char)raw[i];
  if(quoted){if(escaped)escaped=false;else if(c=='\\'){
    if(i+5<n&&!memcmp(raw+i,"\\u0000",6))return -EPROTO;
    escaped=true;
   }else if(c=='"')quoted=false;}
  else if(c=='"')quoted=true;
  else if(c=='{'||c=='['){if(++depth>16)return -EPROTO;}
  else if(c=='}'||c==']'){if(!depth)return -EPROTO;--depth;}
 }
 if(quoted||depth)return -EPROTO;
 /* vv_wsi validates JSON message UTF-8; the spare byte is owned by this run. */
 s->rx[n]=0;cJSON*root=vv_reply_json_parse(raw,n);int rc=-EPROTO;
 if(!cJSON_IsObject(root)||!unique(root,0))goto done;
 cJSON*h=get(root,"header"),*kind=get(h,"event");
 if(!cJSON_IsObject(h)||!equals(get(h,"task_id"),s->task))goto done;
 if(equals(kind,"task-failed")){
  /* Log only fixed classifications after validating this task's identity.
   * Provider messages/IDs may contain account data. Keep the original error
   * and never retry/replay a task from this diagnostic path. */
  const char *code=equals(get(h,"error_code"),"Arrearage")?"Arrearage":"other";
  printf("TTS_TASK_FAILED code=%s state=%u texts=%u pcm_bytes=%zu\n",
         code,s->state,s->texts,s->pcm);
  rc=-EIO;goto done;
 }
 if(equals(kind,"task-started")){
  if(s->state||!s->run_sent)goto done;
  s->state=1;s->ready=s->c->now_ms(s->c->clock_arg);rc=0;goto done;
 }
 if(equals(kind,"task-finished")){
  if(s->canceling){
   /* No audio completeness check: cancelled output is deliberately partial.
    * A terminal before the cancel frame is fully sent cannot acknowledge it. */
   if(!s->cancel_sent)goto done;
   s->cancel_ack=true;rc=0;goto done;
  }
  /* Queuing finish is not proof that its masked frame reached the peer. */
  if(s->state!=2||!s->finish_sent||!s->pcm||s->odd)goto done;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
  if(s->c->opus){
   struct vv_opus_pcm_stats stats;rc=vv_opus_pcm_finish(s->opus,true);if(rc)goto done;
   vv_opus_pcm_get_stats(s->opus,&stats);
   if(stats.pcm_bytes!=s->pcm||stats.wire_bytes!=s->wire_bytes){rc=-EPROTO;goto done;}
  }
#endif
  s->state=3;rc=0;goto done;
 }
 if(equals(kind,"result-generated")&&(s->state==1||s->state==2)&&s->texts){
  cJSON*output=get(get(root,"payload"),"output"),*type=get(output,"type");
  if(cJSON_IsObject(output)&&(equals(type,"sentence-begin")||equals(type,"sentence-synthesis")||equals(type,"sentence-end")))rc=0;
 }
done:cJSON_Delete(root);return rc;
}
static int message(void*a,unsigned op,const void*p,size_t n){
 struct tts_ws*s=a;
 /* Do not let late binary invoke Opus or the sink once cancellation is known.
  * This flag never bypasses transport security or permits another answer. */
 if(s->c->wire_cancel&&op==2&&(s->canceling||cancelled(s->cancel)))return 0;
 int rc=op==2?audio(s,p,n):op==1?event(s,p,n):-EPROTO;
 /* Cancellation can occur inside codec/sink. Preserve the live WSI solely to
  * finish its pending frame and send bounded cancel cleanup at the owner. */
 if(s->c->wire_cancel&&(rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&cancelled(s->cancel))return 0;
 return rc;
}
static int setting(const char*s){
 if(!s||!*s)return -EINVAL;
 size_t n=0;for(;n<129&&s[n];n++){unsigned char c=(unsigned char)s[n];
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==':'))return -EINVAL;
 }return n<=128?0:-EINVAL;
}
static int command(struct tts_ws*s,const char*action,const char*text,char*out,size_t cap,size_t*bytes){
 int n;
 if(!strcmp(action,"run-task")){
  n=snprintf(out,cap,"{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"task_group\":\"audio\",\"task\":\"tts\",\"function\":\"SpeechSynthesizer\",\"model\":\"%s\",\"parameters\":{\"text_type\":\"PlainText\",\"voice\":\"%s\",\"format\":\"%s\"%s,\"sample_rate\":16000,\"volume\":50,\"rate\":1,\"pitch\":1,\"enable_ssml\":false},\"input\":{}}}",s->task,s->c->model,s->c->voice,s->c->opus?"opus":"pcm",s->c->opus?",\"bit_rate\":32":"");
 }else if(!strcmp(action,"cancel-task")){
  n=snprintf(out,cap,"{\"header\":{\"action\":\"finish-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{\"directive\":\"cancel\"}}}",s->task);
 }else if(!strcmp(action,"finish-task")){
  n=snprintf(out,cap,"{\"header\":{\"action\":\"finish-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}",s->task);
 }else{
  /* Print uses immutable allocator hooks, not the parser's shared error cursor. */
  cJSON*str=cJSON_CreateString(text);if(!str)return -ENOMEM;
  char*encoded=cJSON_PrintUnformatted(str);cJSON_Delete(str);if(!encoded)return -ENOMEM;
  n=snprintf(out,cap,"{\"header\":{\"action\":\"continue-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{\"text\":%s}}}",s->task,encoded);
  vv_https_secure_zero(encoded,strlen(encoded));cJSON_free(encoded);
 }
 if(n<0||(size_t)n>=cap)return -E2BIG;
 *bytes=(size_t)n;return 0;
}
/* Single transport owner; does not change pending WSI/TLS write storage.
 * The transport remains responsible for its security gate on every try I/O. */
static void wire_cancel(struct tts_ws*s){
 if(!s->c->wire_cancel||!s->ws.active||!s->run_sent||s->state<1||s->state>2)return;
 uint64_t start=s->c->now_ms(s->c->clock_arg);
 if(start==UINT64_MAX||start<s->ws.last_now||start<s->began||start-s->began>=TOTAL_MS)return;
 uint64_t remain=TOTAL_MS-(start-s->began),limit=start+(remain<200?remain:200);
 s->canceling=true;unsigned pumps=0;bool queued=false;int cleanup=-ETIMEDOUT;
 char control[256];size_t bytes=0;
 while(pumps<256){
  uint64_t now=s->c->now_ms(s->c->clock_arg);
  if(now==UINT64_MAX||now<start||now<s->ws.last_now||now>=limit)break;
  /* Even tx_offset==0 may be a TLS WANT with the original pointer pinned.
   * Drain intact or close; never replace a partially attempted frame. */
  if(!queued&&!busy(s)){
   cleanup=command(s,"cancel-task",NULL,control,sizeof(control),&bytes);if(cleanup)break;
   cleanup=vv_wsi_queue(&s->ws,1,control,bytes);if(cleanup)break;
   s->cancel_pending=true;queued=true;
  }
  cleanup=vv_wsi_pump(&s->ws,now,false,1);++pumps;
  if(cleanup||!s->ws.active||s->cancel_ack)break;
  /* This connection belongs only to the cancelled answer. Once its complete
   * cancel frame is accepted by TLS, close locally without waiting for an ACK.
   * cancel_ack remains false unless the peer actually confirmed it. Pending
   * original/cancel frames keep the same bounded drain and pinned storage. */
  if(s->cancel_sent)break;
  cleanup=-ETIMEDOUT;
  if(pumps<256)s->c->sleep_ms(s->c->sleep_arg,1);
 }
 printf("TTS_WIRE_CANCEL queued=%u sent=%u terminal=%u cleanup=%d pumps=%u ack_wait_skipped=%u\n",
  (unsigned)queued,(unsigned)s->cancel_sent,(unsigned)s->cancel_ack,cleanup,pumps,
  (unsigned)(!cleanup&&s->ws.active&&s->cancel_sent&&!s->cancel_ack));
 vv_https_secure_zero(control,sizeof(control));
}
int vv_tts_ws_run(const struct vv_tts_ws_config*c,vv_reply_next_text_fn next,void*next_arg,
 vv_stream_pcm_sink sink,void*sink_arg,const struct vv_https_cancel*cancel,struct vv_aliyun_stream_result*out){
 if(out)memset(out,0,sizeof(*out));
 if(!c||!out||!next||!sink||!c->io.read||!c->io.write||!c->io.random||!c->io.close||!c->now_ms||!c->sleep_ms){
  if(c&&c->io.close)c->io.close(c->io.arg);
  if(out)out->http_result=-EINVAL;
  return -EINVAL;
 }
 struct tts_ws*s=calloc(1,sizeof(*s));if(!s){c->io.close(c->io.arg);out->http_result=-ENOMEM;return -ENOMEM;}
 s->c=c;s->sink=sink;s->sink_arg=sink_arg;s->cancel=cancel;
 char control[4096]={0},text[1025]={0};unsigned char id[16]={0};size_t bytes=0;
 int rc=setting(c->model);if(!rc)rc=setting(c->voice);
 bool owned=false;uint64_t last=0,tx_deadline=0,phase=0;
 if(rc)goto done;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 if(c->opus){rc=vv_opus_pcm_create_qwen_live(opus_pcm,s,cancel,&s->opus);if(rc)goto done;}
#else
 if(c->opus){rc=-ENOTSUP;goto done;}
#endif
 s->began=last=c->now_ms(c->clock_arg);if(last>UINT64_MAX-TOTAL_MS){rc=-ETIMEDOUT;goto done;}
 phase=last+15000;
 if(cancelled(cancel)){rc=-ECANCELED;goto done;}
 struct vv_wsi_ops io={read_io,write_io,random_io,close_io,message,s};
 rc=vv_wsi_init(&s->ws,&io,s->rx,RX_MAX,last,last+TOTAL_MS);if(rc)goto done;owned=true;
 rc=c->io.random(c->io.arg,id,sizeof(id));if(rc){if(rc>0)rc=-EIO;goto done;}
 id[6]=(id[6]&15)|64;id[8]=(id[8]&63)|128;
 snprintf(s->task,sizeof(s->task),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",id[0],id[1],id[2],id[3],id[4],id[5],id[6],id[7],id[8],id[9],id[10],id[11],id[12],id[13],id[14],id[15]);
 rc=command(s,"run-task",NULL,control,sizeof(control),&bytes);if(rc)goto done;
 rc=vv_wsi_queue(&s->ws,1,control,bytes);if(rc)goto done;s->run_pending=true;tx_deadline=last+10000;
 for(;;){
  uint64_t now=c->now_ms(c->clock_arg);
  if(now==UINT64_MAX||now<last||now-s->began>=TOTAL_MS||(phase&&now>=phase)||(tx_deadline&&now>=tx_deadline)){rc=-ETIMEDOUT;break;}
  last=now;if(cancelled(cancel)){rc=-ECANCELED;break;}
  rc=vv_wsi_pump(&s->ws,now,false,1);if(rc){if(rc>0)rc=-ECONNRESET;break;}
  if(c->wire_cancel&&cancelled(cancel)){rc=-ECANCELED;break;}
  if(s->state==3){rc=cancelled(cancel)?-ECANCELED:0;break;}
  if(!s->ws.active){rc=-ECONNRESET;break;}
  if(!busy(s))tx_deadline=0;else if(!tx_deadline)tx_deadline=now+10000;
  if(s->state==1&&!busy(s)){
   size_t length=0;int got=next(next_arg,text,sizeof(text),&length);
   if(got<0){rc=got;break;}
   if(c->wire_cancel&&cancelled(cancel)){rc=-ECANCELED;break;}
   if(got==1){
    if(!length||length>=sizeof(text)||text[length]||strlen(text)!=length||vv_chat_text_measure(text,sizeof(text)-1,NULL,NULL,NULL)){rc=-EPROTO;break;}
    rc=command(s,"continue-task",text,control,sizeof(control),&bytes);if(rc)break;
    rc=vv_wsi_queue(&s->ws,1,control,bytes);if(rc)break;
    if(!s->texts)s->first_text=now;
    ++s->texts;tx_deadline=now+10000;phase=now+22000;
   }else if(got==2){
    if(!s->texts){rc=-EPROTO;break;}
    rc=command(s,"finish-task",NULL,control,sizeof(control),&bytes);if(rc)break;
    rc=vv_wsi_queue(&s->ws,1,control,bytes);if(rc)break;
    s->state=2;s->finish_pending=true;tx_deadline=now+10000;phase=0;
   }else if(got){rc=-EPROTO;break;}
   else if(!s->texts)phase=s->ready+22000;
   vv_https_secure_zero(text,sizeof(text));vv_https_secure_zero(control,sizeof(control));
  }
  c->sleep_ms(c->sleep_arg,1);
 }
done:
 if((rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&cancelled(cancel)&&c->wire_cancel){
  wire_cancel(s);rc=-ECANCELED;
 }
 if(owned)vv_wsi_abort(&s->ws);else c->io.close(c->io.arg);
 if(!rc&&s->state!=3)rc=-EPROTO;
 out->http_result=rc;out->pcm_bytes=s->pcm;out->terminal_audio_complete=!rc;
 uint64_t ended=c->now_ms(c->clock_arg);
 printf("TTS_WSS rc=%d texts=%u pcm_blocks=%u bytes=%zu task_complete=%u ready_ms=%llu first_text_ms=%llu first_pcm_ms=%llu total_ms=%llu opus=%u wire_bytes=%zu wire_blocks=%u\n",rc,s->texts,s->blocks,s->pcm,(unsigned)!rc,
  (unsigned long long)(s->ready>=s->began?s->ready-s->began:0),
  (unsigned long long)(s->first_text>=s->began?s->first_text-s->began:0),
  (unsigned long long)(s->first_pcm>=s->began?s->first_pcm-s->began:0),
  (unsigned long long)(ended>=s->began&&ended!=UINT64_MAX?ended-s->began:0),c->opus?1u:0u,s->wire_bytes,s->wire_blocks);
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 if(c->opus){struct vv_opus_pcm_stats stats;vv_opus_pcm_get_stats(s->opus,&stats);
  printf("OPUS_STREAM rc=%d packets=%u pcm_bytes=%zu wire_bytes=%zu pre_skip_48k=%u eos=%u granule_untrusted=1 granule_mismatches=%llu\n",
   rc,stats.packets,stats.pcm_bytes,stats.wire_bytes,stats.pre_skip_48k,stats.eos,
   (unsigned long long)stats.granule_mismatches);
 }
 vv_opus_pcm_destroy(s->opus);
#endif
 vv_https_secure_zero(control,sizeof(control));vv_https_secure_zero(text,sizeof(text));vv_https_secure_zero(id,sizeof(id));
 vv_https_secure_zero(s,sizeof(*s));free(s);return rc;
}

#else
#include "vv_tts_ws.h"
#include "vv_reply_json.h"
#include "vv_chat_stream.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
#include "vv_opus_pcm.h"
#endif
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model Studio Qwen-Audio-TTS run/continue/finish duplex protocol:
 * https://help.aliyun.com/zh/model-studio/qwen-audio-tts-client-events
 * https://help.aliyun.com/zh/model-studio/qwen-audio-tts-server-events
 * One verified connection per answer; every text fragment shares its task.
 * This removes sentence reconnects. No implicit protocol/model fallback. */
#define RX_MAX 65536u
#define TOTAL_MS 90000u
#define PCM_MAX 2880000u
struct tts_ws {
 const struct vv_tts_ws_config *c;struct vv_wsi ws;
 vv_stream_pcm_sink sink;void *sink_arg;
 unsigned char rx[RX_MAX+1];char task[37];
 unsigned state,events,texts,blocks,wire_blocks;size_t pcm,wire_bytes;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 struct vv_opus_pcm *opus;
#endif
 bool odd,run_pending,run_sent,finish_pending,finish_sent;unsigned char low;
 uint64_t began,ready,first_text,first_pcm;
};
static bool cancelled(const struct vv_https_cancel *c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static ssize_t read_io(void*a,void*p,size_t n){struct tts_ws*s=a;return s->c->io.read(s->c->io.arg,p,n);}
static ssize_t write_io(void*a,const void*p,size_t n){
 struct tts_ws*s=a;ssize_t rc=s->c->io.write(s->c->io.arg,p,n);
 /* WSI passes the remaining bytes of one owned frame on every short write.
  * Remember the application frame, independently of a later ping reply. */
 if(rc>0&&(size_t)rc==n){
  if(s->run_pending){s->run_pending=false;s->run_sent=true;}
  if(s->finish_pending){s->finish_pending=false;s->finish_sent=true;}
 }
 return rc;
}
static int random_io(void*a,uint8_t*p,size_t n){struct tts_ws*s=a;return s->c->io.random(s->c->io.arg,p,n);}
static void close_io(void*a){struct tts_ws*s=a;s->c->io.close(s->c->io.arg);}
static bool busy(const struct tts_ws*s){return s->ws.tx_length||s->ws.pending_opcode||s->ws.close_after||s->ws.tx_close;}
static cJSON *get(const cJSON *o,const char*k){return cJSON_GetObjectItemCaseSensitive(o,k);}
static bool equals(const cJSON *o,const char*v){return cJSON_IsString(o)&&o->valuestring&&!strcmp(o->valuestring,v);}
static bool unique(const cJSON *o,unsigned depth){
 if(depth>16)return false;
 for(const cJSON*a=o?o->child:NULL;a;a=a->next){
  if(cJSON_IsObject(o))for(const cJSON*b=a->next;b;b=b->next)
   if(!a->string||!b->string||!strcmp(a->string,b->string))return false;
  if(!unique(a,depth+1))return false;
 }
 return true;
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
static int opus_pcm(void*arg,const unsigned char*p,size_t n){
 struct tts_ws*s=arg;
 if(!p||!n||n%2)return -EPROTO;
 if(n>PCM_MAX-s->pcm)return -EOVERFLOW;
 int rc=s->sink(s->sink_arg,p,n);if(rc)return rc;
 if(!s->blocks)s->first_pcm=s->c->now_ms(s->c->clock_arg);
 s->pcm+=n;++s->blocks;return 0;
}
#endif
static int audio(struct tts_ws*s,const unsigned char*p,size_t n){
 if(!s->state||s->state>=3||!s->texts)return -EPROTO;
 if(n>PCM_MAX-s->wire_bytes)return -EOVERFLOW;
 s->wire_bytes+=n;if(n)++s->wire_blocks;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 if(s->c->opus)return vv_opus_pcm_feed(s->opus,p,n);
#endif
 if(n>PCM_MAX-s->pcm-(s->odd?1u:0u))return -EOVERFLOW;
 int rc=0;size_t at=0;
 if(s->odd&&n){unsigned char pair[2]={s->low,p[0]};
  rc=s->sink(s->sink_arg,pair,2);if(rc)return rc;s->pcm+=2;s->odd=false;at=1;
 }
 size_t even=(n-at)&~(size_t)1;
 if(even){rc=s->sink(s->sink_arg,p+at,even);if(rc)return rc;s->pcm+=even;at+=even;}
 if(at<n){s->low=p[at];s->odd=true;}
 if(n){if(!s->blocks)s->first_pcm=s->c->now_ms(s->c->clock_arg);++s->blocks;}
 return 0;
}
static int event(struct tts_ws*s,const void*data,size_t n){
 if(!data||!n||n>RX_MAX||memchr(data,0,n)||s->state>=3||++s->events>4096)return -EPROTO;
 const char*raw=data;bool quoted=false,escaped=false;unsigned depth=0;
 for(size_t i=0;i<n;i++){
  unsigned char c=(unsigned char)raw[i];
  if(quoted){if(escaped)escaped=false;else if(c=='\\'){
    if(i+5<n&&!memcmp(raw+i,"\\u0000",6))return -EPROTO;
    escaped=true;
   }else if(c=='"')quoted=false;}
  else if(c=='"')quoted=true;
  else if(c=='{'||c=='['){if(++depth>16)return -EPROTO;}
  else if(c=='}'||c==']'){if(!depth)return -EPROTO;--depth;}
 }
 if(quoted||depth)return -EPROTO;
 /* vv_wsi validates JSON message UTF-8; the spare byte is owned by this run. */
 s->rx[n]=0;cJSON*root=vv_reply_json_parse(raw,n);int rc=-EPROTO;
 if(!cJSON_IsObject(root)||!unique(root,0))goto done;
 cJSON*h=get(root,"header"),*kind=get(h,"event");
 if(!cJSON_IsObject(h)||!equals(get(h,"task_id"),s->task))goto done;
 if(equals(kind,"task-failed")){
  /* Log only fixed classifications after validating this task's identity.
   * Provider messages/IDs may contain account data. Keep the original error
   * and never retry/replay a task from this diagnostic path. */
  const char *code=equals(get(h,"error_code"),"Arrearage")?"Arrearage":"other";
  printf("TTS_TASK_FAILED code=%s state=%u texts=%u pcm_bytes=%zu\n",
         code,s->state,s->texts,s->pcm);
  rc=-EIO;goto done;
 }
 if(equals(kind,"task-started")){
  if(s->state||!s->run_sent)goto done;
  s->state=1;s->ready=s->c->now_ms(s->c->clock_arg);rc=0;goto done;
 }
 if(equals(kind,"task-finished")){
  /* Queuing finish is not proof that its masked frame reached the peer. */
  if(s->state!=2||!s->finish_sent||!s->pcm||s->odd)goto done;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
  if(s->c->opus){
   struct vv_opus_pcm_stats stats;rc=vv_opus_pcm_finish(s->opus,true);if(rc)goto done;
   vv_opus_pcm_get_stats(s->opus,&stats);
   if(stats.pcm_bytes!=s->pcm||stats.wire_bytes!=s->wire_bytes){rc=-EPROTO;goto done;}
  }
#endif
  s->state=3;rc=0;goto done;
 }
 if(equals(kind,"result-generated")&&(s->state==1||s->state==2)&&s->texts){
  cJSON*output=get(get(root,"payload"),"output"),*type=get(output,"type");
  if(cJSON_IsObject(output)&&(equals(type,"sentence-begin")||equals(type,"sentence-synthesis")||equals(type,"sentence-end")))rc=0;
 }
done:cJSON_Delete(root);return rc;
}
static int message(void*a,unsigned op,const void*p,size_t n){
 struct tts_ws*s=a;return op==2?audio(s,p,n):op==1?event(s,p,n):-EPROTO;
}
static int setting(const char*s){
 if(!s||!*s)return -EINVAL;
 size_t n=0;for(;n<129&&s[n];n++){unsigned char c=(unsigned char)s[n];
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==':'))return -EINVAL;
 }return n<=128?0:-EINVAL;
}
static int command(struct tts_ws*s,const char*action,const char*text,char*out,size_t cap,size_t*bytes){
 int n;
 if(!strcmp(action,"run-task")){
  n=snprintf(out,cap,"{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"task_group\":\"audio\",\"task\":\"tts\",\"function\":\"SpeechSynthesizer\",\"model\":\"%s\",\"parameters\":{\"text_type\":\"PlainText\",\"voice\":\"%s\",\"format\":\"%s\"%s,\"sample_rate\":16000,\"volume\":50,\"rate\":1,\"pitch\":1,\"enable_ssml\":false},\"input\":{}}}",s->task,s->c->model,s->c->voice,s->c->opus?"opus":"pcm",s->c->opus?",\"bit_rate\":32":"");
 }else if(!strcmp(action,"finish-task")){
  n=snprintf(out,cap,"{\"header\":{\"action\":\"finish-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}",s->task);
 }else{
  /* Print uses immutable allocator hooks, not the parser's shared error cursor. */
  cJSON*str=cJSON_CreateString(text);if(!str)return -ENOMEM;
  char*encoded=cJSON_PrintUnformatted(str);cJSON_Delete(str);if(!encoded)return -ENOMEM;
  n=snprintf(out,cap,"{\"header\":{\"action\":\"continue-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{\"text\":%s}}}",s->task,encoded);
  vv_https_secure_zero(encoded,strlen(encoded));cJSON_free(encoded);
 }
 if(n<0||(size_t)n>=cap)return -E2BIG;
 *bytes=(size_t)n;return 0;
}
int vv_tts_ws_run(const struct vv_tts_ws_config*c,vv_reply_next_text_fn next,void*next_arg,
 vv_stream_pcm_sink sink,void*sink_arg,const struct vv_https_cancel*cancel,struct vv_aliyun_stream_result*out){
 if(out)memset(out,0,sizeof(*out));
 if(!c||!out||!next||!sink||!c->io.read||!c->io.write||!c->io.random||!c->io.close||!c->now_ms||!c->sleep_ms){
  if(c&&c->io.close)c->io.close(c->io.arg);
  if(out)out->http_result=-EINVAL;
  return -EINVAL;
 }
 struct tts_ws*s=calloc(1,sizeof(*s));if(!s){c->io.close(c->io.arg);out->http_result=-ENOMEM;return -ENOMEM;}
 s->c=c;s->sink=sink;s->sink_arg=sink_arg;
 char control[4096]={0},text[1025]={0};unsigned char id[16]={0};size_t bytes=0;
 int rc=setting(c->model);if(!rc)rc=setting(c->voice);
 bool owned=false;uint64_t last=0,tx_deadline=0,phase=0;
 if(rc)goto done;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 if(c->opus){rc=vv_opus_pcm_create_qwen_live(opus_pcm,s,cancel,&s->opus);if(rc)goto done;}
#else
 if(c->opus){rc=-ENOTSUP;goto done;}
#endif
 s->began=last=c->now_ms(c->clock_arg);if(last>UINT64_MAX-TOTAL_MS){rc=-ETIMEDOUT;goto done;}
 phase=last+15000;
 if(cancelled(cancel)){rc=-ECANCELED;goto done;}
 struct vv_wsi_ops io={read_io,write_io,random_io,close_io,message,s};
 rc=vv_wsi_init(&s->ws,&io,s->rx,RX_MAX,last,last+TOTAL_MS);if(rc)goto done;owned=true;
 rc=c->io.random(c->io.arg,id,sizeof(id));if(rc){if(rc>0)rc=-EIO;goto done;}
 id[6]=(id[6]&15)|64;id[8]=(id[8]&63)|128;
 snprintf(s->task,sizeof(s->task),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",id[0],id[1],id[2],id[3],id[4],id[5],id[6],id[7],id[8],id[9],id[10],id[11],id[12],id[13],id[14],id[15]);
 rc=command(s,"run-task",NULL,control,sizeof(control),&bytes);if(rc)goto done;
 rc=vv_wsi_queue(&s->ws,1,control,bytes);if(rc)goto done;s->run_pending=true;tx_deadline=last+10000;
 for(;;){
  uint64_t now=c->now_ms(c->clock_arg);
  if(now==UINT64_MAX||now<last||now-s->began>=TOTAL_MS||(phase&&now>=phase)||(tx_deadline&&now>=tx_deadline)){rc=-ETIMEDOUT;break;}
  last=now;if(cancelled(cancel)){rc=-ECANCELED;break;}
  rc=vv_wsi_pump(&s->ws,now,false,1);if(rc){if(rc>0)rc=-ECONNRESET;break;}
  if(s->state==3){rc=cancelled(cancel)?-ECANCELED:0;break;}
  if(!s->ws.active){rc=-ECONNRESET;break;}
  if(!busy(s))tx_deadline=0;else if(!tx_deadline)tx_deadline=now+10000;
  if(s->state==1&&!busy(s)){
   size_t length=0;int got=next(next_arg,text,sizeof(text),&length);
   if(got<0){rc=got;break;}
   if(got==1){
    if(!length||length>=sizeof(text)||text[length]||strlen(text)!=length||vv_chat_text_measure(text,sizeof(text)-1,NULL,NULL,NULL)){rc=-EPROTO;break;}
    rc=command(s,"continue-task",text,control,sizeof(control),&bytes);if(rc)break;
    rc=vv_wsi_queue(&s->ws,1,control,bytes);if(rc)break;
    if(!s->texts)s->first_text=now;
    ++s->texts;tx_deadline=now+10000;phase=now+22000;
   }else if(got==2){
    if(!s->texts){rc=-EPROTO;break;}
    rc=command(s,"finish-task",NULL,control,sizeof(control),&bytes);if(rc)break;
    rc=vv_wsi_queue(&s->ws,1,control,bytes);if(rc)break;
    s->state=2;s->finish_pending=true;tx_deadline=now+10000;phase=0;
   }else if(got){rc=-EPROTO;break;}
   else if(!s->texts)phase=s->ready+22000;
   vv_https_secure_zero(text,sizeof(text));vv_https_secure_zero(control,sizeof(control));
  }
  c->sleep_ms(c->sleep_arg,1);
 }
done:
 if(owned)vv_wsi_abort(&s->ws);else c->io.close(c->io.arg);
 if(!rc&&s->state!=3)rc=-EPROTO;
 out->http_result=rc;out->pcm_bytes=s->pcm;out->terminal_audio_complete=!rc;
 uint64_t ended=c->now_ms(c->clock_arg);
 printf("TTS_WSS rc=%d texts=%u pcm_blocks=%u bytes=%zu task_complete=%u ready_ms=%llu first_text_ms=%llu first_pcm_ms=%llu total_ms=%llu opus=%u wire_bytes=%zu wire_blocks=%u\n",rc,s->texts,s->blocks,s->pcm,(unsigned)!rc,
  (unsigned long long)(s->ready>=s->began?s->ready-s->began:0),
  (unsigned long long)(s->first_text>=s->began?s->first_text-s->began:0),
  (unsigned long long)(s->first_pcm>=s->began?s->first_pcm-s->began:0),
  (unsigned long long)(ended>=s->began&&ended!=UINT64_MAX?ended-s->began:0),c->opus?1u:0u,s->wire_bytes,s->wire_blocks);
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 if(c->opus){struct vv_opus_pcm_stats stats;vv_opus_pcm_get_stats(s->opus,&stats);
  printf("OPUS_STREAM rc=%d packets=%u pcm_bytes=%zu wire_bytes=%zu pre_skip_48k=%u eos=%u granule_untrusted=1 granule_mismatches=%llu\n",
   rc,stats.packets,stats.pcm_bytes,stats.wire_bytes,stats.pre_skip_48k,stats.eos,
   (unsigned long long)stats.granule_mismatches);
 }
 vv_opus_pcm_destroy(s->opus);
#endif
 vv_https_secure_zero(control,sizeof(control));vv_https_secure_zero(text,sizeof(text));vv_https_secure_zero(id,sizeof(id));
 vv_https_secure_zero(s,sizeof(*s));free(s);return rc;
}

#endif
