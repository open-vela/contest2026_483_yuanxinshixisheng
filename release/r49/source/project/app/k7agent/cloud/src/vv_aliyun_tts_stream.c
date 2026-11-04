#include "vv_aliyun_tts_stream.h"
#include "vv_aliyun_protocol.h"
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <string.h>

int vv_aliyun_tts_stream_json(const char *model,const char *voice,const char *text,
 char *out,size_t cap,size_t *written){
 int rc=vv_aliyun_tts_json(model,voice,text,out,cap,written);if(rc)return rc;
 cJSON *r=cJSON_Parse(out),*pcm=cJSON_CreateString("pcm");
 if(!r||!pcm){cJSON_Delete(r);cJSON_Delete(pcm);memset(out,0,cap);*written=0;return -ENOMEM;}
 cJSON *input=cJSON_GetObjectItemCaseSensitive(r,"input");
 if(!cJSON_ReplaceItemInObjectCaseSensitive(input,"format",pcm)){
  cJSON_Delete(pcm);cJSON_Delete(r);memset(out,0,cap);*written=0;return -EPROTO;
 }
 if(!cJSON_PrintPreallocated(r,out,(int)cap,0)){memset(out,0,cap);*written=0;rc=-ENOSPC;}
 else *written=strlen(out);
 cJSON_Delete(r);return rc;
}

