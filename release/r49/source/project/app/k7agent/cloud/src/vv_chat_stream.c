#include "vv_chat_stream.h"
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

static bool whitespace(uint32_t c){
  return (c>=9&&c<=13)||c==32||c==0x85||c==0xa0||c==0x1680||
    (c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000;
}
/* Length is explicit, so an escaped NUL cannot hide a suffix from validation. */
static int utf8(const char *s,size_t bytes,size_t *count,bool *content){
  size_t at=0,n=0;bool any=false;
  while(at<bytes){
    uint32_t v=(unsigned char)s[at++],minimum=0;unsigned more=0;
    if(v<128){}
    else if(v>=0xc2&&v<=0xdf){more=1;minimum=0x80;v&=31;}
    else if(v>=0xe0&&v<=0xef){more=2;minimum=0x800;v&=15;}
    else if(v>=0xf0&&v<=0xf4){more=3;minimum=0x10000;v&=7;}
    else return VV_CHAT_PROTOCOL;
    if(more>bytes-at)return VV_CHAT_PROTOCOL;
    while(more--){unsigned b=(unsigned char)s[at++];if((b&0xc0)!=0x80)return VV_CHAT_PROTOCOL;v=(v<<6)|(b&63);}
    if(v<minimum||v>0x10ffff||(v>=0xd800&&v<=0xdfff)||
       !v||v==127||(v<32&&!whitespace(v)))return VV_CHAT_PROTOCOL;
    ++n;if(!whitespace(v))any=true;
  }
  if(count)*count=n;
  if(content)*content=any;
  return 0;
}
static int bounded(const char *s,size_t max,size_t *bytes){
  if(!s)return VV_CHAT_INVALID;
  size_t n=0;while(n<=max&&s[n])++n;
  if(n>max)return VV_CHAT_LIMIT;
  *bytes=n;return 0;
}
int vv_chat_text_measure(const char *text,size_t max,size_t *bytes,size_t *count,bool *content){
  if(bytes)*bytes=0;
  if(count)*count=0;
  if(content)*content=false;
  size_t n=0,scalars=0;bool any=false;int rc=bounded(text,max,&n);
  if(!rc)rc=utf8(text,n,&scalars,&any);
  if(rc)return rc;
  if(bytes)*bytes=n;
  if(count)*count=scalars;
  if(content)*content=any;
  return 0;
}
int vv_chat_text_utf16(const char *text,size_t max,size_t *bytes,size_t *units,bool *content){
  if(bytes)*bytes=0;
  if(units)*units=0;
  size_t n=0,count=0;int rc=vv_chat_text_measure(text,max,&n,&count,content);if(rc)return rc;
  /* Validation above guarantees every four-byte lead is one supplementary
   * scalar, represented by two code units in the existing Java DTO. */
  for(size_t i=0;i<n;i++)if((unsigned char)text[i]>=0xf0)count++;
  if(bytes)*bytes=n;
  if(units)*units=count;
  return 0;
}
int vv_chat_question_json(const char *text,char *out,size_t cap,size_t *written){
  if(written)*written=0;
  if(out&&cap)out[0]=0;
  if(!out||!cap||!written)return VV_CHAT_INVALID;
  size_t count=0;bool content=false;
  int rc=vv_chat_text_utf16(text,VV_CHAT_QUESTION_BYTES,NULL,&count,&content);
  if(rc)return rc==VV_CHAT_LIMIT?-E2BIG:rc;
  if(!content)return VV_CHAT_INVALID;
  if(count>VV_CHAT_QUESTION_UTF16_MAX)return -E2BIG;
  cJSON *root=cJSON_CreateObject();
  if(!root||!cJSON_AddStringToObject(root,"text",text)){cJSON_Delete(root);return VV_CHAT_MEMORY;}
  if(cap>VV_CHAT_REQUEST_MAX)cap=VV_CHAT_REQUEST_MAX;
  if(!cJSON_PrintPreallocated(root,out,(int)cap,0)){memset(out,0,cap);rc=VV_CHAT_LIMIT;}
  else *written=strlen(out);
  cJSON_Delete(root);return rc;
}
static int fail(struct vv_chat_stream *s,int error){
  memset(s->answer,0,sizeof(s->answer));s->answer_bytes=0;s->error=error;return error;
}
static int gate(struct vv_chat_stream *s){
  if(s->error)return s->error;
  return s->cancelled&&s->cancelled(s->cancel_arg)?fail(s,VV_CHAT_CANCELLED):0;
}
int vv_chat_stream_init(struct vv_chat_stream *s,bool (*cancelled)(void *),void *arg){
  if(!s)return VV_CHAT_INVALID;
  memset(s,0,sizeof(*s));s->cancelled=cancelled;s->cancel_arg=arg;return 0;
}
static int inspect(cJSON *v,unsigned depth){
  if(depth>16)return VV_CHAT_LIMIT;
  if(cJSON_IsString(v)&&utf8(v->valuestring,strlen(v->valuestring),NULL,NULL))return VV_CHAT_PROTOCOL;
  for(cJSON *a=v->child;a;a=a->next){
    if(cJSON_IsObject(v)){
      if(!a->string||utf8(a->string,strlen(a->string),NULL,NULL))return VV_CHAT_PROTOCOL;
      for(cJSON *b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return VV_CHAT_PROTOCOL;
      if(!strcmp(a->string,"mock")&&cJSON_IsTrue(a))return VV_CHAT_MOCK;
    }
    int rc=inspect(a,depth+1);if(rc)return rc;
  }
  return 0;
}
static int event(struct vv_chat_stream *s){
  if(!s->event_seen&&!s->data_seen)return 0;
  if(s->terminal||!s->event_seen||!s->data_seen)return VV_CHAT_PROTOCOL;
  if(++s->events>4096)return VV_CHAT_LIMIT;
  s->json[s->json_used]=0;
  int rc=utf8(s->json,s->json_used,NULL,NULL);if(rc)return rc;
  bool quoted=false;unsigned depth=0;
  for(size_t i=0;i<s->json_used;i++){
    char c=s->json[i];
    if(quoted&&c=='\\'){
      if(++i>=s->json_used)return VV_CHAT_PROTOCOL;
      if(s->json[i]=='u'&&i+4<s->json_used&&!memcmp(s->json+i+1,"0000",4))return VV_CHAT_PROTOCOL;
      continue;
    }
    if(c=='"')quoted=!quoted;
    else if(!quoted&&(c=='{'||c=='[')){if(++depth>16)return VV_CHAT_LIMIT;}
    else if(!quoted&&(c=='}'||c==']')){if(!depth)return VV_CHAT_PROTOCOL;--depth;}
  }
  if(quoted||depth)return VV_CHAT_PROTOCOL;
  const char *end=NULL;cJSON *root=cJSON_ParseWithLengthOpts(s->json,s->json_used+1,&end,1);
  if(!root||!cJSON_IsObject(root)){cJSON_Delete(root);return VV_CHAT_PROTOCOL;}
  rc=inspect(root,0);if(rc)goto done;
  if(!strcmp(s->event,"response.accepted")){rc=0;goto done;}
  if(!strcmp(s->event,"response.delta")){
    cJSON *delta=cJSON_GetObjectItemCaseSensitive(root,"delta");size_t count=0;
    if(!cJSON_IsString(delta)){rc=VV_CHAT_PROTOCOL;goto done;}
    rc=utf8(delta->valuestring,strlen(delta->valuestring),&count,NULL);if(rc)goto done;
    if(count>100000u-s->delta_scalars){rc=VV_CHAT_LIMIT;goto done;}
    s->delta_scalars+=count;goto done;
  }
  if(!strcmp(s->event,"response.completed")){
    cJSON *answer=cJSON_GetObjectItemCaseSensitive(root,"answerText");bool content=false;
    if(!cJSON_IsString(answer)){rc=VV_CHAT_PROTOCOL;goto done;}
    size_t n=strlen(answer->valuestring);
    if(n>VV_CHAT_ANSWER_MAX){rc=VV_CHAT_LIMIT;goto done;}
    rc=utf8(answer->valuestring,n,NULL,&content);if(rc)goto done;
    if(!content){rc=VV_CHAT_PROTOCOL;goto done;}
    memcpy(s->answer,answer->valuestring,n+1);s->answer_bytes=n;s->terminal=true;goto done;
  }
  if(!strcmp(s->event,"response.failed")){
    cJSON *code=cJSON_GetObjectItemCaseSensitive(root,"code");
    rc=VV_CHAT_FAILED;
    if(cJSON_IsString(code)&&!strcmp(code->valuestring,"DEPENDENCY_TIMEOUT"))rc=VV_CHAT_DEPENDENCY_TIMEOUT;
    else if(cJSON_IsString(code)&&!strcmp(code->valuestring,"DEPENDENCY_UNAVAILABLE"))rc=VV_CHAT_DEPENDENCY_UNAVAILABLE;
    goto done;
  }
  rc=VV_CHAT_PROTOCOL;
done:cJSON_Delete(root);return rc;
}
static int line(struct vv_chat_stream *s){
  s->line[s->line_used]=0;
  if(!s->line_used){
    int rc=event(s);s->json_used=0;s->event_seen=false;s->data_seen=false;s->event[0]=0;return rc;
  }
  if(s->line[0]==':')return 0;
  char *colon=strchr(s->line,':'),*value=colon?colon+1:s->line+s->line_used;
  if(colon)*colon=0;
  if(*value==' ')++value;
  if(!strcmp(s->line,"data")){
    size_t n=strlen(value),sep=s->data_seen?1u:0u;
    if(n+sep>VV_CHAT_EVENT_MAX-s->json_used)return VV_CHAT_LIMIT;
    if(sep)s->json[s->json_used++]='\n';
    memcpy(s->json+s->json_used,value,n);s->json_used+=n;s->data_seen=true;return 0;
  }
  if(!strcmp(s->line,"event")){
    size_t n=strlen(value);
    if(s->event_seen||n>=sizeof(s->event))return VV_CHAT_PROTOCOL;
    memcpy(s->event,value,n+1);s->event_seen=true;return 0;
  }
  return !strcmp(s->line,"id")||!strcmp(s->line,"retry")?0:VV_CHAT_PROTOCOL;
}
int vv_chat_stream_feed(struct vv_chat_stream *s,const void *bytes,size_t n){
  if(!s||(!bytes&&n))return VV_CHAT_INVALID;
  int rc=gate(s);if(rc)return rc;
  if(n>VV_CHAT_WIRE_MAX-s->wire_bytes)return fail(s,VV_CHAT_LIMIT);
  s->wire_bytes+=n;const unsigned char *p=bytes;
  for(size_t i=0;i<n;i++){
    unsigned char c=p[i];
    if(s->cr){s->cr=false;if(c=='\n')continue;}
    if(c=='\r'||c=='\n'){
      s->cr=c=='\r';rc=line(s);s->line_used=0;
      if(rc)return fail(s,rc);
      if((rc=gate(s)))return rc;
    }else{
      if(!c)return fail(s,VV_CHAT_PROTOCOL);
      if(s->line_used==VV_CHAT_EVENT_MAX)return fail(s,VV_CHAT_LIMIT);
      s->line[s->line_used++]=(char)c;
    }
  }
  return gate(s);
}
int vv_chat_stream_finish(struct vv_chat_stream *s,char *out,size_t cap){
  if(out&&cap)memset(out,0,cap);
  if(!s||!out||!cap)return VV_CHAT_INVALID;
  int rc=gate(s);if(rc)return rc;
  if(s->line_used||s->event_seen||s->data_seen||!s->terminal)return fail(s,VV_CHAT_PROTOCOL);
  if(s->answer_bytes>=cap)return fail(s,VV_CHAT_LIMIT);
  memcpy(out,s->answer,s->answer_bytes+1);return 0;
}
