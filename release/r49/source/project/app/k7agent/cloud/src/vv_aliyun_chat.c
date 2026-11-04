#include "vv_aliyun_chat.h"
#include "vv_aliyun_endpoint.h"
#include "vv_chat_search_policy.h"
#include "../../../k7radio/cJSON.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
#include "vv_reply_json.h"
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Protocol checked against the primary Model Studio Chat Completions docs:
 * https://help.aliyun.com/zh/model-studio/qwen-api-via-openai-chat-completions
 * Existing Beijing dashscope domain remains supported. Selected current-turn
 * questions use server-side turbo web search; no client tool/device dispatch.
 * https://help.aliyun.com/zh/model-studio/web-search */
static const char system_prompt[] =
 "你是OpenVela桌面语音助手。用自然简洁中文回答，通常一至三句，不超过120字，"
 "不用Markdown，不输出思考过程。设备具备云台人脸跟踪、三视图测肤采集上传功能。"
 "你只负责对话，不能执行设备操作，不要声称已执行动作或生成真实皮肤报告。"
 "历史问答只作上下文，请回答当前问题。";
static const char minute_story_prompt[] =
 "你是OpenVela桌面语音助手。当前用户明确要求约一分钟的故事。"
 "请用自然中文讲一个约280至320个汉字的完整故事，含开头、发展和结尾，"
 "适合以自然语速讲述约一分钟。直接讲故事，不用Markdown，不输出思考过程或字数说明。"
 "设备具备云台人脸跟踪、三视图测肤采集上传功能。"
 "你只负责对话，不能执行设备操作，不要声称已执行动作或生成真实皮肤报告。"
 "历史问答只作上下文，请回答当前问题。";
/* Search request capability is distinct from successful retrieval. Keep the
 * ordinary/story prompts byte-identical and never claim unverified results. */
static const char search_prompt[] =
 "你是OpenVela桌面语音助手。用自然简洁中文回答，通常一至三句，不超过120字，"
 "不用Markdown，不输出思考过程。设备具备云台人脸跟踪、三视图测肤采集上传功能。"
 "你只负责对话，不能执行设备操作，不要声称已执行动作或生成真实皮肤报告。"
 "历史问答只作上下文，请回答当前问题。"
 "本轮已请求服务端联网检索，请依据本轮实际提供的检索资料回答。"
 "历史中不能联网的说法不代表本轮能力。若资料缺失、不足或过时，明确说暂未查到可核实的最新信息，"
 "不要将本次未查到说成设备不具备联网功能，也不要编造数据、来源或检索成功。"
 "时效信息简要注明来源及资料日期；股价注明行情时点，不把历史或延迟行情当实时价格。";
