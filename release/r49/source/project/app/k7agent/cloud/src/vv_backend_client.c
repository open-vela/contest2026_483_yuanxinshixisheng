#include "vv_real_shuiguang.h"
#include "vv_backend_client.h"
#include "../../../k7radio/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct buffer { unsigned char *bytes; size_t used,capacity; };
struct body { const char *text; size_t length; };
static ssize_t body_read(void *arg,size_t offset,void *dst,size_t capacity) {
  struct body *b=arg;
  if(offset>b->length) return -1;
  size_t n=b->length-offset; if(n>capacity) n=capacity;
  memcpy(dst,b->text+offset,n); return (ssize_t)n;
}
static int receive(void *arg,const void *src,size_t size) {
  struct buffer *b=arg;
  if(size>b->capacity-b->used) return -1;
  memcpy(b->bytes+b->used,src,size); b->used+=size; return 0;
}
void vv_backend_client_clear_session(struct vv_backend_client *c) {
  if(!c) return;
  vv_https_secure_zero(c->token,sizeof(c->token)); c->expires_at=0;
}
static int clock_now(struct vv_backend_client *c,uint64_t *now) {
  if(!c->utc_now || !c->utc_now(c->clock_arg,now) || *now<c->last_utc) {
    vv_backend_client_clear_session(c); return VV_BACKEND_CLIENT_TIME;
  }
  c->last_utc=*now; return 0;
}
static bool config_ok(struct vv_backend_client *c) {
  return c && c->https && c->host && *c->host && c->base_path &&
    (!*c->base_path || (c->base_path[0]=='/' &&
      c->base_path[strlen(c->base_path)-1]!='/')) &&
    !strpbrk(c->base_path,"?#\r\n") && strlen(c->base_path)<256;
}
static int authenticated(struct vv_backend_client *c) {
  uint64_t now; int rc=clock_now(c,&now); if(rc) return rc;
  if(!c->token[0] || now>=c->expires_at || c->expires_at-now<=60) {
    vv_backend_client_clear_session(c); return VV_BACKEND_CLIENT_AUTH;
  }
  return 0;
}
static uint16_t port(uint16_t p) { return p?p:443; }
static int exchange_minimum(struct vv_backend_client *c,const struct vv_https_request *request,
  struct buffer *buffer,struct vv_https_response *response,const struct vv_https_cancel *cancel,unsigned minimum_timeout_ms) {
  memset(response,0,sizeof(*response)); c->last_http_status=0;
  struct vv_https_client https=*c->https;
  if(https.limits.total_timeout_ms<minimum_timeout_ms)https.limits.total_timeout_ms=minimum_timeout_ms;
  if(https.limits.max_response_body_bytes>buffer->capacity)
    https.limits.max_response_body_bytes=buffer->capacity;
  int rc=c->development_lan_http ?
    vv_http_development_perform(&https,request,receive,buffer,cancel,response) :
    vv_https_perform(&https,request,receive,buffer,cancel,response);
  c->last_http_status=response->status_code;
  /* Parsed status is diagnostic even when the response is incomplete. */
  printf("BACKEND_HTTP rc=%d http=%d\n",rc,response->status_code);
  if(response->status_code==401) {
    vv_backend_client_clear_session(c);
    if(!rc)return VV_BACKEND_CLIENT_AUTH;
  }
  return rc;
}
static int exchange(struct vv_backend_client *c,const struct vv_https_request *request,
 struct buffer *buffer,struct vv_https_response *response,const struct vv_https_cancel *cancel){
 return exchange_minimum(c,request,buffer,response,cancel,0);
}
static int perform_minimum(struct vv_backend_client *c,const struct vv_backend_endpoint *ep,
  const char *method,const char *json,size_t length,bool auth,struct buffer *buffer,
  struct vv_https_response *response,const struct vv_https_cancel *cancel,unsigned minimum_timeout_ms) {
  char bearer[VV_BACKEND_TOKEN_CAP+8];
  struct vv_https_header headers[2]; size_t count=0;
  memset(response,0,sizeof(*response)); c->last_http_status=0;
  if(json) headers[count++]=(struct vv_https_header){"Content-Type","application/json",false};
  if(auth) {
    snprintf(bearer,sizeof(bearer),"Bearer %s",c->token);
    headers[count++]=(struct vv_https_header){"Authorization",bearer,true};
  }
  struct body body={json,length};
  struct vv_https_request request={.method=method,.host=ep->host,.port=ep->port,
    .path=ep->path,.headers=headers,.header_count=count,.body_length=length,
    .body_read=json?body_read:NULL,.body_arg=&body};
  /* Per-call body limit does not mutate the caller's shared transport settings. */
  int rc=exchange_minimum(c,&request,buffer,response,cancel,minimum_timeout_ms);
  vv_https_secure_zero(bearer,sizeof(bearer));
  return rc;
}
static int perform(struct vv_backend_client *c,const struct vv_backend_endpoint *ep,
 const char *method,const char *json,size_t length,bool auth,struct buffer *buffer,
 struct vv_https_response *response,const struct vv_https_cancel *cancel){
 return perform_minimum(c,ep,method,json,length,auth,buffer,response,cancel,0);
}
static int codec_error(int rc) {
  return rc==VV_BACKEND_MOCK?VV_BACKEND_CLIENT_MOCK:VV_BACKEND_CLIENT_PROTOCOL;
}
struct chat_sink {
  struct vv_chat_stream *stream;
  struct vv_https_response *response;
  int error;
};
static bool chat_mime(const char *s){
  const char *want="text/event-stream";
  while(*want){unsigned char c=(unsigned char)*s++;if(c>='A'&&c<='Z')c+=32;if(c!=(unsigned char)*want++)return false;}
  return !*s||*s==';';
}
static int chat_receive(void *arg,const void *data,size_t bytes){
  struct chat_sink *s=arg;
  /* Header parser completes before the first body callback. Nothing reaches
   * caller output until both the response framing and SSE terminal validate. */
  if(s->response->status_code!=200||!chat_mime(s->response->content_type))
    return s->error=VV_BACKEND_CLIENT_HTTP;
  return s->error=vv_chat_stream_feed(s->stream,data,bytes);
}
int vv_backend_client_chat(struct vv_backend_client *c,const char *text,
  char *answer,size_t cap,const struct vv_https_cancel *cancel){
  if(answer&&cap)vv_https_secure_zero(answer,cap);
  if(!config_ok(c)||!answer||!cap||cap>VV_CHAT_ANSWER_MAX+1u)return VV_BACKEND_CLIENT_INVALID;
  c->last_http_status=0;
  if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return VV_HTTPS_ECANCELLED;
  int rc=authenticated(c);if(rc)return rc;
  char *json=calloc(1,VV_CHAT_REQUEST_MAX);
  struct vv_chat_stream *parser=calloc(1,sizeof(*parser));
  char bearer[VV_BACKEND_TOKEN_CAP+8]={0};
  if(!json||!parser){rc=VV_BACKEND_CLIENT_MEMORY;goto done;}
  size_t length=0;rc=vv_chat_question_json(text,json,VV_CHAT_REQUEST_MAX,&length);if(rc)goto done;
  rc=vv_chat_stream_init(parser,cancel?cancel->is_cancelled:NULL,cancel?cancel->arg:NULL);if(rc)goto done;
  char path[320];int written=snprintf(path,sizeof(path),"%s/api/v1/gimbal-ai/messages",c->base_path);
  if(written<0||(size_t)written>=sizeof(path)){rc=VV_BACKEND_CLIENT_INVALID;goto done;}
  snprintf(bearer,sizeof(bearer),"Bearer %s",c->token);
  struct vv_https_header headers[]={{"Content-Type","application/json",false},
    {"Authorization",bearer,true},{"Accept","text/event-stream",false}};
  struct body body={json,length};
  struct vv_https_request request={.method="POST",.host=c->host,.port=c->port,
    .path=path,.headers=headers,.header_count=3,.body_length=length,.body_read=body_read,.body_arg=&body};
  struct vv_https_response response={0};struct chat_sink sink={parser,&response,0};
  struct vv_https_client https=*c->https;
  https.limits.max_response_body_bytes=VV_CHAT_WIRE_MAX;
  if(https.limits.max_chunk_bytes>VV_CHAT_WIRE_MAX)https.limits.max_chunk_bytes=VV_CHAT_WIRE_MAX;
  rc=c->development_lan_http?
    vv_http_development_perform(&https,&request,chat_receive,&sink,cancel,&response):
    vv_https_perform(&https,&request,chat_receive,&sink,cancel,&response);
  c->last_http_status=response.status_code;
  if(response.status_code==401){vv_backend_client_clear_session(c);rc=VV_BACKEND_CLIENT_AUTH;}
  else if(sink.error)rc=sink.error;
  if(!rc&&(response.status_code!=200||!chat_mime(response.content_type)))rc=VV_BACKEND_CLIENT_HTTP;
  if(!rc)rc=authenticated(c);
  if(!rc)rc=vv_chat_stream_finish(parser,answer,cap);
  if(rc==VV_CHAT_CANCELLED)rc=VV_HTTPS_ECANCELLED;
done:
  vv_https_secure_zero(bearer,sizeof(bearer));
  if(parser){vv_https_secure_zero(parser,sizeof(*parser));free(parser);}
  if(json){vv_https_secure_zero(json,VV_CHAT_REQUEST_MAX);free(json);}
  if(rc)vv_https_secure_zero(answer,cap);
  return rc;
}
static uint64_t epoch(const char *s) {
  unsigned y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
  unsigned m=(s[5]-'0')*10+s[6]-'0', d=(s[8]-'0')*10+s[9]-'0';
  if(y<1970) return 0;
  uint64_t days=0;
  for(unsigned i=1970;i<y;++i) days+=365+(i%4==0 && (i%100!=0 || i%400==0));
  static const unsigned month[]={31,28,31,30,31,30,31,31,30,31,30,31};
  for(unsigned i=1;i<m;++i) days+=month[i-1]+(i==2 && y%4==0 && (y%100!=0 || y%400==0));
  return (days+d-1)*86400+((s[11]-'0')*10+s[12]-'0')*3600+
    ((s[14]-'0')*10+s[15]-'0')*60+(s[17]-'0')*10+s[18]-'0';
}
int vv_backend_client_login(struct vv_backend_client *c,const char *credential,
  const char *version,const char *proof,const struct vv_https_cancel *cancel) {
  if(!config_ok(c)) return VV_BACKEND_CLIENT_INVALID;
  vv_backend_client_clear_session(c); c->last_http_status=0;
  uint64_t now; int rc=clock_now(c,&now); if(rc) return rc;
  char *json=malloc(VV_BACKEND_JSON_MAX), *reply=malloc(VV_BACKEND_JSON_MAX);
  struct vv_backend_session *session=calloc(1,sizeof(*session));
  if(!json || !reply || !session) { rc=VV_BACKEND_CLIENT_MEMORY; goto done; }
  size_t length=0;
  rc=vv_backend_login_json(credential,version,proof,json,VV_BACKEND_JSON_MAX,&length);
  if(rc) { rc=codec_error(rc); goto done; }
  char path[320]; snprintf(path,sizeof(path),"%s/api/v1/gimbal-sessions",c->base_path);
  struct vv_backend_endpoint ep={c->host,c->port,path};
  struct buffer buffer={(unsigned char *)reply,0,VV_BACKEND_JSON_MAX};
  struct vv_https_response response;
  rc=perform(c,&ep,"POST",json,length,false,&buffer,&response,cancel);
  if(rc) goto done;
  if(response.status_code!=200) { rc=VV_BACKEND_CLIENT_HTTP; goto done; }
  rc=vv_backend_parse_session(reply,buffer.used,session);
  if(rc) { rc=codec_error(rc); goto done; }
  rc=clock_now(c,&now); if(rc) goto done;
  uint64_t expires=epoch(session->expires_at);
  if(expires<=now || expires-now<=60) { rc=VV_BACKEND_CLIENT_AUTH; goto done; }
  strcpy(c->token,session->token); c->expires_at=expires;
done:
  if(json) { vv_https_secure_zero(json,VV_BACKEND_JSON_MAX); free(json); }
  if(reply) { vv_https_secure_zero(reply,VV_BACKEND_JSON_MAX); free(reply); }
  if(session) { vv_https_secure_zero(session,sizeof(*session)); free(session); }
  return rc;
}
static bool uuid(const char *s) {
  if(!s || strlen(s)!=36) return false;
  for(unsigned i=0;i<36;++i) {
    if(i==8 || i==13 || i==18 || i==23) { if(s[i]!='-') return false; }
    else if(!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f') || (s[i]>='A' && s[i]<='F'))) return false;
  }
  return true;
}
static bool unique_keys(cJSON *obj) {
  if(!cJSON_IsObject(obj)) return false;
  for(cJSON *a=obj->child;a;a=a->next)
    for(cJSON *b=a->next;b;b=b->next)
      if(a->string && b->string && !strcmp(a->string,b->string)) return false;
  return true;
}
/* Bound recursion before cJSON allocates, even when linked against its default
 * depth-1000 build. Braces and escaped quotes inside strings are not nesting. */
static bool json_preflight(const char *json,size_t size) {
  if(!json || !size || size>VV_BACKEND_JSON_MAX)return false;
  unsigned depth=0;bool quoted=false;
  for(size_t i=0;i<size;++i) {
    unsigned char ch=(unsigned char)json[i];
    if(!ch)return false;
    if(quoted && ch=='\\') {
      if(++i>=size || !json[i])return false;
      if(json[i]=='u' && i+4<size && !memcmp(json+i+1,"0000",4))return false;
      continue;
    }
    if(ch=='"')quoted=!quoted;
    else if(!quoted && (ch=='{' || ch=='[')) {if(++depth>4)return false;}
    else if(!quoted && (ch=='}' || ch==']')) {if(!depth)return false;--depth;}
  }
  return !quoted && depth==0;
}
static bool replaced(const char *json,size_t size) {
  /* Error envelope only; reject embedded/escaped NUL and duplicate root/error
   * fields. No substring matching on server-provided error text. */
  if(!json_preflight(json,size))return false;
  const char *end=NULL; cJSON *root=cJSON_ParseWithLengthOpts(json,size,&end,0);
  bool ok=false;
  if(!unique_keys(root)) goto done;
  while(end<json+size && (*end==' ' || *end=='\t' || *end=='\r' || *end=='\n')) ++end;
  if(end!=json+size) goto done;
  cJSON *error=cJSON_GetObjectItemCaseSensitive(root,"error");
  if(!unique_keys(error)) goto done;
  cJSON *code=cJSON_GetObjectItemCaseSensitive(error,"code");
  ok=cJSON_IsString(code) && !strcmp(code->valuestring,"TASK_REPLACED");
done:
  cJSON_Delete(root); return ok;
}
int vv_backend_client_task(struct vv_backend_client *c,const char *id,
  struct vv_backend_task *out,const struct vv_https_cancel *cancel) {
  if(out) memset(out,0,sizeof(*out));
  if(!config_ok(c) || !out || !uuid(id)) return VV_BACKEND_CLIENT_INVALID;
  c->last_http_status=0; int rc=authenticated(c); if(rc) return rc;
  unsigned char *reply=malloc(VV_BACKEND_JSON_MAX);
  if(!reply) return VV_BACKEND_CLIENT_MEMORY;
  char path[352]; snprintf(path,sizeof(path),"%s/api/v1/skin-assessment-tasks/%s",c->base_path,id);
  struct vv_backend_endpoint ep={c->host,c->port,path};
  struct buffer buffer={reply,0,VV_BACKEND_JSON_MAX}; struct vv_https_response response;
  rc=perform(c,&ep,"GET",NULL,0,true,&buffer,&response,cancel);
  if(!rc) {
    if(response.status_code==409 && replaced((const char *)reply,buffer.used)) rc=VV_BACKEND_CLIENT_REPLACED;
    else if(response.status_code!=200) rc=VV_BACKEND_CLIENT_HTTP;
    else { rc=vv_backend_parse_task((const char *)reply,buffer.used,id,out); if(rc) rc=codec_error(rc); }
  }
  vv_https_secure_zero(reply,VV_BACKEND_JSON_MAX); free(reply); return rc;
}
static bool idempotency_valid(const char *s) {
  if(!s || !*s) return false;
  size_t n=0;
  for(;*s;++s,++n)
    if(n>=128 || !((*s>='a' && *s<='z') || (*s>='A' && *s<='Z') ||
        (*s>='0' && *s<='9') || strchr("_.:-",*s))) return false;
  return true;
}
static int acceptance(const char *json,size_t size,char out[37]) {
  /* Use protocol parser for final bounded/full-consumption/mock/duplicate
   * validation. This first parse only obtains the expected task ID/version. */
  if(!json_preflight(json,size))return VV_BACKEND_CLIENT_PROTOCOL;
  cJSON *root=cJSON_ParseWithLengthOpts(json,size,NULL,0);
  cJSON *data=cJSON_GetObjectItemCaseSensitive(root,"data");
  cJSON *id=cJSON_GetObjectItemCaseSensitive(data,"taskId");
  cJSON *version=cJSON_GetObjectItemCaseSensitive(data,"photoVersion");
  int rc=VV_BACKEND_CLIENT_PROTOCOL;
  if(cJSON_IsString(id) && uuid(id->valuestring) && cJSON_IsString(version) &&
     !strcmp(version->valuestring,"1")) {
    struct vv_backend_task parsed;
    rc=vv_backend_parse_task(json,size,id->valuestring,&parsed);
    if(rc) rc=codec_error(rc);
    else if(parsed.status!=VV_TASK_QUEUED) rc=VV_BACKEND_CLIENT_PROTOCOL;
    else strcpy(out,parsed.task_id);
  }
  cJSON_Delete(root);return rc;
}
int vv_backend_client_upload(struct vv_backend_client *c,const char *session,
  const char *consent,const char *boundary,const char *key,
  const struct k7_upload_image images[3],char out[37],const struct vv_https_cancel *cancel) {
  if(out) memset(out,0,37);
  if(!config_ok(c) || !out || !idempotency_valid(key)) return VV_BACKEND_CLIENT_INVALID;
  c->last_http_status=0; int rc=authenticated(c);if(rc)return rc;
  struct k7_assessment_body *body=malloc(sizeof(*body));
  unsigned char *reply=malloc(VV_BACKEND_JSON_MAX);
  if(!body || !reply) { rc=VV_BACKEND_CLIENT_MEMORY;goto done; }
  if(k7_assessment_body_init(body,session,consent,boundary,images)) {
    rc=VV_BACKEND_CLIENT_INVALID;goto done;
  }
  char path[320],bearer[VV_BACKEND_TOKEN_CAP+8];
  snprintf(path,sizeof(path),"%s/api/v1/skin-assessment-tasks",c->base_path);
  snprintf(bearer,sizeof(bearer),"Bearer %s",c->token);
  struct vv_https_header headers[]={
    {"Content-Type",body->content_type,false},
    {"Authorization",bearer,true},{"Idempotency-Key",key,true}};
  struct vv_https_request request={.method="POST",.host=c->host,.port=c->port,
    .path=path,.headers=headers,.header_count=3,.body_length=body->total,
    .body_read=k7_assessment_body_read,.body_arg=body};
  struct buffer buffer={reply,0,VV_BACKEND_JSON_MAX};struct vv_https_response response;
  rc=exchange(c,&request,&buffer,&response,cancel);
  vv_https_secure_zero(bearer,sizeof(bearer));
  if(!rc) {
    if(response.status_code!=200 && response.status_code!=202) rc=VV_BACKEND_CLIENT_HTTP;
    else rc=acceptance((const char *)reply,buffer.used,out);
  }
done:
  if(body) {vv_https_secure_zero(body,sizeof(*body));free(body);}
  if(reply) {vv_https_secure_zero(reply,VV_BACKEND_JSON_MAX);free(reply);}
  return rc;
}
static int audit_json(cJSON *v,unsigned depth) {
  if(depth>4) return VV_BACKEND_CLIENT_PROTOCOL;
  if(cJSON_IsObject(v) && !unique_keys(v)) return VV_BACKEND_CLIENT_PROTOCOL;
  for(cJSON *a=v->child;a;a=a->next) {
    if(a->string && !strcmp(a->string,"mock") && cJSON_IsTrue(a)) return VV_BACKEND_CLIENT_MOCK;
    int rc=audit_json(a,depth+1);if(rc)return rc;
  }
  return 0;
}
static int brief_text(cJSON *data,const char *name,char *out,size_t capacity) {
  cJSON *v=cJSON_GetObjectItemCaseSensitive(data,name);
  if(!v || cJSON_IsNull(v)) return 0;
  if(!cJSON_IsString(v) || strlen(v->valuestring)>=capacity) return VV_BACKEND_CLIENT_PROTOCOL;
  strcpy(out,v->valuestring);return 0;
}
static int parse_brief_mode(const char *json,size_t size,const char *id,struct vv_backend_brief *out,bool strict) {
  if(!json_preflight(json,size))return VV_BACKEND_CLIENT_PROTOCOL;
  const char *end=NULL; cJSON *root=cJSON_ParseWithLengthOpts(json,size,&end,0);
  int rc=VV_BACKEND_CLIENT_PROTOCOL;
  if(!cJSON_IsObject(root))goto done;
  while(end<json+size && (*end==' ' || *end=='\t' || *end=='\r' || *end=='\n'))++end;
  if(end!=json+size)goto done;
  rc=audit_json(root,0);if(rc)goto done;
  cJSON *data=cJSON_GetObjectItemCaseSensitive(root,"data");
  cJSON *report=cJSON_GetObjectItemCaseSensitive(data,"reportId");
  cJSON *view=cJSON_GetObjectItemCaseSensitive(data,"view");
  if(!cJSON_IsObject(data) || !cJSON_IsString(report) || strcmp(report->valuestring,id) ||
#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE && !K7_REAL_SHUIGUANG_PROFILE
     (strict ? (!cJSON_IsString(view) || strcmp(view->valuestring,"brief")) : (view && (!cJSON_IsString(view) || strcmp(view->valuestring,"brief"))))) {rc=VV_BACKEND_CLIENT_PROTOCOL;goto done;}
#else
     !cJSON_IsString(view) || strcmp(view->valuestring,"brief")) {rc=VV_BACKEND_CLIENT_PROTOCOL;goto done;}
#endif
  rc=brief_text(data,"conclusion",out->conclusion,sizeof(out->conclusion));if(rc)goto done;
  rc=brief_text(data,"description",out->description,sizeof(out->description));if(rc)goto done;
  if(K7_REAL_SHUIGUANG_PROFILE||strict){
    if(!vv_real_report_text(out->conclusion,sizeof(out->conclusion),false)||!vv_real_report_text(out->description,sizeof(out->description),false)){rc=VV_BACKEND_CLIENT_PROTOCOL;goto done;}
    if(!out->conclusion[0]){rc=VV_BACKEND_CLIENT_REPORT_MISSING;goto done;}
  }
  strcpy(out->report_id,id);
done:
  cJSON_Delete(root);if(rc)memset(out,0,sizeof(*out));return rc;
}
static int parse_brief(const char *json,size_t size,const char *id,struct vv_backend_brief *out){
 return parse_brief_mode(json,size,id,out,false);
}
int vv_backend_client_task_wait(struct vv_backend_client *c,const char *id,
 unsigned wait_seconds,struct vv_backend_task *task,struct vv_backend_brief *brief,
 const struct vv_https_cancel *cancel){
 if(task)memset(task,0,sizeof(*task));
 if(brief)memset(brief,0,sizeof(*brief));
 if(!config_ok(c)||!task||!brief||!uuid(id)||wait_seconds>25)return VV_BACKEND_CLIENT_INVALID;
 c->last_http_status=0;int rc=authenticated(c);if(rc)return rc;
 char path[416];
 int length=snprintf(path,sizeof(path),"%s/api/v1/skin-assessment-tasks/%s?waitSeconds=%u&includeBrief=true",c->base_path,id,wait_seconds);
 if(length<0||(size_t)length>=sizeof(path))return VV_BACKEND_CLIENT_INVALID;
 unsigned char *reply=malloc(VV_BACKEND_JSON_MAX);if(!reply)return VV_BACKEND_CLIENT_MEMORY;
 struct vv_backend_endpoint ep={c->host,c->port,path};
 struct buffer buffer={reply,0,VV_BACKEND_JSON_MAX};struct vv_https_response response;
 /* Local HTTP copy only: allow server wait plus TLS/headers, while the caller's
  * unchanged cancellation token still enforces the whole assessment deadline. */
 rc=perform_minimum(c,&ep,"GET",NULL,0,true,&buffer,&response,cancel,(wait_seconds+15u)*1000u);
 if(!rc){
  if(response.status_code==409&&replaced((const char *)reply,buffer.used))rc=VV_BACKEND_CLIENT_REPLACED;
  else if(response.status_code==502||response.status_code==503||response.status_code==504)rc=VV_HTTPS_EIO;
  else if(response.status_code!=200)rc=VV_BACKEND_CLIENT_HTTP;
  else{
   rc=vv_backend_parse_task((const char *)reply,buffer.used,id,task);
   if(rc)rc=codec_error(rc);
   else if(task->status==VV_TASK_REPORT_READY)
    rc=parse_brief_mode((const char *)reply,buffer.used,task->report_id,brief,true);
  }
 }
 vv_https_secure_zero(reply,VV_BACKEND_JSON_MAX);free(reply);
 if(rc){memset(task,0,sizeof(*task));memset(brief,0,sizeof(*brief));}
 return rc;
}

