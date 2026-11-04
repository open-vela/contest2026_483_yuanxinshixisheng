#ifndef VV_VOICE_SETTINGS_H
#define VV_VOICE_SETTINGS_H
#include <string.h>
#include <stddef.h>
#include <errno.h>
struct vv_native_runtime;struct vv_https_cancel;
/* 1=committed, 0=not a setting/platform, negative=setting rejected. */
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
 if(!strncmp(s,"请",strlen("请")))s+=strlen("请");
 if(!strncmp(s,"帮我",strlen("帮我")))s+=strlen("帮我");
 if(!strcmp(s,"调大音量")||!strcmp(s,"音量调大")||!strcmp(s,"调高音量")||!strcmp(s,"音量调高")||!strcmp(s,"调大一点")||!strcmp(s,"调大")||!strcmp(s,"音量大一点")){
  *out=(struct vv_voice_setting){VV_SETTING_VOLUME_RELATIVE,10};return 1;}
 if(!strcmp(s,"调小音量")||!strcmp(s,"音量调小")||!strcmp(s,"调低音量")||!strcmp(s,"音量调低")||!strcmp(s,"调小一点")||!strcmp(s,"调小")||!strcmp(s,"音量小一点")){
  *out=(struct vv_voice_setting){VV_SETTING_VOLUME_RELATIVE,-10};return 1;}
 if(!strcmp(s,"换音色")||!strcmp(s,"切换音色")||!strcmp(s,"换个音色")||!strcmp(s,"换一个音色")||!strcmp(s,"换一下音色")){
  *out=(struct vv_voice_setting){VV_SETTING_VOICE,-1};return 1;}
 if(!strcmp(s,"换成流萤的音色")||!strcmp(s,"换成流萤音色")||!strcmp(s,"切换到流萤音色")||!strcmp(s,"切换成流萤的音色")||!strcmp(s,"换流萤音色")){
  *out=(struct vv_voice_setting){VV_SETTING_VOICE,1};return 1;}
 if(!strcmp(s,"恢复默认音色")||!strcmp(s,"换回原来的音色")){
  *out=(struct vv_voice_setting){VV_SETTING_VOICE,0};return 1;}
 const char *const prefixes[]={"音量调到","音量调整到","把音量调到","将音量调到","调大音量到","调小音量到","调大到","调小到","音量设为","音量设置为","把音量设置为"};
 const char *value=NULL;
 for(unsigned i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);i++){size_t z=strlen(prefixes[i]);if(!strncmp(s,prefixes[i],z)){value=s+z;break;}}
 if(!value)return 0;
 while(*value==' ')value++;
 char num[32];size_t bytes;
 if(!strncmp(value,"百分之",strlen("百分之"))){value+=strlen("百分之");bytes=strlen(value);}
 else {bytes=strlen(value);if(!bytes||value[bytes-1]!='%')return 0;bytes--;}
 if(!bytes||bytes>=sizeof(num))return -ERANGE;
 memcpy(num,value,bytes);num[bytes]=0;
 int v=vv_setting_number(num);if(v<0)return 0;
 if(v<1||v>100)return -ERANGE;
 *out=(struct vv_voice_setting){VV_SETTING_VOLUME,v};return 1;
}
#endif
