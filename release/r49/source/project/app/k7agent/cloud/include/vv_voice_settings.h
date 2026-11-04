#ifndef VV_VOICE_SETTINGS_H
#define VV_VOICE_SETTINGS_H
#include <string.h>
#include <stddef.h>
#include <errno.h>
struct vv_native_runtime;struct vv_https_cancel;
/* 1=committed, 2=setting rejected without ending the dialogue,
 * 0=not a setting/platform, negative=cancellation or operational failure. */
int vv_native_platform_local_setting(struct vv_native_runtime *,const char *,char *,size_t,const struct vv_https_cancel *);
/* Exact completed utterances only; no substring, quoted prose or negation.
 * 1=recognized, 0=ordinary text, negative=recognized invalid setting. */
enum vv_voice_setting_kind { VV_SETTING_VOLUME=1,VV_SETTING_VOLUME_RELATIVE=2,VV_SETTING_VOICE=3 };
struct vv_voice_setting { enum vv_voice_setting_kind kind; int value; };
static inline int vv_setting_digit(const char **p){
 static const char *const digits[]={"零","一","二","三","四","五","六","七","八","九"};
 for(int i=0;i<10;i++)if(!strncmp(*p,digits[i],strlen(digits[i]))){*p+=strlen(digits[i]);return i;}
 if(!strncmp(*p,"两",strlen("两"))){*p+=strlen("两");return 2;}
 return -1;
}
static inline int vv_setting_number(const char *s){
 if(!*s)return -1;
 if(*s>='0'&&*s<='9'){
  int n=0;unsigned count=0;
  for(;*s;s++){if(*s<'0'||*s>'9'||++count>3)return -1;n=n*10+*s-'0';}
  return n;
 }
 if(!strcmp(s,"一百")||!strcmp(s,"百"))return 100;
 const char *p=s;int first=vv_setting_digit(&p),n=0;
 if(first<0){p=s;if(strncmp(p,"十",strlen("十")))return -1;n=10;p+=strlen("十");}
 else if(!*p)return first;
 else if(!strncmp(p,"十",strlen("十"))&&first>0){n=first*10;p+=strlen("十");}
 else return -1;
 if(*p){int last=vv_setting_digit(&p);if(last<=0||*p)return -1;n+=last;}
 return n;
}
static inline int vv_voice_setting_parse(const char *text,struct vv_voice_setting *out){
 if(!text||!out)return -EINVAL;
 char b[192];size_t n=strlen(text);if(n>=sizeof(b))return 0;
 memcpy(b,text,n+1);char *s=b;
 while(*s==' '||*s=='\t')s++;
 n=strlen(s);while(n&&(s[n-1]==' '||s[n-1]=='\t'))s[--n]=0;
 const char *const ends[]={"。","！","!"};
 for(unsigned i=0;i<3;i++){size_t z=strlen(ends[i]);if(n>=z&&!strcmp(s+n-z,ends[i])){s[n-=z]=0;break;}}
 /* A polite request question is executable only when its entire remaining
  * body is one of the exact setting commands below. Discussion is not. */
 const char *const requests[]={"你能不能帮我","你可以帮我","你能否帮我","你能帮我","请你帮我","能不能帮我","可以帮我","能否帮我","能帮我","请帮我","帮我"};
 for(unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++){
  size_t z=strlen(requests[i]);if(strncmp(s,requests[i],z))continue;
  s+=z;n=strlen(s);
  if(n&&s[n-1]=='?')s[--n]=0;
  else if(n>=strlen("？")&&!strcmp(s+n-strlen("？"),"？"))s[n-=strlen("？")]=0;
  const char *const tails[]={"好不好","可以吗","好吗","吗"};
  for(unsigned j=0;j<sizeof(tails)/sizeof(tails[0]);j++){
   size_t tail=strlen(tails[j]);if(n>=tail&&!strcmp(s+n-tail,tails[j])){s[n-=tail]=0;break;}
  }
  break;
 }
 if(!strncmp(s,"请",strlen("请")))s+=strlen("请");
 if(!strncmp(s,"帮我",strlen("帮我")))s+=strlen("帮我");
 if(!strcmp(s,"调大音量")||!strcmp(s,"音量调大")||!strcmp(s,"调高音量")||!strcmp(s,"音量调高")||!strcmp(s,"调大一点")||!strcmp(s,"调大")||!strcmp(s,"音量大一点")||!strcmp(s,"再大一点")||!strcmp(s,"声音大一点")||!strcmp(s,"调大一点音量")||!strcmp(s,"再调大一点")||!strcmp(s,"音量调大一点")){
  *out=(struct vv_voice_setting){VV_SETTING_VOLUME_RELATIVE,10};return 1;}
 if(!strcmp(s,"调小音量")||!strcmp(s,"音量调小")||!strcmp(s,"调低音量")||!strcmp(s,"音量调低")||!strcmp(s,"调小一点")||!strcmp(s,"调小")||!strcmp(s,"音量小一点")||!strcmp(s,"再小一点")||!strcmp(s,"声音小一点")||!strcmp(s,"调小一点音量")||!strcmp(s,"再调小一点")||!strcmp(s,"音量调小一点")){
  *out=(struct vv_voice_setting){VV_SETTING_VOLUME_RELATIVE,-10};return 1;}
 if(!strcmp(s,"换音色")||!strcmp(s,"切换音色")||!strcmp(s,"换个音色")||!strcmp(s,"换一个音色")||!strcmp(s,"换一下音色")){
  *out=(struct vv_voice_setting){VV_SETTING_VOICE,-1};return 1;}
 if(!strcmp(s,"换成流萤的音色")||!strcmp(s,"换成流萤音色")||!strcmp(s,"切换到流萤音色")||!strcmp(s,"切换成流萤的音色")||!strcmp(s,"换流萤音色")){
  *out=(struct vv_voice_setting){VV_SETTING_VOICE,1};return 1;}
 if(!strcmp(s,"恢复默认音色")||!strcmp(s,"换回原来的音色")){
  *out=(struct vv_voice_setting){VV_SETTING_VOICE,0};return 1;}
 const char *const prefixes[]={"音量调到","音量调整到","把音量调到","将音量调到","调大音量到","调小音量到","调大到","调小到","音量设为","音量设置为","把音量设置为",
  "调音量到","调音量至","音量调至","音量调整至","把音量调至","将音量调至","调至","调到"};
 const char *value=NULL;
 for(unsigned i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);i++){size_t z=strlen(prefixes[i]);if(!strncmp(s,prefixes[i],z)){value=s+z;break;}}
 if(!value)return 0;
 while(*value==' ')value++;
 char num[32];size_t bytes;
 /* Bare numbers require an explicit volume anchor. Unanchored commands keep
  * requiring a percentage; unrelated "调到80" must remain ordinary text. */
 if(!strncmp(value,"百分之",strlen("百分之"))||!strncmp(value,"百分至",strlen("百分至"))||!strncmp(value,"百分比",strlen("百分比"))){value+=strlen("百分之");bytes=strlen(value);}
 else {bytes=strlen(value);if(bytes&&value[bytes-1]=='%')bytes--;else if(!strstr(s,"音量"))return 0;}
 if(!bytes||bytes>=sizeof(num))return -ERANGE;
 memcpy(num,value,bytes);num[bytes]=0;
 int v=vv_setting_number(num);if(v<0)return 0;
 if(v<1||v>100)return -ERANGE;
 *out=(struct vv_voice_setting){VV_SETTING_VOLUME,v};return 1;
}
#endif
