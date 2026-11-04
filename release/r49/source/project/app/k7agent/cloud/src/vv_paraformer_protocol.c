#include "vv_paraformer.h"
#include "device_voice_intent.h"
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* User-authorized final transcript display: bounded, one terminal-safe line. */
static void log_final_text(const char *text,size_t bytes){
 char safe[257];size_t used=0;
 for(size_t i=0;i<bytes;){
  unsigned char b=(unsigned char)text[i];size_t n=1;unsigned cp=b,min=0;
  if(b>=0xc2&&b<=0xdf){n=2;cp=b&31;min=0x80;}
  else if(b>=0xe0&&b<=0xef){n=3;cp=b&15;min=0x800;}
  else if(b>=0xf0&&b<=0xf4){n=4;cp=b&7;min=0x10000;}
  bool valid=b<0x80||n>1;
  if(i+n>bytes)valid=false;
  for(size_t j=1;valid&&j<n;j++){
   unsigned char next=(unsigned char)text[i+j];
   if((next&0xc0)!=0x80)valid=false;else cp=(cp<<6)|(next&63);
  }
  if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))valid=false;
  if(!valid){n=1;cp='?';}
  bool replace=!valid||cp<32||(cp>=127&&cp<=159)||
    cp==0x2028||cp==0x2029||(cp>=0x202a&&cp<=0x202e)||
    (cp>=0x2066&&cp<=0x2069);
  size_t add=replace?1:n;if(used+add>sizeof(safe)-1)break;
  if(replace)safe[used++]='?';else{memcpy(safe+used,text+i,n);used+=n;}
  i+=n;
 }
 safe[used]=0;printf("ASR_FINAL text=%s\n",safe);fflush(stdout);
}
static int output(char *out,size_t cap,size_t *n,int length){
 (void)out;
 if(length<0||(size_t)length>=cap)return -ENOSPC;
 *n=(size_t)length;return 0;
}
unsigned vv_pf_command(const struct vv_pf_protocol *s){
 if(!s||s->state!=3)return K7_VOICE_NONE;
 if(s->command_stop)return K7_VOICE_STOP;
 if(s->command_quiet)return K7_VOICE_QUIET;
 return s->command_veto||s->command_conflict?K7_VOICE_NONE:s->command_intent;
}
int vv_pf_start(struct vv_pf_protocol *s,const uint8_t random[16],char *out,size_t cap,size_t *n){
 if(!s||!random||!out||!n)return -EINVAL;
 memset(s,0,sizeof(*s));uint8_t b[16];memcpy(b,random,16);b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;
 snprintf(s->task_id,sizeof(s->task_id),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
 return output(out,cap,n,snprintf(out,cap,"{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"task_group\":\"audio\",\"task\":\"asr\",\"function\":\"recognition\",\"model\":\"paraformer-realtime-v2\",\"parameters\":{\"format\":\"pcm\",\"sample_rate\":16000,\"semantic_punctuation_enabled\":false,\"max_sentence_silence\":600},\"input\":{}}}",s->task_id));
}
int vv_pf_finish(struct vv_pf_protocol *s,char *out,size_t cap,size_t *n){
 if(!s||s->state!=1||!out||!n)return -EINVAL;
 int rc=output(out,cap,n,snprintf(out,cap,"{\"header\":{\"action\":\"finish-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}",s->task_id));
 if(!rc)s->state=2;
 return rc;
}
static cJSON *get(cJSON *o,const char *key){return cJSON_GetObjectItemCaseSensitive(o,key);}
static int equal(cJSON *o,const char *value){return cJSON_IsString(o)&&o->valuestring&&!strcmp(o->valuestring,value);}
static int unique(cJSON *o){
 if(!o)return 1;
 for(cJSON *a=o->child;a;a=a->next){
  if(a->string)for(cJSON *b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return 0;
  if(!unique(a))return 0;
 }
 return 1;
}
int vv_pf_event(struct vv_pf_protocol *s,const void *data,size_t n){
 if(!s||s->state>=3||!data||!n||n>VV_PF_RX_CAP)return -EINVAL;
 /* Reject hidden string suffixes. JSON size and linked cJSON depth are bounded. */
 const char *raw=data;
 unsigned depth=0;bool quoted=false,escaped=false;
 for(size_t i=0;i<n;i++){
  unsigned char ch=(unsigned char)raw[i];
  if(quoted){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='"')quoted=false;}
  else if(ch=='"')quoted=true;
  else if(ch=='{'||ch=='['){if(++depth>16){s->state=4;return -EPROTO;}}
  else if(ch=='}'||ch==']'){if(!depth){s->state=4;return -EPROTO;}depth--;}
 }
 if(quoted||depth){s->state=4;return -EPROTO;}
 if(memchr(raw,0,n)){s->state=4;return -EPROTO;}
 for(size_t i=0;i+5<n;i++)if(!memcmp(raw+i,"\\u0000",6)){s->state=4;return -EPROTO;}
 char *copy=malloc(n+1);if(!copy)return -ENOMEM;memcpy(copy,raw,n);copy[n]=0;
 cJSON *o=cJSON_ParseWithLengthOpts(copy,n+1,NULL,1);free(copy);
 int rc=-EPROTO;
 if(!cJSON_IsObject(o)||!unique(o))goto done;
 cJSON *h=get(o,"header");if(!cJSON_IsObject(h)||!equal(get(h,"task_id"),s->task_id))goto done;
 cJSON *event=get(h,"event");
 if(equal(event,"task-failed")){rc=-EACCES;goto done;} /* No auth/config retry. */
 if(equal(event,"task-started")){if(s->state)goto done;s->state=1;rc=0;goto done;}
 if(equal(event,"task-finished")){if(s->state!=2||s->pending_partial)goto done;s->state=3;rc=0;goto done;}
 if(!equal(event,"result-generated")||(s->state!=1&&s->state!=2))goto done;
 cJSON *sentence=get(get(get(o,"payload"),"output"),"sentence");
 if(!cJSON_IsObject(sentence))goto done;
 cJSON *hb=get(sentence,"heartbeat");if(cJSON_IsTrue(hb)){rc=0;goto done;}
 cJSON *text=get(sentence,"text"),*end=get(sentence,"sentence_end"),*begin=get(sentence,"begin_time");
 if(!cJSON_IsString(text)||!text->valuestring||!cJSON_IsBool(end)||!cJSON_IsNumber(begin)||begin->valuedouble<0||begin->valuedouble>60000||begin->valuedouble!=(uint32_t)begin->valuedouble)goto done;
 size_t bytes=strlen(text->valuestring);if(bytes>=sizeof(s->text))goto done;
 /* Numeric diagnostics and explicitly authorized final transcript display.
  * Log every cloud final, including duplicates, before deduplication. */
 if(cJSON_IsTrue(end)){
  log_final_text(text->valuestring,bytes);
  size_t characters=0;
  for(size_t j=0;j<bytes;j++)if(((unsigned char)text->valuestring[j]&0xc0)!=0x80)characters++;
  printf("PARAFORMER_FINAL bytes=%lu chars=%lu intent=%d prior_finals=%u\n",
   (unsigned long)bytes,(unsigned long)characters,
   (int)k7_voice_completed_text(text->valuestring,bytes,true,true),s->finals);fflush(stdout);
 }

 if(!cJSON_IsTrue(end)){s->pending_partial=bytes!=0;rc=0;goto done;}
 uint32_t stamp=(uint32_t)begin->valuedouble;
 if(s->finals&&stamp<=s->last_begin){
  if(stamp==s->last_begin&&!strcmp(s->text+s->last_offset,text->valuestring)){s->pending_partial=false;rc=0;}
  goto done;
 }
 if(bytes>=sizeof(s->text)-s->used)goto done;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
 /* Resolve all delivered finals, including ones arriving while draining.
  * A negative/unknown nonempty final vetoes affirmative motion; STOP wins.
  * Repeated identical commands are one admission, never multiple actions. */
 if(bytes){
  unsigned intent=(unsigned)k7_voice_completed_text(text->valuestring,bytes,true,true);
  if(intent==K7_VOICE_STOP)s->command_stop=true;
  else if(intent==K7_VOICE_QUIET)s->command_quiet=true;
  else if(!intent)s->command_veto=true;
  else if(s->command_intent&&s->command_intent!=intent)s->command_conflict=true;
  else s->command_intent=intent;
  if(intent)s->command_seen=true;
 }
#endif
 s->last_offset=s->used;memcpy(s->text+s->used,text->valuestring,bytes+1);s->used+=bytes;
 s->last_begin=stamp;s->finals++;s->pending_partial=false;rc=0;
done:
 cJSON_Delete(o);if(rc)s->state=4;return rc;
}