static bool minute_story(const char *text)
{
 /* vv_dialogue appends the current question after all history and terminates
  * it with this assistant label. Never select a duration from old turns. */
 const char *begin=text,*end=text+strlen(text),*mark;
 const char current[]="当前用户：",tail[]="\n助手：";
 if((size_t)(end-text)>=sizeof(tail)-1&&!memcmp(end-(sizeof(tail)-1),tail,sizeof(tail)-1)){
  end-=sizeof(tail)-1;
  if(!strncmp(text,current,sizeof(current)-1))begin=text+sizeof(current)-1;
  const char label[]="\n当前用户：";
  const char *scan=text;
  while((mark=strstr(scan,label))&&mark<end){begin=mark+sizeof(label)-1;scan=begin;}
 }
 mark=strstr(begin,"故事");if(!mark||mark+strlen("故事")>end)return false;
 const char *const durations[]={"一分钟","1分钟","60秒","六十秒"};
 for(size_t i=0;i<sizeof(durations)/sizeof(*durations);i++){
  const char *scan=begin;
  while((mark=strstr(scan,durations[i]))&&mark+strlen(durations[i])<=end){
   /* Do not read 11 minutes or 160 seconds as one minute. Chinese number
    * prefixes are excluded too (十一分钟/一百六十秒). */
   bool longer=mark>begin&&mark[-1]>='0'&&mark[-1]<='9';
   const char *const numbers[]={"零","一","二","三","四","五","六","七","八","九","十","百","千","万","两"};
   for(size_t j=0;!longer&&j<sizeof(numbers)/sizeof(*numbers);j++){
    size_t n=strlen(numbers[j]);
    if((size_t)(mark-begin)>=n&&!memcmp(mark-n,numbers[j],n))longer=true;
   }
   if(!longer)return true;
   scan=mark+strlen(durations[i]);
  }
 }
 return false;
}
static bool cancelled(const struct vv_https_cancel *c)
{ return c && c->is_cancelled && c->is_cancelled(c->arg); }
static int quoted(char *out,size_t cap,size_t *at,const char *text)
{
 if(*at>=cap-1)return VV_CHAT_LIMIT;
 out[(*at)++]='"';
 for(const unsigned char *p=(const unsigned char *)text;*p;p++){
  char escape[7];const char *bytes=(const char *)p;size_t n=1;
  if(*p=='"'||*p=='\\'){escape[0]='\\';escape[1]=(char)*p;bytes=escape;n=2;}
  else if(*p<32){snprintf(escape,sizeof(escape),"\\u%04x",(unsigned)*p);bytes=escape;n=6;}
  if(n>cap-1-*at)return VV_CHAT_LIMIT;
  memcpy(out+*at,bytes,n);*at+=n;
 }
 if(*at>=cap-1)return VV_CHAT_LIMIT;
 out[(*at)++]='"';out[*at]=0;return 0;
}
int vv_aliyun_chat_request(const char *text,char *out,size_t cap,size_t *bytes)
{
 if(bytes)*bytes=0;
 if(out&&cap)vv_https_secure_zero(out,cap);
 if(!out||!cap||!bytes)return VV_CHAT_INVALID;
 size_t units=0;bool content=false;
 int rc=vv_chat_text_utf16(text,VV_CHAT_QUESTION_BYTES,NULL,&units,&content);
 if(rc)return rc;
 if(!content)return VV_CHAT_INVALID;
 if(units>VV_CHAT_QUESTION_UTF16_MAX)return VV_CHAT_LIMIT;
 const char prefix[]="{\"model\":\"qwen-flash\",\"stream\":true,\"enable_thinking\":false,\"max_tokens\":256,\"messages\":[{\"role\":\"system\",\"content\":";
 const char story_prefix[]="{\"model\":\"qwen-flash\",\"stream\":true,\"enable_thinking\":false,\"max_tokens\":1024,\"messages\":[{\"role\":\"system\",\"content\":";
 const char search_prefix[]="{\"model\":\"qwen-flash\",\"stream\":true,\"enable_thinking\":false,\"max_tokens\":256,\"enable_search\":true,\"search_options\":{\"forced_search\":true,\"search_strategy\":\"turbo\"},\"include_tool_usage\":true,\"stream_options\":{\"include_usage\":true},\"messages\":[{\"role\":\"system\",\"content\":";
 const char middle[]="},{\"role\":\"user\",\"content\":";
 bool story=minute_story(text);
 bool search=!story && vv_chat_wants_search(text);
 const char *selected_prefix=story?story_prefix:search?search_prefix:prefix;
 size_t at=strlen(selected_prefix);
 if(at>=cap)return VV_CHAT_LIMIT;
 memcpy(out,selected_prefix,at);
 rc=quoted(out,cap,&at,story?minute_story_prompt:search?search_prompt:system_prompt);if(rc)goto fail;
 if(sizeof(middle)-1>cap-1-at){rc=VV_CHAT_LIMIT;goto fail;}
 memcpy(out+at,middle,sizeof(middle)-1);at+=sizeof(middle)-1;
 rc=quoted(out,cap,&at,text);if(rc)goto fail;
 if(3>cap-1-at){rc=VV_CHAT_LIMIT;goto fail;}
 memcpy(out+at,"}]}",4);*bytes=at+3;return 0;
fail:vv_https_secure_zero(out,cap);return rc;
}
static int fail(struct vv_aliyun_chat_stream *s,int rc)
{ vv_https_secure_zero(s->answer,sizeof(s->answer));s->answer_bytes=0;s->error=rc;return rc; }
static int gate(struct vv_aliyun_chat_stream *s)
{ return s->error?s->error:cancelled(s->cancel)?fail(s,VV_HTTPS_ECANCELLED):0; }
void vv_aliyun_chat_stream_init(struct vv_aliyun_chat_stream *s,const struct vv_https_cancel *c)
{ vv_aliyun_chat_stream_init_delta(s,c,NULL,NULL); }
void vv_aliyun_chat_stream_init_delta(struct vv_aliyun_chat_stream *s,const struct vv_https_cancel *c,
 vv_aliyun_chat_delta_fn on_delta,void *delta_arg)
{ if(s){memset(s,0,sizeof(*s));s->cancel=c;s->on_delta=on_delta;s->delta_arg=delta_arg;} }
static int inspect(const cJSON *v,unsigned depth)
{
 if(depth>16)return VV_CHAT_LIMIT;
 if(cJSON_IsString(v)&&vv_chat_text_measure(v->valuestring,VV_ALIYUN_CHAT_EVENT_MAX,NULL,NULL,NULL))return VV_CHAT_PROTOCOL;
 for(const cJSON *a=v->child;a;a=a->next){
  if(cJSON_IsObject(v)){
   if(!a->string||vv_chat_text_measure(a->string,VV_ALIYUN_CHAT_EVENT_MAX,NULL,NULL,NULL))return VV_CHAT_PROTOCOL;
   for(const cJSON *b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return VV_CHAT_PROTOCOL;
  }
  int rc=inspect(a,depth+1);if(rc)return rc;
 }
 return 0;
}
static cJSON *field(const cJSON *v,const char *name){return cJSON_GetObjectItemCaseSensitive(v,name);}
static bool present(const cJSON *v){return v&&!cJSON_IsNull(v);}
static int json_event(struct vv_aliyun_chat_stream *s)
{
 if(!s->data_seen)return 0;
 if(s->done||++s->events>4096)return VV_CHAT_PROTOCOL;
 s->event[s->event_bytes]=0;
 if(!strcmp(s->event,"[DONE]")){
  if(!s->stopped)return VV_CHAT_PROTOCOL;
  s->done=true;return 0;
 }
 if(vv_chat_text_measure(s->event,s->event_bytes,NULL,NULL,NULL))return VV_CHAT_PROTOCOL;
 /* Bound nesting before cJSON recursion and reject escaped NUL truncation. */
 bool in_string=false;unsigned depth=0;
 for(size_t i=0;i<s->event_bytes;i++){
  char c=s->event[i];
  if(in_string&&c=='\\'){
   if(++i>=s->event_bytes)return VV_CHAT_PROTOCOL;
   if(s->event[i]=='u'&&i+4<s->event_bytes&&!memcmp(s->event+i+1,"0000",4))return VV_CHAT_PROTOCOL;
   continue;
  }
  if(c=='"')in_string=!in_string;
  else if(!in_string&&(c=='{'||c=='[')){if(++depth>16)return VV_CHAT_LIMIT;}
  else if(!in_string&&(c=='}'||c==']')){if(!depth)return VV_CHAT_PROTOCOL;--depth;}
 }
 if(in_string||depth)return VV_CHAT_PROTOCOL;
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
 cJSON *root=vv_reply_json_parse(s->event,s->event_bytes);
#else
 const char *end=NULL;cJSON *root=cJSON_ParseWithLengthOpts(s->event,s->event_bytes+1,&end,1);
#endif
 if(!root||!cJSON_IsObject(root)){cJSON_Delete(root);return VV_CHAT_PROTOCOL;}
 int rc=inspect(root,0);if(rc)goto done;
 if(present(field(root,"error"))){rc=VV_CHAT_FAILED;goto done;}
 cJSON *id=field(root,"id"),*object=field(root,"object"),*choices=field(root,"choices");
 if(!cJSON_IsString(id)||!id->valuestring[0]||strlen(id->valuestring)>=sizeof(s->id)||
    !cJSON_IsString(object)||strcmp(object->valuestring,"chat.completion.chunk")||!cJSON_IsArray(choices)){
  rc=VV_CHAT_PROTOCOL;goto done;
 }
 if(s->id[0]&&strcmp(s->id,id->valuestring)){rc=VV_CHAT_PROTOCOL;goto done;}
 if(!s->id[0])strcpy(s->id,id->valuestring);
 int count=cJSON_GetArraySize(choices);
 if(count==0){
  if(!s->stopped||s->usage_seen||!cJSON_IsObject(field(root,"usage")))rc=VV_CHAT_PROTOCOL;
  else {
   s->usage_seen=true;
   /* Observation only: a tool count means search intent, not usable results.
    * Optional metadata must never relax or change stream validation. */
   cJSON *usage=field(root,"usage");
   cJSON *tools=field(usage,"x_tools");
   {
    cJSON *search=field(tools,"search");
    cJSON *number=field(search,"count"),*tokens=field(usage,"prompt_tokens");
    bool number_ok=cJSON_IsNumber(number)&&number->valuedouble>=0&&number->valuedouble<=2147483647.0;
    bool tokens_ok=cJSON_IsNumber(tokens)&&tokens->valuedouble>=0&&tokens->valuedouble<=2147483647.0;
    number_ok=number_ok&&number->valuedouble==(unsigned)number->valuedouble;
    tokens_ok=tokens_ok&&tokens->valuedouble==(unsigned)tokens->valuedouble;
    printf("CHAT_USAGE count_present=%u count=%u prompt_tokens_present=%u prompt_tokens=%u retrieval_verified=0\n",
     (unsigned)number_ok,number_ok?(unsigned)number->valuedouble:0u,
     (unsigned)tokens_ok,tokens_ok?(unsigned)tokens->valuedouble:0u);fflush(stdout);
   }
  }
  goto done;
 }
 if(count!=1||s->stopped){rc=VV_CHAT_PROTOCOL;goto done;}
 cJSON *choice=cJSON_GetArrayItem(choices,0),*delta=field(choice,"delta"),*index=field(choice,"index"),*finish=field(choice,"finish_reason");
 if(!cJSON_IsObject(choice)||!cJSON_IsObject(delta)||!cJSON_IsNumber(index)||index->valuedouble!=0||!finish){rc=VV_CHAT_PROTOCOL;goto done;}
 cJSON *role=field(delta,"role"),*text=field(delta,"content"),*reasoning=field(delta,"reasoning_content");
 if((present(role)&&(!cJSON_IsString(role)||strcmp(role->valuestring,"assistant")))||
    present(field(delta,"tool_calls"))||present(field(delta,"function_call"))||
    present(field(delta,"refusal"))||present(field(delta,"audio"))||
    (present(reasoning)&&(!cJSON_IsString(reasoning)||reasoning->valuestring[0]))){rc=VV_CHAT_PROTOCOL;goto done;}
 size_t text_bytes=0;
 if(present(text)){
  if(!cJSON_IsString(text)){rc=VV_CHAT_PROTOCOL;goto done;}
  text_bytes=strlen(text->valuestring);
  if(text_bytes>VV_CHAT_ANSWER_MAX-s->answer_bytes){rc=VV_CHAT_LIMIT;goto done;}
 }
 if(!cJSON_IsNull(finish)){
  if(!cJSON_IsString(finish)||strcmp(finish->valuestring,"stop")){rc=VV_CHAT_FAILED;goto done;}
  s->stopped=true;
 }
 /* Dispatch only after the entire event, including finish_reason, has passed
  * validation. inspect() validated every decoded string's UTF-8 above. */
 if(text_bytes){
  memcpy(s->answer+s->answer_bytes,text->valuestring,text_bytes+1);s->answer_bytes+=text_bytes;
  if(s->on_delta){
   if((rc=gate(s)))goto done;
   s->emitting_delta=true;
   int accepted=s->on_delta(s->delta_arg,text->valuestring,text_bytes);
   s->emitting_delta=false;
   rc=gate(s);
   if(!rc&&accepted)rc=accepted<0?accepted:VV_CHAT_FAILED;
  }
 }
done:cJSON_Delete(root);return rc;
}
static int line(struct vv_aliyun_chat_stream *s)
{
 s->line[s->line_bytes]=0;
 if(!s->line_bytes){int rc=json_event(s);s->event_bytes=0;s->data_seen=false;return rc;}
 if(s->line[0]==':')return 0;
 char *colon=strchr(s->line,':'),*value=colon?colon+1:s->line+s->line_bytes;
 if(colon)*colon=0;
 if(*value==' ')value++;
 if(!strcmp(s->line,"data")){
  if(s->done)return VV_CHAT_PROTOCOL;
  size_t n=strlen(value),sep=s->data_seen?1u:0u;
  if(n+sep>VV_ALIYUN_CHAT_EVENT_MAX-s->event_bytes)return VV_CHAT_LIMIT;
  if(sep)s->event[s->event_bytes++]='\n';
  memcpy(s->event+s->event_bytes,value,n);s->event_bytes+=n;s->data_seen=true;return 0;
 }
 /* OpenAI-compatible chunks have no named events; reject error-event streams. */
 return !strcmp(s->line,"id")||!strcmp(s->line,"retry")?0:VV_CHAT_PROTOCOL;
}
int vv_aliyun_chat_stream_feed(struct vv_aliyun_chat_stream *s,const void *data,size_t n)
{
 if(!s||(!data&&n))return VV_CHAT_INVALID;
 if(s->emitting_delta)return fail(s,VV_CHAT_PROTOCOL);
 int rc=gate(s);if(rc)return rc;
 if(n>VV_ALIYUN_CHAT_WIRE_MAX-s->wire_bytes)return fail(s,VV_CHAT_LIMIT);
 s->wire_bytes+=n;const unsigned char *p=data;
 for(size_t i=0;i<n;i++){
  unsigned char c=p[i];
  if(!c)return fail(s,VV_CHAT_PROTOCOL);
  if(s->cr){s->cr=false;if(c=='\n')continue;}
  if(c=='\r'||c=='\n'){
   s->cr=c=='\r';rc=line(s);s->line_bytes=0;if(rc)return fail(s,rc);
  }else{
   if(s->line_bytes>=VV_ALIYUN_CHAT_EVENT_MAX)return fail(s,VV_CHAT_LIMIT);
   s->line[s->line_bytes++]=(char)c;
  }
 }
 return gate(s);
}
int vv_aliyun_chat_stream_finish(struct vv_aliyun_chat_stream *s,char *out,size_t cap)
{
 if(out&&cap)vv_https_secure_zero(out,cap);
 if(!s||!out||!cap)return VV_CHAT_INVALID;
 if(s->emitting_delta)return fail(s,VV_CHAT_PROTOCOL);
 int rc=gate(s);if(rc)return rc;
 /* A cut off event is not a normal terminal even if it contains [DONE]. */
 if(s->line_bytes||s->data_seen||!s->stopped||!s->done)return fail(s,VV_CHAT_PROTOCOL);
 bool content=false;rc=vv_chat_text_measure(s->answer,VV_CHAT_ANSWER_MAX,NULL,NULL,&content);
 if(rc||!content)return fail(s,VV_CHAT_PROTOCOL);
 if(s->answer_bytes>=cap)return fail(s,VV_CHAT_LIMIT);
 memcpy(out,s->answer,s->answer_bytes+1);return 0;
}
struct body {const char *json;size_t bytes;};
static ssize_t body_read(void *arg,size_t offset,void *out,size_t cap)
{ struct body *b=arg;if(offset>b->bytes)return -1;size_t n=b->bytes-offset;if(n>cap)n=cap;memcpy(out,b->json+offset,n);return (ssize_t)n; }
struct sink {struct vv_aliyun_chat_stream *stream;struct vv_https_response *response;int error;};
static bool mime(const char *s)
{ const char *m="text/event-stream";while(*m){unsigned char c=(unsigned char)*s++;if(c>='A'&&c<='Z')c+=32;if(c!=(unsigned char)*m++)return false;}return !*s||*s==';'; }
static int receive(void *arg,const void *data,size_t bytes)
{
 struct sink *s=arg;
 if(s->response->status_code!=200||!mime(s->response->content_type))return s->error=VV_CHAT_DEPENDENCY_UNAVAILABLE;
 return s->error=vv_aliyun_chat_stream_feed(s->stream,data,bytes);
}
int vv_aliyun_chat_perform(const struct vv_https_client *client,const char *key,const char *text,
 char *answer,size_t cap,const struct vv_https_cancel *cancel)
{
 return vv_aliyun_chat_perform_stream(client,key,text,answer,cap,cancel,NULL,NULL);
}
int vv_aliyun_chat_perform_stream(const struct vv_https_client *client,const char *key,const char *text,
 char *answer,size_t cap,const struct vv_https_cancel *cancel,vv_aliyun_chat_delta_fn on_delta,void *delta_arg)
{
 return vv_aliyun_chat_perform_stream_at_host(client,VV_ALIYUN_LEGACY_HOST,key,text,answer,cap,cancel,on_delta,delta_arg);
}
int vv_aliyun_chat_perform_at_host(const struct vv_https_client *client,const char *host,const char *key,const char *text,
 char *answer,size_t cap,const struct vv_https_cancel *cancel)
{
 return vv_aliyun_chat_perform_stream_at_host(client,host,key,text,answer,cap,cancel,NULL,NULL);
}
/* Authorized transcript diagnostics. Flatten controls so text cannot inject
 * log records or terminal escapes. Never log credentials, headers or host. */
static void search_text_log(const char *label,const char *text)
{
 size_t bytes=strlen(text);printf("%s text=",label);
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
  if(replace)putchar('?');else (void)fwrite(text+i,1,n,stdout);
  i+=n;
 }
 putchar('\n');fflush(stdout);
}
int vv_aliyun_chat_perform_stream_at_host(const struct vv_https_client *client,const char *host,const char *key,const char *text,
 char *answer,size_t cap,const struct vv_https_cancel *cancel,vv_aliyun_chat_delta_fn on_delta,void *delta_arg)
{
 if(answer&&cap)vv_https_secure_zero(answer,cap);
 if(!client||!key||!answer||!cap||cap>VV_CHAT_ANSWER_MAX+1u||!vv_aliyun_speech_host_valid(host))return VV_CHAT_INVALID;
 char request_host[129];memcpy(request_host,host,strlen(host)+1);
 if(cancelled(cancel))return VV_HTTPS_ECANCELLED;
 size_t key_bytes=0;while(key_bytes<=2048&&key[key_bytes]){unsigned char c=(unsigned char)key[key_bytes++];if(c<33||c>126)return VV_CHAT_INVALID;}
 if(!key_bytes||key_bytes>2048)return VV_CHAT_INVALID;
 char *json=calloc(VV_CHAT_REQUEST_MAX,1);
 struct vv_aliyun_chat_stream *stream=calloc(1,sizeof(*stream));
 char bearer[2056]={0};int rc=VV_CHAT_MEMORY;bool search=false;
 if(!json||!stream)goto done;
 size_t length=0;rc=vv_aliyun_chat_request(text,json,VV_CHAT_REQUEST_MAX,&length);if(rc)goto done;
 search=!minute_story(text)&&vv_chat_wants_search(text);
 if(search){printf("CHAT_SEARCH requested=1 strategy=turbo\n");fflush(stdout);search_text_log("CHAT_SEARCH_INPUT",text);}
 vv_aliyun_chat_stream_init_delta(stream,cancel,on_delta,delta_arg);
 snprintf(bearer,sizeof(bearer),"Bearer %s",key);
 struct vv_https_header headers[]={{"Authorization",bearer,true},{"Content-Type","application/json",false},{"Accept","text/event-stream",false}};
 struct body body={json,length};
 struct vv_https_request request={.method="POST",.host=request_host,.port=443,
  .path="/compatible-mode/v1/chat/completions",.headers=headers,.header_count=3,
  .body_length=length,.body_read=body_read,.body_arg=&body};
 struct vv_https_client https=*client;
 https.limits.max_response_body_bytes=VV_ALIYUN_CHAT_WIRE_MAX;
 if(https.limits.max_chunk_bytes>VV_ALIYUN_CHAT_WIRE_MAX)https.limits.max_chunk_bytes=VV_ALIYUN_CHAT_WIRE_MAX;
 if(https.limits.total_timeout_ms>30000)https.limits.total_timeout_ms=30000;
 struct vv_https_response response={0};struct sink sink={stream,&response,0};
 rc=vv_https_perform(&https,&request,receive,&sink,cancel,&response);
 if(sink.error)rc=sink.error;
 if(!rc&&(response.status_code!=200||!mime(response.content_type)))rc=VV_CHAT_DEPENDENCY_UNAVAILABLE;
 if(!rc)rc=vv_aliyun_chat_stream_finish(stream,answer,cap);
 if(cancelled(cancel))rc=VV_HTTPS_ECANCELLED;
 if(search){
  printf("CHAT_SEARCH_RESULT rc=%d http=%u usage_seen=%u\n",rc,(unsigned)response.status_code,(unsigned)stream->usage_seen);fflush(stdout);
  if(!rc)search_text_log("CHAT_SEARCH_ANSWER",answer);
 }
done:
 vv_https_secure_zero(bearer,sizeof(bearer));
 if(stream){vv_https_secure_zero(stream,sizeof(*stream));free(stream);}
 if(json){vv_https_secure_zero(json,VV_CHAT_REQUEST_MAX);free(json);}
 if(rc)vv_https_secure_zero(answer,cap);
 return rc;
}
