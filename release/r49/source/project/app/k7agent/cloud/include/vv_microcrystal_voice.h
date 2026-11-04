/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VV_MICROCRYSTAL_VOICE_H
#define VV_MICROCRYSTAL_VOICE_H
#define VV_MICROCRYSTAL_REPORT_FAILED (-2401)
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
enum vv_micro_voice_action { VV_MICRO_NONE, VV_MICRO_START, VV_MICRO_LEVEL, VV_MICRO_STOP, VV_MICRO_RELATIVE };
struct vv_micro_voice_command { enum vv_micro_voice_action action; int level; };
/* Exact final utterances only. Volume grammar is deliberately independent. */
static inline struct vv_micro_voice_command vv_micro_voice_parse(const char *text,bool active){
 struct vv_micro_voice_command no={VV_MICRO_NONE,0};
 if(!text||strlen(text)>=192)return no;
 char b[192];size_t n=0;
 for(size_t i=0;text[i];i++)if(text[i]!=' '&&text[i]!='\t'&&text[i]!='\r'&&text[i]!='\n')b[n++]=text[i];
 b[n]=0;char *s=b;
 /* Observed final ASR spelling for this instrument, not fuzzy matching.
  * Normalize the name, then still require one complete command below. */
 const char *const names[]={"微巾","危经","飞京","微经","围巾","维晶","微精","微镜"};
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)for(char *at=b;(at=strstr(at,names[i]))!=NULL;at+=strlen("微晶"))memcpy(at,"微晶",strlen("微晶"));
 const char *const marks[]={"。","！","!","."};
 for(unsigned i=0;i<sizeof(marks)/sizeof(marks[0]);i++){size_t z=strlen(marks[i]);if(n>=z&&!strcmp(b+n-z,marks[i])){b[n-=z]=0;break;}}
 const char *const requests[]={"你能不能帮我","你可以帮我","你能帮我","请你帮我","能不能帮我","可以帮我","请帮我","帮我","请"};
 bool polite=false;
 for(unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++){size_t z=strlen(requests[i]);if(!strncmp(s,requests[i],z)){s+=z;polite=true;break;}}
 if(polite){
  n=strlen(s);const char *const tails[]={"？","?","好不好","可以吗","好吗","吗"};
  for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<sizeof(tails)/sizeof(tails[0]);i++){size_t z=strlen(tails[i]);if(n>=z&&!strcmp(s+n-z,tails[i])){s[n-=z]=0;break;}}
 }
 n=strlen(s);const char *const particles[]={"吧","啊","呀"};
 for(unsigned i=0;i<sizeof(particles)/sizeof(particles[0]);i++){size_t z=strlen(particles[i]);if(n>=z&&!strcmp(s+n-z,particles[i])){s[n-=z]=0;break;}}
 const char *const starts[]={"启动微晶","打开微晶","开启微晶","开始微晶","连接微晶","微晶启动","微晶开启","启动一下微晶","打开一下微晶","开启一下微晶","把微晶启动","把微晶打开","开始微晶护理","启动微晶护理","微晶开始工作","微晶开始"};
 for(unsigned i=0;i<sizeof(starts)/sizeof(starts[0]);i++)if(!strcmp(s,starts[i]))return (struct vv_micro_voice_command){VV_MICRO_START,1};
 const char *const stops[]={"停止微晶","停止了微晶","关闭微晶","关掉微晶","断开微晶","微晶停止","微晶关闭","停止微晶护理","微晶停一下","微晶停止一下","把微晶停下来","把微晶关掉","停一下微晶","停下微晶","微晶停下来","停止一下微晶","停止微晶工作"};
 for(unsigned i=0;i<sizeof(stops)/sizeof(stops[0]);i++)if(!strcmp(s,stops[i]))return (struct vv_micro_voice_command){VV_MICRO_STOP,0};
 if(active&&(!strcmp(s,"停止")||!strcmp(s,"停下来")||!strcmp(s,"停一下")))return (struct vv_micro_voice_command){VV_MICRO_STOP,0};
 const char *const up[]={"微晶调高一档","微晶升一档","微晶加一档","再高一档","升一档","调高一档"};
 const char *const down[]={"微晶调低一档","微晶降一档","微晶减一档","再低一档","降一档","调低一档"};
 for(unsigned i=0;i<sizeof(up)/sizeof(up[0]);i++)if(!strcmp(s,up[i]))return (struct vv_micro_voice_command){VV_MICRO_RELATIVE,1};
 for(unsigned i=0;i<sizeof(down)/sizeof(down[0]);i++)if(!strcmp(s,down[i]))return (struct vv_micro_voice_command){VV_MICRO_RELATIVE,-1};
 const char *const prefixes[]={"把微晶调到","将微晶调到","微晶切换到","微晶切换成","微晶调整到","微晶调到","微晶调至","微晶设为","微晶设置为","微晶","切换到","切换成","调整到","调到","调至","切到","换到",""};
 const char *const levels[][4]={{"一档","1档","一挡","1挡"},{"二档","2档","二挡","2挡"},{"三档","3档","三挡","3挡"}};
 for(unsigned p=0;p<sizeof(prefixes)/sizeof(prefixes[0]);p++){
  size_t z=strlen(prefixes[p]);if(strncmp(s,prefixes[p],z))continue;
  for(int level=0;level<3;level++)for(unsigned k=0;k<4;k++)if(!strcmp(s+z,levels[level][k]))return (struct vv_micro_voice_command){VV_MICRO_LEVEL,level+1};
 }
 return no;
}
bool vv_microcrystal_manual_active(void);
int vv_microcrystal_manual_command(enum vv_micro_voice_action,int,char *,size_t);
void vv_microcrystal_manual_shutdown(void);
int vv_microcrystal_manual_stop_for_assessment(void);
#endif
