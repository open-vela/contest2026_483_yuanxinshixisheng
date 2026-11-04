#include "vv_native_runtime.h"
#include "vv_local_assessment.h"
#include "capture_stream.h"
#include "k7sound_api.h"
#include "k7_photo.h"
#include "k7_pipeline.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_session_pcm.h"
#endif
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) &&     defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
#include "vv_paraformer_k7.h"
#endif
#ifdef CONFIG_SMP
#include <sched.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_NATIVE_ENDPOINT
#include "command_endpoint.h"
#endif
struct capture {unsigned char *pcm;size_t cap,used;const struct vv_https_cancel *cancel;int error;unsigned samples;int result;atomic_bool done;
#ifdef CONFIG_EXAMPLES_K7CLOUD_NATIVE_ENDPOINT
 struct k7_command_endpoint endpoint;
#endif
};
static int sample(void *arg,uint32_t left,uint32_t right){struct capture *s=arg;(void)right;if(s->error)return s->error;if(s->cancel&&s->cancel->is_cancelled&&s->cancel->is_cancelled(s->cancel->arg))return s->error=-ECANCELED;if(s->cap-s->used<2)return s->error=-EOVERFLOW;s->pcm[s->used++]=(unsigned char)(left>>16);s->pcm[s->used++]=(unsigned char)(left>>24);
#ifdef CONFIG_EXAMPLES_K7CLOUD_NATIVE_ENDPOINT
 k7_endpoint_sample(&s->endpoint,(int16_t)(left>>16));
 /* Positive sink return ends PIO capture successfully after this sample. */
 if(s->used%640u==0 && k7_endpoint_frame(&s->endpoint))return 1;
#endif
 return 0;}
static void *record(void *arg){
 struct capture *s=arg;
 s->result=k7sound_capture_stream_cued(s->samples,sample,s);
 atomic_store_explicit(&s->done,true,memory_order_release);
 return NULL;
}
static int capture(void *arg,unsigned samples,unsigned char *pcm,size_t cap,size_t *used,const struct vv_https_cancel *cancel){
 (void)arg;
 if(!pcm||!used||samples<320||samples>48000||samples%320||cap<(size_t)samples*2)return -EINVAL;
 *used=0;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 struct capture s={.pcm=pcm,.cap=cap,.cancel=cancel,.samples=samples};
 atomic_init(&s.done,false);
 pthread_attr_t attr;pthread_t thread;
 int err=pthread_attr_init(&attr);if(err)return -err;
 /* Match the established board speech bridge's PIO capture scheduling.
  * The caller joins before upload; no socket work runs on this CPU1 worker. */
 err=pthread_attr_setstacksize(&attr,32768);
#ifdef CONFIG_SMP
 if(!err){cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(1,&cpus);
  err=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);}
#endif
 if(!err)err=pthread_create(&thread,&attr,record,&s);
 pthread_attr_destroy(&attr);
 if(err)return -err;
 err=pthread_join(thread,NULL);
 if(err){
  /* Even an unexpected join failure cannot release borrowed PCM/cancel
   * storage while the bounded recorder can still use it. */
  while(!atomic_load_explicit(&s.done,memory_order_acquire)){
   struct timespec pause={0,1000000};nanosleep(&pause,NULL);
  }
  (void)pthread_detach(thread);
  return -err;
 }
 int rc=s.error?s.error:s.result;
 if(!rc)*used=s.used;
 return rc;
}
static int status(void *arg,uint32_t *epoch,unsigned *mask){(void)arg;return k7_photo_native_status(epoch,mask);}
static int copy(void *arg,uint32_t epoch,unsigned side,unsigned char *dst,size_t cap,size_t *size){(void)arg;struct k7_photo_record r={0};int rc=k7_photo_native_copy(epoch,side,&r,dst,cap);if(!rc&&(r.epoch!=epoch||r.side!=side||!r.bytes||r.bytes>cap))rc=-EIO;if(size)*size=rc?0:r.bytes;return rc;}
int vv_native_runtime_k7_discard(uint32_t epoch){return k7_photo_native_discard(epoch);}
static int held(void *arg,unsigned *revision){(void)arg;return k7_pipeline_hold_revision(revision);}
static int resume(void *arg,unsigned revision){(void)arg;return k7_pipeline_resume_tracking(revision);}
static int playback_cancelled(void *arg){const struct vv_https_cancel *c=arg;return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static int play(void *arg,const unsigned char *pcm,size_t bytes,const struct vv_https_cancel *cancel){
 (void)arg;
 if(!pcm||!bytes||bytes>960000||bytes%2||(uintptr_t)pcm%_Alignof(int16_t))return -EINVAL;
 if(playback_cancelled((void *)cancel))return -ECANCELED;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 return vv_session_pcm_play(pcm,bytes,cancel);
#else
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) &&     defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
 int cleanup=vv_paraformer_k7_close();if(cleanup)return cleanup;
#endif
 return k7sound_speak_mono16_cancel((const int16_t *)pcm,(unsigned)(bytes/2),playback_cancelled,(void *)cancel);
#endif
}
void vv_native_runtime_k7_ops(struct vv_native_runtime_ops *ops,bool (*ready)(void *),void *arg){if(ops)*ops=(struct vv_native_runtime_ops){.ready=ready,.capture=capture,.photo_status=status,.photo_copy=copy,.hold_revision=held,.resume=resume,.arg=arg,.play=play};}
