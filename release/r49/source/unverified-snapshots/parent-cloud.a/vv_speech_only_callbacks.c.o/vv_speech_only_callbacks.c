#include "vv_speech_only_callbacks.h"
#include "vv_speech_owner.h"
#include <errno.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
static unsigned missing(void *arg){
 struct vv_speech_only_callbacks *s=arg;unsigned bits=0;
 if(!s->preview_alive(s->preview_arg))bits|=VV_NATIVE_PREVIEW;
 if(!vv_speech_owner_native_available(s))bits|=VV_NATIVE_OWNER;
 return bits;
}
static int claim(void *arg){return vv_speech_owner_native_claim(arg);}
static void release(void *arg){vv_speech_owner_native_release(arg);}
static int request(void *arg,struct vv_assessment_request *q){(void)arg;(void)q;return -EACCES;}
static void release_request(void *arg,struct vv_assessment_request *q){(void)arg;(void)q;}
static void sleep_ms(void *arg,unsigned ms){
 (void)arg;struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};
 while(nanosleep(&t,&t)&&errno==EINTR){}
}
int vv_speech_only_callbacks_apply(struct vv_native_platform_config *c,struct vv_speech_only_callbacks *s){
 if(!c||!s||!s->preview_alive)return -EINVAL;
 c->missing=missing;c->claim=claim;c->release=release;c->make_request=request;
 c->release_request=release_request;c->sleep_ms=sleep_ms;c->arg=s;return 0;
}
bool vv_speech_only_k7_preview_alive(void *arg){
 (void)arg;
#if defined(__NuttX__) && defined(CONFIG_EXAMPLES_K7HOST_TRACK)
 extern int k7_pipeline_voice_session_status(void);
 return k7_pipeline_voice_session_status()==0;
#else
 return false;
#endif
}
