#ifndef VV_REAL_SHUIGUANG_H
#define VV_REAL_SHUIGUANG_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifndef K7_REAL_SHUIGUANG_PROFILE
#define K7_REAL_SHUIGUANG_PROFILE 0
#endif
#define VV_REAL_WAIT_VOICE_STOP (-5201) /* Exact finalized report-wait STOP only. */
#if K7_REAL_SHUIGUANG_PROFILE
struct vv_native_runtime;
/* Only an already admitted report's local failure cleanup may use this.
 * Ignore link/business-session availability, never trust or physical safety. */
bool vv_native_platform_local_failure_ready(struct vv_native_runtime *);
struct vv_backend_task;struct vv_backend_brief;struct vv_https_cancel;
int vv_native_platform_wait_task(void *,const char *,struct vv_backend_task *,
 struct vv_backend_brief *,const struct vv_https_cancel *);
int vv_native_platform_wait_task_progress(void *,const char *,struct vv_backend_task *,
 struct vv_backend_brief *,const struct vv_https_cancel *,
 int (*)(void *,const struct vv_https_cancel *),void *);
#endif
#define VV_REAL_BUSINESS_HOST "meeting-serial-bids-launches.trycloudflare.com"
#define VV_REAL_BUSINESS_PORT 443u
#define VV_REAL_BUSINESS_BASE "/openvela"
static inline bool vv_real_business_target(const char *host,uint16_t port,const char *base,bool plain){
 return host&&base&&!plain&&port==VV_REAL_BUSINESS_PORT&&!strcmp(host,VV_REAL_BUSINESS_HOST)&&!strcmp(base,VV_REAL_BUSINESS_BASE);
}
/* Validate server text without generating scores or appending advice. */
static inline bool vv_real_report_text(const char *s,size_t cap,bool required){
 if(!s)return false;
 size_t n=0;while(n<cap&&s[n])++n;
 if(n==cap||(!n&&required))return false;
 for(size_t i=0;i<n;){
  unsigned v=(unsigned char)s[i++],more=0,min=0;
  if(v<128){if(v<32||v==127)return false;}
  else if(v>=0xc2&&v<=0xdf){more=1;min=0x80;v&=31;}
  else if(v>=0xe0&&v<=0xef){more=2;min=0x800;v&=15;}
  else if(v>=0xf0&&v<=0xf4){more=3;min=0x10000;v&=7;}
  else return false;
  if(n-i<more)return false;
  while(more--){unsigned b=(unsigned char)s[i++];if((b&0xc0)!=0x80)return false;v=(v<<6)|(b&63);}
  if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return false;
 }
 return true;
}
#endif