int vv_backend_client_brief(struct vv_backend_client *c,const char *id,
  struct vv_backend_brief *out,const struct vv_https_cancel *cancel) {
  if(out)memset(out,0,sizeof(*out));
  if(!config_ok(c) || !out || !uuid(id))return VV_BACKEND_CLIENT_INVALID;
  c->last_http_status=0;int rc=authenticated(c);if(rc)return rc;
  unsigned char *reply=malloc(VV_BACKEND_JSON_MAX);if(!reply)return VV_BACKEND_CLIENT_MEMORY;
  char path[352];snprintf(path,sizeof(path),"%s/api/v1/skin-reports/%s?view=brief",c->base_path,id);
  struct vv_backend_endpoint ep={c->host,c->port,path};
  struct buffer buffer={reply,0,VV_BACKEND_JSON_MAX};struct vv_https_response response;
  rc=perform(c,&ep,"GET",NULL,0,true,&buffer,&response,cancel);
  if(!rc) {
    if(response.status_code==409 && replaced((const char *)reply,buffer.used))rc=VV_BACKEND_CLIENT_REPLACED;
    else if(response.status_code!=200)rc=VV_BACKEND_CLIENT_HTTP;
    else rc=parse_brief((const char *)reply,buffer.used,id,out);
  }
  vv_https_secure_zero(reply,VV_BACKEND_JSON_MAX);free(reply);return rc;
}
int vv_backend_client_tts(struct vv_backend_client *c,const struct vv_backend_endpoint *ep,
  bool auth,const char *text,unsigned char *pcm,size_t cap,size_t *size,
  const struct vv_https_cancel *cancel) {
  if(size) *size=0;
  if(!config_ok(c) || !ep || !ep->host || !*ep->host || !ep->path ||
     !pcm || !size || !cap || cap>960000) return VV_BACKEND_CLIENT_INVALID;
  vv_https_secure_zero(pcm,cap); c->last_http_status=0;
  if(auth && (strcmp(ep->host,c->host) || port(ep->port)!=port(c->port))) return VV_BACKEND_CLIENT_INVALID;
  uint64_t now; int rc=auth?authenticated(c):clock_now(c,&now); if(rc) return rc;
  char *json=malloc(VV_BACKEND_JSON_MAX); if(!json) return VV_BACKEND_CLIENT_MEMORY;
  size_t length=0; rc=vv_backend_tts_json(text,json,VV_BACKEND_JSON_MAX,&length);
  if(rc) { rc=codec_error(rc); goto done; }
  struct buffer buffer={pcm,0,cap}; struct vv_https_response response;
  rc=perform(c,ep,"POST",json,length,auth,&buffer,&response,cancel);
  if(!rc) {
    if(response.status_code!=200) rc=VV_BACKEND_CLIENT_HTTP;
    else if(!vv_backend_tts_content_type(response.content_type) || !buffer.used || buffer.used%2)
      rc=VV_BACKEND_CLIENT_AUDIO;
    else *size=buffer.used;
  }
done:
  vv_https_secure_zero(json,VV_BACKEND_JSON_MAX); free(json);
  if(rc) vv_https_secure_zero(pcm,cap);
  return rc;
}
