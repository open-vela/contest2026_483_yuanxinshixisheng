#include "k7boot_native.h"
#include "native_boot_sequence.h"
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
static atomic_bool started,cancelled;
static const struct vv_k7_platform_boot_config *config;
static vv_k7_secure_load_fn loader;static void *loader_arg;
static struct vv_native_platform *platform;
static bool speech_only;
int k7boot_native_configure(const struct vv_k7_platform_boot_config *c,vv_k7_secure_load_fn f,void *a){
 if(!c||!f)return -EINVAL;
 if(atomic_load(&started)||config)return -EBUSY;
 config=c;loader=f;loader_arg=a;return 0;
}
int k7boot_native_configure_speech_only(const struct vv_k7_platform_boot_config *c){
 if(!c||!c->business.speech)return -EINVAL;
 if(atomic_load(&started)||config)return -EBUSY;
 config=c;speech_only=true;return 0;
}
static bool stopped(void *a){(void)a;return atomic_load(&cancelled);}
static uint64_t now(void*a){struct timespec t;(void)a;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static void wait(void*a,unsigned ms){(void)a;usleep(ms*1000u);}
static int prepare(void*a){
 struct vv_native_platform_config c;unsigned missing;int rc;uint64_t t=now(a);
 struct vv_https_cancel cancel={stopped,0};
 if(!config||(!speech_only&&!loader))return -ENODATA;
 if(t>UINT64_MAX-30000)return -EOVERFLOW;
 rc=vv_k7_platform_factory_prepare(config,t+30000,&c,&missing);if(rc)return rc;
 if(speech_only&&!c.speech)return -EACCES;
 if(stopped(0))return -ECANCELED;
 platform=vv_native_platform_create(&c);if(!platform)return -EIO;
 rc=speech_only?0:vv_k7_platform_factory_login(platform,loader,loader_arg,&cancel);
 if(!rc&&!stopped(0))rc=vv_native_platform_bind(platform);
 else if(!rc)rc=-ECANCELED;
 return rc;
}
static int service(void*a){(void)a;return vv_native_service_run();}
void k7boot_native_cancel(void){atomic_store(&cancelled,true);vv_native_service_stop();}
int k7boot_native_worker(int argc,char **argv){
 (void)argc;(void)argv;if(atomic_exchange(&started,true))return -EALREADY;
 const struct vv_native_boot_ops o={vv_native_boot_preview_start,vv_native_boot_preview_ready,vv_native_boot_network_ready,prepare,service,vv_native_boot_stop_hold,stopped,now,wait,0};
 int rc=vv_native_boot_run(&o);
 if(platform){int cleanup=vv_native_platform_destroy(platform);if(!cleanup)platform=0;else if(!rc)rc=cleanup;}
 return rc;
}
