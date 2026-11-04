#include "vv_asr_batch.h"
#include "../../../k7radio/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RESPONSE_MAX 8192
struct body { const void *part[3]; size_t length[3]; };
struct response_buffer { unsigned char *data; size_t used; };
static ssize_t body_read(void *arg,size_t offset,void *dst,size_t cap) {
  struct body *b=arg;size_t copied=0;
  for(unsigned i=0;i<3 && copied<cap;++i) {
    if(offset>=b->length[i]){offset-=b->length[i];continue;}
    size_t n=b->length[i]-offset;if(n>cap-copied)n=cap-copied;
    memcpy((unsigned char *)dst+copied,(const unsigned char *)b->part[i]+offset,n);
    copied+=n;offset=0;
  }
  return (ssize_t)copied;
}
static int receive(void *arg,const void *data,size_t size) {
  struct response_buffer *b=arg;if(size>RESPONSE_MAX-b->used)return -1;
  memcpy(b->data+b->used,data,size);b->used+=size;return 0;
}
static bool preflight(const unsigned char *s,size_t size) {
  unsigned depth=0;bool quoted=false;
  if(!size || size>RESPONSE_MAX)return false;
  for(size_t i=0;i<size;++i) {
    unsigned char c=s[i];if(!c)return false;
    if(quoted && c==92) {
      if(++i>=size || !s[i])return false;
      if(s[i]=='u' && i+4<size && !memcmp(s+i+1,"0000",4))return false;
      continue;
    }
    if(c=='"')quoted=!quoted;
    else if(!quoted && (c=='{' || c=='[')){if(++depth>4)return false;}
    else if(!quoted && (c=='}' || c==']')){if(!depth)return false;--depth;}
  }
  return !quoted && !depth;
}
static int audit(cJSON *v) {
  for(cJSON *a=v->child;a;a=a->next) {
    if(cJSON_IsObject(v)) {
      if(!a->string)return VV_ASR_BATCH_PROTOCOL;
      for(cJSON *b=a->next;b;b=b->next)
        if(b->string && !strcmp(a->string,b->string))return VV_ASR_BATCH_PROTOCOL;
      if(!strcmp(a->string,"mock") && cJSON_IsTrue(a))return VV_ASR_BATCH_MOCK;
    }
    int rc=audit(a);if(rc)return rc;
  }
  return 0;
}
static bool utf8(const unsigned char *s) {
  while(*s) {
    unsigned c=*s++,code,min,n;
    if(c<128){if(c<32 || c==127)return false;continue;}
    if(c>=0xc2 && c<=0xdf){code=c&31;n=1;min=0x80;}
    else if(c>=0xe0 && c<=0xef){code=c&15;n=2;min=0x800;}
    else if(c>=0xf0 && c<=0xf4){code=c&7;n=3;min=0x10000;}
    else return false;
    while(n--){unsigned d=*s++;if(d<0x80 || d>0xbf)return false;code=(code<<6)|(d&63);}
    if(code<min || code>0x10ffff || (code>=0xd800 && code<=0xdfff))return false;
  }
  return true;
}
static int parse(struct response_buffer *b,char *text) {
  if(!preflight(b->data,b->used))return VV_ASR_BATCH_PROTOCOL;
  const char *start=(const char *)b->data,*end=NULL;
  cJSON *root=cJSON_ParseWithLengthOpts(start,b->used,&end,0);
  int rc=VV_ASR_BATCH_PROTOCOL;
  if(!cJSON_IsObject(root))goto done;
  while(end<start+b->used && (*end==' '||*end=='\t'||*end=='\r'||*end=='\n'))++end;
  if(end!=start+b->used)goto done;
  rc=audit(root);if(rc)goto done;
  cJSON *success=cJSON_GetObjectItemCaseSensitive(root,"success");
  cJSON *value=cJSON_GetObjectItemCaseSensitive(root,"text");
  if(!cJSON_IsTrue(success) || !cJSON_IsString(value) ||
     strlen(value->valuestring)>=VV_ASR_BATCH_TEXT_CAP || !utf8((const unsigned char *)value->valuestring))
    {rc=VV_ASR_BATCH_PROTOCOL;goto done;}
  strcpy(text,value->valuestring);
done:
  cJSON_Delete(root);return rc;
}
int vv_asr_batch_perform(struct vv_https_client *client,const struct vv_asr_batch_request *q,
  const struct vv_https_cancel *cancel,struct vv_asr_batch_result *out) {
  if(!out)return VV_ASR_BATCH_INVALID;
  memset(out,0,sizeof(*out));
  if(!client || !q || !q->host || !*q->host || !q->path || q->path[0]!='/' ||
     !q->pcm || !q->pcm_bytes || q->pcm_bytes%2 || q->pcm_bytes>VV_ASR_BATCH_PCM_MAX ||
     q->sample_rate!=16000 || q->channels!=1 || q->bits_per_sample!=16 ||
     !q->boundary_hex || strlen(q->boundary_hex)!=32)return VV_ASR_BATCH_INVALID;
  for(unsigned i=0;i<32;++i)if(!((q->boundary_hex[i]>='0'&&q->boundary_hex[i]<='9')||
      (q->boundary_hex[i]>='a'&&q->boundary_hex[i]<='f')))return VV_ASR_BATCH_INVALID;
  for(size_t i=0;i+32<=q->pcm_bytes;++i)
    if(!memcmp(q->pcm+i,q->boundary_hex,32))return VV_ASR_BATCH_INVALID;
  char header[256],trailer[80],mime[96];
  snprintf(header,sizeof(header),"--VV-%s\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\nContent-Type: audio/pcm\r\n\r\n",q->boundary_hex);
  snprintf(trailer,sizeof(trailer),"\r\n--VV-%s--\r\n",q->boundary_hex);
  snprintf(mime,sizeof(mime),"multipart/form-data; boundary=VV-%s",q->boundary_hex);
  struct body body={{header,q->pcm,trailer},{strlen(header),q->pcm_bytes,strlen(trailer)}};
  struct vv_https_header h={"Content-Type",mime,false};
  struct vv_https_request request={.method="POST",.host=q->host,.port=q->port,.path=q->path,
    .headers=&h,.header_count=1,.body_length=body.length[0]+body.length[1]+body.length[2],
    .body_read=body_read,.body_arg=&body};
  struct response_buffer buffer={calloc(1,RESPONSE_MAX),0};
  if(!buffer.data)return VV_ASR_BATCH_MEMORY;
  struct vv_https_client transport=*client;
  if(transport.limits.max_response_body_bytes>RESPONSE_MAX)transport.limits.max_response_body_bytes=RESPONSE_MAX;
  struct vv_https_response response;
  int rc=vv_https_perform(&transport,&request,receive,&buffer,cancel,&response);
  if(!rc) {
    if(response.status_code!=200)rc=VV_ASR_BATCH_HTTP;
    else rc=parse(&buffer,out->text);
  }
  if(rc)memset(out,0,sizeof(*out));else out->http_status=response.status_code;
  vv_https_secure_zero(buffer.data,RESPONSE_MAX);free(buffer.data);return rc;
}
