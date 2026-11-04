#include "device_voice_intent.h"
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <math.h>
#include <string.h>

/* Only the photo branch uses this exact request grammar. The existing stop,
 * quiet, negation and user-selected fuzzy greeting precedence stays intact. */
static int photo_request(const char *text)
{
  char b[129];size_t n=0;
  for(size_t i=0;text[i];i++) {
    if(text[i]==' '||text[i]=='\t'||text[i]=='\r'||text[i]=='\n')continue;
    if(n==sizeof(b)-1)return 0;
    b[n++]=text[i];
  }
  b[n]=0;char *s=b;
  const char *const ends[]={"。","！","!","."};
  for(unsigned i=0;i<sizeof(ends)/sizeof(ends[0]);i++) {
    size_t z=strlen(ends[i]);if(n>=z&&!strcmp(s+n-z,ends[i])){s[n-=z]=0;break;}
  }
  const char *const requests[]={"你能不能帮我","你可以帮我","你能否帮我","你能帮我","请你帮我","能不能帮我","可以帮我","能否帮我","能帮我","请帮我","帮我","给我"};
  int polite=0;
  for(unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++) {
    size_t z=strlen(requests[i]);if(strncmp(s,requests[i],z))continue;
    s+=z;polite=1;break;
  }
  if(polite) {
    n=strlen(s);
    if(n&&s[n-1]=='?')s[--n]=0;
    else if(n>=strlen("？")&&!strcmp(s+n-strlen("？"),"？"))s[n-=strlen("？")]=0;
    const char *const tails[]={"好不好","可以吗","好吗","吗"};
    for(unsigned i=0;i<sizeof(tails)/sizeof(tails[0]);i++) {
      size_t z=strlen(tails[i]);if(n>=z&&!strcmp(s+n-z,tails[i])){s[n-=z]=0;break;}
    }
  } else {
    const char *const wants[]={"我想要","我想","我要","想","要","请"};
    for(unsigned i=0;i<sizeof(wants)/sizeof(wants[0]);i++) {
      size_t z=strlen(wants[i]);if(!strncmp(s,wants[i],z)){s+=z;break;}
    }
  }
  const char *const commands[]={
    "皮肤检测","皮肤检查","皮肤状态检测","皮肤状态检查","测肤","开始测肤",
    "开始皮肤检测","开始皮肤检查","开始检测皮肤","开始检查皮肤",
    "检测皮肤","检查皮肤","检测一下皮肤","检查一下皮肤",
    "检测我的皮肤","检查我的皮肤","检测一下我的皮肤","检查一下我的皮肤",
    "做皮肤检测","做皮肤检查","做个皮肤检测","做个皮肤检查",
    "做一次皮肤检测","做一次皮肤检查","做一下皮肤检测","做一下皮肤检查",
    "做皮肤状态检测","做皮肤状态检查","做一下皮肤状态检测","做一下皮肤状态检查"};
  for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++)
    if(!strcmp(s,commands[i]))return 1;
  return 0;
}

static enum k7_voice_intent command(const char *text)
{
  char normalized[129];size_t used=0;
  for(size_t i=0;text[i];)
    {
      unsigned char ch=(unsigned char)text[i];
      if(ch==' ' || ch=='\t' || ch=='\r' || ch=='\n' ||
         ch==',' || ch=='.' || ch=='!' || ch=='?'){i++;continue;}
      const char *marks[]={"，","。","！","？","　"};
      int skip=0;
      for(unsigned k=0;k<sizeof(marks)/sizeof(marks[0]);k++)
        if(!strncmp(text+i,marks[k],strlen(marks[k])))
          {i+=strlen(marks[k]);skip=1;break;}
      if(skip)continue;
      if(used==sizeof(normalized)-1)return K7_VOICE_NONE;
      normalized[used++]=(ch>='A' && ch<='Z')?(char)(ch-'A'+'a'):text[i];i++;
    }
  normalized[used]=0;
  if(strstr(normalized,"停止云台") || strstr(normalized,"停止测肤") ||
     strstr(normalized,"退出云台"))return K7_VOICE_STOP;
  if(strstr(normalized,"停止播报"))return K7_VOICE_QUIET;
  if(strstr(normalized,"不要") || strstr(normalized,"别启动") || strstr(normalized,"取消启动") ||
     strstr(normalized,"不启动") || strstr(normalized,"别检测") || strstr(normalized,"取消检测") ||
     strstr(normalized,"不检测") || strstr(normalized,"别检查") || strstr(normalized,"取消检查") ||
     strstr(normalized,"不检查"))return K7_VOICE_NONE;
  /* User-selected fuzzy wake: any final containing this greeting.
   * Stop, quiet and negation above retain precedence. */
  if(strstr(normalized,"你好"))return K7_VOICE_TRACK;
  if(photo_request(text))return K7_VOICE_PHOTO;
  return K7_VOICE_NONE;
}

