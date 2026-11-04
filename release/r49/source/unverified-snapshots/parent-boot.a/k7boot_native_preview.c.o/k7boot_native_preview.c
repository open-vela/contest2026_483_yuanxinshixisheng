/* Explicit native-cloud preview adapter; never contacts a PC gateway. */
#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include "native_boot_sequence.h"
#include "../k7radio/k7_radio_service.h"
#include "../k7host/k7_pipeline.h"
#include "../k7display/rk3576_display_native.h"
#include "../k7agent/cloud/include/vv_native_service.h"
extern int k7host_main(int,char **);
extern int k7host_headless_camera_ready(void);
extern int k7host_panel_preview_ready(void);
static atomic_bool started,finished;
static atomic_int result;
static int fail(int rc){
 if(rc>=0)rc=-EIO;
 atomic_store(&result,rc);atomic_store(&finished,true);
 vv_native_service_stop();k7_pipeline_halt();return rc;
}
static int camera(int argc,char **argv){
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_CONTINUOUS
#if defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE) && !defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_DISPLAY)
 char *args[]={"k7host","voicecam-service",0};
#else
 char *args[]={"k7host","voicepanel-service",0};
#endif
 (void)argc;(void)argv;
 int rc=k7host_main(2,args);fail(rc?-EIO:-EPIPE);return rc;
#else
#if defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE) && !defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_DISPLAY)
 /* Existing voicecam preserves the same vision/voice hold pipeline while
  * omitting the optional LCD sidecar. Do not light the panel during trials. */
 char *args[]={"k7host","voicecam","10800",0};(void)argc;(void)argv;
#else
 char *args[]={"k7host","voicepanel","10800",0};(void)argc;(void)argv;
#endif
 int rc=k7host_main(3,args);fail(rc?-EIO:-EPIPE);return rc;
#endif
}
int vv_native_boot_preview_start(void *arg){
 char *args[]={"k7host","start",0};(void)arg;
 if(atomic_exchange(&started,true))return -EALREADY;
 /* Existing one-attempt resource/PLL/display guards remain authoritative.
  * Never accept EALREADY as evidence that an unrelated session is usable. */
#if !defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE) || defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_DISPLAY)
 int rc=k7display_native_start();if(rc)return fail(rc);
 rc=k7display_video_ready();if(rc)return fail(rc);
#else
 puts("VVALIYUN display_start=0 visual_pipeline=voicecam");
#endif
 if(k7host_main(2,args))return fail(-EIO);
 return 0;
}
int vv_native_boot_preview_ready(void *arg){
 static bool worker_started;int rc;(void)arg;
 if(!atomic_load(&started))return -ENODEV;
 if(atomic_load(&finished))return atomic_load(&result);
#if !defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE) || defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_DISPLAY)
 rc=k7display_video_ready();if(rc)return fail(rc);
#endif
 if(!worker_started){
  rc=k7host_headless_camera_ready();
  if(rc)return rc==-EAGAIN?-ENXIO:fail(rc);
  int pid=task_create("vvnativepanel",100,65536,camera,0);
  if(pid<0)return fail(errno?-errno:-EIO);
  worker_started=true;
 }
 if(atomic_load(&finished))return atomic_load(&result);
 /* A colorbar scanout alone is insufficient: require a camera frame that
  * was successfully committed by the independent LCD sidecar. */
#if !defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE) || defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE_DISPLAY)
 rc=k7host_panel_preview_ready();
 if(rc)return (rc==-ENODEV||rc==-EAGAIN)?-EAGAIN:fail(rc);
#endif
 rc=k7_pipeline_voice_mode_status(0);
 if(rc==-ENODEV||rc==-EAGAIN)return -EAGAIN;
 return rc?fail(rc):0;
}
int vv_native_boot_network_ready(void *arg){
 struct k7radio_link_snapshot s;(void)arg;int rc=k7radio_get_link_snapshot(&s);
 return rc?rc:(s.wifi_connected&&s.ipv4_ready?0:-EAGAIN);
}
void vv_native_boot_stop_hold(void *arg){(void)arg;vv_native_service_stop();k7_pipeline_halt();}