static int fail(struct vv_aliyun_tts_stream *s,int rc){return s->error=rc;}
static int gate(struct vv_aliyun_tts_stream *s){
 if(s->error)return s->error;
 return s->cancelled&&s->cancelled(s->cancel_arg)?fail(s,-ECANCELED):0;
}
int vv_aliyun_tts_stream_init(struct vv_aliyun_tts_stream *s,size_t max,
 vv_aliyun_tts_pcm_fn sink,void *arg,bool (*cancel)(void *),void *ca){
 if(!s||!sink||!max||max%2||max>960000u)return -EINVAL;
 memset(s,0,sizeof(*s));s->max_bytes=max;s->sink=sink;s->sink_arg=arg;
 s->cancelled=cancel;s->cancel_arg=ca;return 0;
}
static int b64(unsigned char c){
 if(c>='A'&&c<='Z')return c-'A';
 if(c>='a'&&c<='z')return c-'a'+26;
 if(c>='0'&&c<='9')return c-'0'+52;
 return c=='+'?62:c=='/'?63:-1;
}
static int decode(const char *text,unsigned char *out,size_t *used){
 size_t n=strlen(text),at=0;if(n%4)return -EPROTO;
 for(size_t i=0;i<n;i+=4){
  int a=b64(text[i]),b=b64(text[i+1]);
  bool p=text[i+2]=='=',q=text[i+3]=='=';
  int c=p?0:b64(text[i+2]),d=q?0:b64(text[i+3]);
  if(a<0||b<0||c<0||d<0||(p&&!q)||((p||q)&&i+4!=n)||
     (p&&(b&15))||(!p&&q&&(c&3)))return -EPROTO;
  out[at++]=(unsigned char)((a<<2)|(b>>4));
  if(!p)out[at++]=(unsigned char)((b<<4)|(c>>2));
  if(!q)out[at++]=(unsigned char)((c<<6)|d);
 }
 *used=at;return 0;
}
static int audit(cJSON *v,unsigned depth){
 if(depth>16)return -EPROTO;
 for(cJSON *a=v->child;a;a=a->next){
  if(cJSON_IsObject(v))for(cJSON *b=a->next;b;b=b->next)
   if(a->string&&b->string&&!strcmp(a->string,b->string))return -EPROTO;
  if(audit(a,depth+1))return -EPROTO;
 }
 return 0;
}
static int event(struct vv_aliyun_tts_stream *s){
 if(!s->json_used)return s->error_event?-EIO:0;
 if(s->error_event)return -EIO;
 s->json[s->json_used]=0;
 if(!strcmp(s->json,"[DONE]"))return s->stop?0:-EPROTO;
 if(s->stop)return -EPROTO;
 /* Bound nesting before cJSON even when compiled with its larger default. */
 unsigned depth=0;bool quoted=false,escape=false;
 for(size_t i=0;i<s->json_used;i++){
  char c=s->json[i];
  if(quoted){if(escape){escape=false;continue;}if(c=='\\')escape=true;else if(c=='"')quoted=false;}
  else if(c=='"')quoted=true;
  else if(c=='{'||c=='['){if(++depth>16)return -EPROTO;}
  else if(c=='}'||c==']'){if(!depth)return -EPROTO;--depth;}
 }
 if(depth||quoted||strstr(s->json,"\\u0000"))return -EPROTO;
 const char *end=NULL;cJSON *r=cJSON_ParseWithLengthOpts(s->json,s->json_used+1,&end,1);
 if(!r)return -EPROTO;
 int rc=-EPROTO;
 if(!cJSON_IsObject(r)||audit(r,0))goto done;
 if(cJSON_GetObjectItemCaseSensitive(r,"code")||cJSON_GetObjectItemCaseSensitive(r,"error")){rc=-EIO;goto done;}
 cJSON *o=cJSON_GetObjectItemCaseSensitive(r,"output");
 if(!cJSON_IsObject(o))goto done;
 cJSON *f=cJSON_GetObjectItemCaseSensitive(o,"finish_reason");
 bool stop=cJSON_IsString(f)&&!strcmp(f->valuestring,"stop");
 if(f&&!cJSON_IsNull(f)&&!stop&&!(cJSON_IsString(f)&&!strcmp(f->valuestring,"null")))goto done;
 cJSON *audio=cJSON_GetObjectItemCaseSensitive(o,"audio");
 cJSON *data=cJSON_GetObjectItemCaseSensitive(audio,"data");
 if(audio&&!cJSON_IsObject(audio))goto done;
 if(data&&!cJSON_IsString(data)&&!cJSON_IsNull(data))goto done;
 size_t n=0;
 if(cJSON_IsString(data)&&*data->valuestring){
  rc=decode(data->valuestring,s->pcm+(s->pending?1:0),&n);if(rc)goto done;
  if(n>s->max_bytes-s->total){rc=-EOVERFLOW;goto done;}
  s->total+=n;s->audio_events++;
  if(s->pending){s->pcm[0]=s->odd;n++;s->pending=false;}
  if(n%2){s->odd=s->pcm[--n];s->pending=true;}
  if((rc=gate(s)))goto done;
  if(n&&(rc=s->sink(s->sink_arg,s->pcm,n))){if(rc>0)rc=-EIO;goto done;}
  if((rc=gate(s)))goto done;
 }
 if(stop){if(s->pending||!s->total){rc=-EPROTO;goto done;}s->stop=true;}
 s->events++;rc=0;
done:cJSON_Delete(r);return rc;
}
static int line(struct vv_aliyun_tts_stream *s){
 s->line[s->line_used]=0;
 if(!s->line_used){int rc=event(s);s->json_used=0;s->error_event=false;return rc;}
 if(s->line[0]==':')return 0;
 char *colon=strchr(s->line,':'),*value=colon?colon+1:s->line+s->line_used;
 if(colon)*colon=0;
 if(*value==' ')value++;
 if(!strcmp(s->line,"data")){
  size_t n=strlen(value),sep=s->json_used?1:0;
  if(n+sep>VV_ALIYUN_TTS_EVENT_MAX-s->json_used)return -EOVERFLOW;
  if(sep)s->json[s->json_used++]='\n';
  memcpy(s->json+s->json_used,value,n);s->json_used+=n;
 }else if(!strcmp(s->line,"event")&&!strcmp(value,"error"))s->error_event=true;
 return 0;
}
int vv_aliyun_tts_stream_feed(struct vv_aliyun_tts_stream *s,const unsigned char *data,size_t n){
 if(!s||!s->sink||(!data&&n))return -EINVAL;
 int rc=gate(s);if(rc)return rc;
 for(size_t i=0;i<n;i++){
  unsigned char c=data[i];
  if(s->cr){s->cr=false;if(c=='\n')continue;}
  if(c=='\r'||c=='\n'){
   s->cr=c=='\r';rc=line(s);s->line_used=0;
   if(rc)return fail(s,rc);
  }else{
   if(!c)return fail(s,-EPROTO);
   if(s->line_used==VV_ALIYUN_TTS_EVENT_MAX)return fail(s,-EOVERFLOW);
   s->line[s->line_used++]=(char)c;
  }
 }
 return gate(s);
}
int vv_aliyun_tts_stream_finish(struct vv_aliyun_tts_stream *s){
 if(!s||!s->sink)return -EINVAL;
 int rc=gate(s);if(rc)return rc;
 if(s->line_used||s->json_used||s->error_event||!s->stop||s->pending||!s->total)return fail(s,-EPROTO);
 return 0;
}
