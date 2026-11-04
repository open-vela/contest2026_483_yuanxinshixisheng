#include "vv_prompt_cache.h"
#include "board_voice_prompts.inc"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define PROMPTS (sizeof(k7_voice_prompts)/sizeof(k7_voice_prompts[0]))
_Static_assert(PROMPTS+5<=VV_PROMPT_CACHE_SLOTS,"fixed prompt cache capacity");
static void wipe(void *p,size_t n){volatile unsigned char *v=p;while(n--)*v++=0;}
static int index_of(const char *text){
 if(!text)return -1;
 for(size_t i=0;i<PROMPTS;i++)if(!strcmp(text,k7_voice_prompts[i].text))return (int)i;
 if(!strcmp(text,"OpenVela 云台已启动。"))return (int)PROMPTS;
 if(!strcmp(text,"三视图拍摄完成，正在处理，请稍候。"))return (int)PROMPTS+1;
 if(!strcmp(text,"配网成功。"))return (int)PROMPTS+2;
 if(!strcmp(text,"三视图上传成功，正在生成报告。"))return (int)PROMPTS+3;
 if(!strcmp(text,"皮肤检测报告已完成。"))return (int)PROMPTS+4;
 return -1;
}
bool vv_prompt_cache_eligible(const char *text){return index_of(text)>=0;}
int vv_prompt_cache_seed(struct vv_prompt_cache *s,const struct vv_prompt_pcm_asset *a,size_t count){
 if(!s||(!a&&count)||count>VV_PROMPT_CACHE_SLOTS)return -EINVAL;
 if(s->bytes)return -EBUSY;
 size_t total=0;bool seen[VV_PROMPT_CACHE_SLOTS]={0};
 for(size_t j=0;j<count;j++){
  int i=index_of(a[j].text);size_t n=a[j].bytes;
  if(i<0||seen[i]||!a[j].pcm||!n||n%2||n>VV_PROMPT_CACHE_ITEM_MAX||n>VV_PROMPT_CACHE_TOTAL_MAX-total)return -EINVAL;
  seen[i]=true;total+=n;
 }
 for(size_t j=0;j<count;j++){int i=index_of(a[j].text);s->slot[i].pcm=a[j].pcm;s->slot[i].bytes=a[j].bytes;s->slot[i].owned=false;}
 s->bytes=total;return 0;
}
int vv_prompt_cache_view(struct vv_prompt_cache *s,const char *text,const unsigned char **pcm,size_t *bytes){
 if(pcm)*pcm=NULL;
 if(bytes)*bytes=0;
 if(!s||!text||!pcm||!bytes)return -EINVAL;
 int i=index_of(text);if(i<0||!s->slot[i].pcm)return 0;
 *pcm=s->slot[i].pcm;*bytes=s->slot[i].bytes;return 1;
}
int vv_prompt_cache_get(struct vv_prompt_cache *s,const char *text,unsigned char *out,size_t cap,
 size_t *used,bool (*cancel)(void *),void *arg){
 if(used)*used=0;
 if(!s||!text||!out||!used)return -EINVAL;
 if(cancel&&cancel(arg))return -ECANCELED;
 int i=index_of(text);if(i<0||!s->slot[i].pcm)return 0;
 size_t n=s->slot[i].bytes;if(n>cap)return -ENOSPC;
 memcpy(out,s->slot[i].pcm,n);
 if(cancel&&cancel(arg)){wipe(out,n);return -ECANCELED;}
 *used=n;return 1;
}
int vv_prompt_cache_put(struct vv_prompt_cache *s,const char *text,const unsigned char *pcm,size_t n,
 bool (*cancel)(void *),void *arg){
 if(!s||!text||!pcm||!n||n%2)return -EINVAL;
 if(cancel&&cancel(arg))return -ECANCELED;
 int i=index_of(text);if(i<0||n>VV_PROMPT_CACHE_ITEM_MAX)return 0;
 if(s->slot[i].pcm)return 1;
 if(n>VV_PROMPT_CACHE_TOTAL_MAX-s->bytes)return 0;
 unsigned char *copy=malloc(n);if(!copy)return 0;
 memcpy(copy,pcm,n);
 if(cancel&&cancel(arg)){wipe(copy,n);free(copy);return -ECANCELED;}
 s->slot[i].pcm=copy;s->slot[i].bytes=n;s->slot[i].owned=true;s->bytes+=n;return 1;
}
void vv_prompt_cache_clear(struct vv_prompt_cache *s){
 if(!s)return;
 for(size_t i=0;i<VV_PROMPT_CACHE_SLOTS;i++)if(s->slot[i].pcm&&s->slot[i].owned){wipe((void*)s->slot[i].pcm,s->slot[i].bytes);free((void*)s->slot[i].pcm);}
 wipe(s,sizeof(*s));
}
