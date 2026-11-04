#include "vv_asr_stream.h"
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vv_asr_stream_init(struct vv_asr_stream *s) { if(s)memset(s,0,sizeof(*s)); }
void vv_asr_stream_abort(struct vv_asr_stream *s) { if(s)s->state=VV_ASR_FAILED; }
static int fail(struct vv_asr_stream *s) { s->state=VV_ASR_FAILED;return -EPROTO; }
static int control(char *out,size_t cap,size_t *bytes,const char *value) {
  size_t n=strlen(value);if(!out||!bytes)return -EINVAL;
  if(cap<=n)return -ENOSPC;
  memcpy(out,value,n+1);*bytes=n;return 0;
}
int vv_asr_stream_start(struct vv_asr_stream *s,char *out,size_t cap,size_t *n) {
  if(!s||s->state!=VV_ASR_NEW)return -EINVAL;
  int rc=control(out,cap,n,"{\"type\":\"start\",\"sample_rate\":16000,\"channels\":1,\"encoding\":\"pcm_s16le\",\"frame_ms\":20}");
  if(!rc)s->state=VV_ASR_WAIT_READY;
  return rc;
}
int vv_asr_stream_audio(struct vv_asr_stream *s,const void *pcm,size_t n,
                        unsigned char *out,size_t cap) {
  if(!s||!pcm||!out||n!=VV_ASR_PCM_BYTES||s->state!=VV_ASR_STREAMING)return -EINVAL;
  if(cap<VV_ASR_PACKET_BYTES)return -ENOSPC;
  if(s->sent>=VV_ASR_MAX_FRAMES)return -EFBIG;
  if(s->sent-s->acknowledged>=VV_ASR_WINDOW)return -EAGAIN;
  /* memmove permits PCM already held at out+4, or overlapping caller storage. */
  memmove(out+4,pcm,n);
  for(unsigned i=0;i<4;i++)out[i]=(unsigned char)(s->sent>>(8*i));
  s->sent++;return 0;
}
int vv_asr_stream_stop(struct vv_asr_stream *s,char *out,size_t cap,size_t *n) {
  if(!s||s->state!=VV_ASR_STREAMING)return -EINVAL;
  if(s->sent!=s->acknowledged)return -EAGAIN;
  char b[64];snprintf(b,sizeof(b),"{\"type\":\"stop\",\"next_seq\":%u}",(unsigned)s->sent);
  int rc=control(out,cap,n,b);if(!rc)s->state=VV_ASR_WAIT_COMPLETED;return rc;
}
int vv_asr_stream_cancel(struct vv_asr_stream *s,char *out,size_t cap,size_t *n) {
  if(!s||(s->state!=VV_ASR_STREAMING&&s->state!=VV_ASR_WAIT_COMPLETED))return -EINVAL;
  int rc=control(out,cap,n,"{\"type\":\"cancel\"}");if(!rc)s->state=VV_ASR_CANCELLING;return rc;
}
/* Contract uses a flat JSON object with strings and unsigned integer tokens.
 * Reject nested allocation, embedded NUL (including escaped), float/exponent,
 * and control characters before cJSON allocation. Parse only bounded messages. */