enum k7_voice_intent k7_voice_completed_text(const char *text,size_t bytes,
                                            bool verified,bool completed)
{
  if(!verified||!completed||!text||!bytes||bytes>2048||memchr(text,0,bytes))return K7_VOICE_NONE;
  for(size_t i=0;i<bytes;) {
    unsigned v=(unsigned char)text[i++],need=0,min=0;
    if(v<128) {if(v<32&&v!='\t'&&v!='\r'&&v!='\n')return K7_VOICE_NONE;if(v==127)return K7_VOICE_NONE;}
    else if(v>=0xc2&&v<=0xdf){need=1;min=0x80;v&=31;}
    else if(v>=0xe0&&v<=0xef){need=2;min=0x800;v&=15;}
    else if(v>=0xf0&&v<=0xf4){need=3;min=0x10000;v&=7;}
    else return K7_VOICE_NONE;
    if(bytes-i<need)return K7_VOICE_NONE;
    while(need--){unsigned b=(unsigned char)text[i++];if((b&0xc0)!=0x80)return K7_VOICE_NONE;v=(v<<6)|(b&63);}
    if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return K7_VOICE_NONE;
  }
  char bounded[2049];memcpy(bounded,text,bytes);bounded[bytes]=0;
  return command(bounded);
}

int k7_voice_event(struct k7_voice_result *out,const char *json,size_t size)
{
  if(!out || !json || !size || size>8192 || out->completed ||
     memchr(json,0,size))return -EINVAL;
  /* cJSON strings are NUL terminated: reject an escaped NUL before decoding. */
  for(size_t i=0;i+6<=size;i++)
    if(!memcmp(json+i,"\\u0000",6))return -EINVAL;
  const char *end=NULL;
  cJSON *root=cJSON_ParseWithLengthOpts(json,size,&end,0);
  int rc=-EINVAL;
  if(!root || !cJSON_IsObject(root))goto done;
  while(end<json+size && (*end==' '||*end=='\n'||*end=='\r'||*end=='\t'))end++;
  if(end!=json+size)goto done;
  for(cJSON *a=root->child;a;a=a->next)
    for(cJSON *b=a->next;b;b=b->next)
      if(!strcmp(a->string,b->string))goto done;
  cJSON *type=cJSON_GetObjectItemCaseSensitive(root,"type");
  if(!cJSON_IsString(type))goto done;
  if(!strcmp(type->valuestring,"partial")){rc=0;goto done;}
  if(!strcmp(type->valuestring,"completed")){out->completed=1;rc=0;goto done;}
  if(strcmp(type->valuestring,"final"))goto done;
  cJSON *text=cJSON_GetObjectItemCaseSensitive(root,"text");
  cJSON *id=cJSON_GetObjectItemCaseSensitive(root,"sentence_id");
  if(!cJSON_IsString(text) || !cJSON_IsNumber(id) ||
     !isfinite(id->valuedouble) || id->valuedouble<0 ||
     id->valuedouble>2147483647 || floor(id->valuedouble)!=id->valuedouble)goto done;
  enum k7_voice_intent intent=command(text->valuestring);
  if(out->finals>=32)goto done;
  /* A stop in any accepted final dominates earlier/later wake/photo text.
   * Still require completed, and never act on partial ASR hypotheses. */
  if(intent==K7_VOICE_STOP || out->intent==K7_VOICE_STOP)
    { out->intent=K7_VOICE_STOP;out->ambiguous=0; }
  else
    {
      if(!intent || (out->finals && intent!=out->intent))out->ambiguous=1;
      out->intent=intent;
    }
  out->finals++;rc=0;
done:
  cJSON_Delete(root);return rc;
}

enum k7_voice_intent k7_voice_result_intent(const struct k7_voice_result *result)
{
  return result && result->completed && result->finals && !result->ambiguous
    ? result->intent : K7_VOICE_NONE;
}