static int lexical(const char *p,size_t n) {
  int quoted=0,escape=0;unsigned objects=0;
  for(size_t i=0;i<n;i++) {
    unsigned char c=(unsigned char)p[i];if(!c)return 0;
    if(quoted) {
      if(c<32)return 0;
      if(escape){escape=0;continue;}
      if(c=='\\') {
        if(i+5<n&&!memcmp(p+i,"\\u0000",6))return 0;
        escape=1;
      } else if(c=='"')quoted=0;
    } else if(c=='"')quoted=1;
    else if(c=='{'){if(++objects>1)return 0;}
    else if(c=='['||c==']'||c=='.'||c=='e'||c=='E'||c=='-'||c=='+')return 0;
  }
  return !quoted&&!escape&&objects==1;
}
static cJSON *field(cJSON *o,const char *name) {return cJSON_GetObjectItemCaseSensitive(o,name);}
static int number(cJSON *o,const char *name,uint32_t *v) {
  cJSON *x=field(o,name);
  if(!cJSON_IsNumber(x)||x->valuedouble<0||x->valuedouble>UINT32_MAX)return 0;
  *v=(uint32_t)x->valuedouble;return (double)*v==x->valuedouble;
}
static int utf8(const unsigned char *s) {
  while(*s) {
    uint32_t v;unsigned n;unsigned char c=*s++;
    if(c<0x80)continue;
    if(c>=0xc2&&c<=0xdf){v=c&31;n=1;}
    else if(c>=0xe0&&c<=0xef){v=c&15;n=2;}
    else if(c>=0xf0&&c<=0xf4){v=c&7;n=3;}
    else return 0;
    unsigned count=n;
    while(n--){if((*s&0xc0)!=0x80)return 0;v=(v<<6)|(*s++&63);}
    if((count==2&&v<0x800)||(count==3&&v<0x10000)||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
  }
  return 1;
}
int vv_asr_stream_receive(struct vv_asr_stream *s,const char *raw,size_t n,
                          struct vv_asr_event *ev) {
  if(!s||!ev)return -EINVAL;
  memset(ev,0,sizeof(*ev));
  if(s->state==VV_ASR_NEW||s->state>=VV_ASR_DONE)return -EINVAL;
  if(!raw||!n||n>VV_ASR_MAX_JSON||!lexical(raw,n))return fail(s);
  char *copy=malloc(n+1);if(!copy)return -ENOMEM;
  memcpy(copy,raw,n);copy[n]=0;
  cJSON *o=cJSON_ParseWithLengthOpts(copy,n+1,NULL,1);free(copy);
  if(!cJSON_IsObject(o)){cJSON_Delete(o);return fail(s);}
  int ok=1;unsigned count=0;
  for(cJSON *a=o->child;a;a=a->next){count++;for(cJSON *b=a->next;b;b=b->next)if(!strcmp(a->string,b->string))ok=0;}
  cJSON *t=field(o,"type");if(!cJSON_IsString(t))ok=0;
  if(!ok){cJSON_Delete(o);return fail(s);}
  const char *kind=t->valuestring;uint32_t v=0,w=0,z=0;
  if(!strcmp(kind,"error")) {
    cJSON *code=field(o,"code");ok=count==2&&cJSON_IsString(code)&&strlen(code->valuestring)>0&&strlen(code->valuestring)<sizeof(ev->error_code);
    if(ok){for(const char *p=code->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'))ok=0;}
    if(ok){strcpy(ev->error_code,code->valuestring);ev->kind=VV_ASR_REMOTE_ERROR;s->state=VV_ASR_FAILED;}
  } else if(s->state==VV_ASR_WAIT_READY) {
    ok=!strcmp(kind,"ready")&&count==4&&number(o,"frame_bytes",&v)&&v==640&&number(o,"frame_ms",&w)&&w==20&&number(o,"max_frames",&z)&&z==3000;
    if(ok){s->state=VV_ASR_STREAMING;ev->kind=VV_ASR_READY;}
  } else if(!strcmp(kind,"ack")) {
    ok=count==2&&number(o,"next_seq",&v)&&v==s->acknowledged+1&&v<=s->sent;
    if(ok){s->acknowledged=v;ev->kind=VV_ASR_ACK;}
  } else if(!strcmp(kind,"completed")) {
    ok=count==1&&s->state==VV_ASR_WAIT_COMPLETED;
    if(ok){s->state=VV_ASR_DONE;ev->kind=VV_ASR_COMPLETED;}
  } else if(!strcmp(kind,"cancelled")) {
    ok=count==1&&s->state==VV_ASR_CANCELLING;
    if(ok){s->state=VV_ASR_CANCELLED;ev->kind=VV_ASR_CANCEL_ACK;}
  } else if(!strcmp(kind,"partial")||!strcmp(kind,"final")) {
    cJSON *text=field(o,"text");
    ok=count==4&&number(o,"sentence_id",&v)&&number(o,"revision",&w)&&s->revision<UINT32_MAX&&w==s->revision+1&&cJSON_IsString(text)&&strlen(text->valuestring)<=VV_ASR_MAX_TEXT&&utf8((const unsigned char *)text->valuestring);
    if(ok){s->revision=w;if(s->state!=VV_ASR_CANCELLING){ev->kind=!strcmp(kind,"final")?VV_ASR_FINAL:VV_ASR_PARTIAL;ev->sentence_id=v;ev->revision=w;strcpy(ev->text,text->valuestring);}}
  } else ok=0;
  cJSON_Delete(o);return ok?0:fail(s);
}
