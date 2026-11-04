#include "vv_session_pcm.h"
#include "vv_stream_speaker.h"
#include "vv_voice_session.h"
#include "pcm_playback.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_SMP
#include <sched.h>
#endif
#define MAX_PCM_BYTES 960000u
#define BOOTSTRAP_TIMEOUT_MS 35000u
struct immutable_pcm {const unsigned char *pcm;size_t bytes;};
static bool is_cancelled(const struct vv_https_cancel *cancel){
 return cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg);
}
static bool valid(const unsigned char *pcm,size_t bytes){
 return pcm&&bytes&&bytes<=MAX_PCM_BYTES&&bytes%2==0;
}
static int fetch_immutable(void *arg,vv_stream_pcm_sink sink,void *sink_arg,
 const struct vv_https_cancel *cancel){
 const struct immutable_pcm *source=arg;
 for(size_t at=0;at<source->bytes;){
  /* This is the speaker's private atomic token, not the external guard. */
  if(is_cancelled(cancel))return -ECANCELED;
  size_t bytes=source->bytes-at;if(bytes>8192)bytes=8192;
  int rc=sink(sink_arg,source->pcm+at,bytes);if(rc)return rc;
  at+=bytes;
 }
 return is_cancelled(cancel)?-ECANCELED:0;
}
int vv_session_pcm_play(const unsigned char *pcm,size_t bytes,
 const struct vv_https_cancel *cancel){
 if(!valid(pcm,bytes))return -EINVAL;
 if(is_cancelled(cancel))return -ECANCELED;
 const struct immutable_pcm source={pcm,bytes};
 int rc=vv_stream_speaker_run(fetch_immutable,(void*)&source,cancel);
 if(!rc&&is_cancelled(cancel))rc=-ECANCELED;
 return rc;
}
struct bootstrap {
 struct immutable_pcm source;size_t offset;
 atomic_bool stop,done;int result;
};
static pthread_mutex_t bootstrap_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t now_ms(void){
 struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||
    (uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static void pause_ms(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static int bootstrap_cancel(void *arg){return atomic_load(&((struct bootstrap*)arg)->stop);}
static int bootstrap_pull(void *arg,int16_t *value){
 struct bootstrap *b=arg;if(bootstrap_cancel(b))return -ECANCELED;
 if(b->offset==b->source.bytes)return 0;
 const unsigned char *p=b->source.pcm+b->offset;
 *value=(int16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));b->offset+=2;return 1;
}
static void *bootstrap_worker(void *arg){
 struct bootstrap *b=arg;
 b->result=k7sound_speak_stream16(bootstrap_pull,b,bootstrap_cancel,b);
 atomic_store_explicit(&b->done,true,memory_order_release);return NULL;
}
static int bootstrap_launch(pthread_t *thread,struct bootstrap *b){
 pthread_attr_t attr;int err=pthread_attr_init(&attr);if(err)return -err;
 err=pthread_attr_setstacksize(&attr,32768);
#ifdef CONFIG_SMP
 if(!err){cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(1,&cpus);
  err=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);}
#else
 /* This path is deliberately not a CPU0/SMP-disabled playback workaround. */
 if(!err)err=ENOTSUP;
#endif
 if(!err)err=pthread_create(thread,&attr,bootstrap_worker,b);
 pthread_attr_destroy(&attr);return -err;
}
int vv_bootstrap_pcm_play(const unsigned char *pcm,size_t bytes,
 const struct vv_https_cancel *cancel){
 if(!valid(pcm,bytes))return -EINVAL;
 if(is_cancelled(cancel))return -ECANCELED;
 int rc=pthread_mutex_trylock(&bootstrap_lock);if(rc)return -rc;
 /* Boot sequencing calls this before manager start. The low-level audio
  * owner's existing mutex also rejects a concurrent diagnostic/late start. */
 if(vv_voice_session_audio()){rc=-EBUSY;goto done;}
 uint64_t began=now_ms();if(began==UINT64_MAX){rc=-EIO;goto done;}
 struct bootstrap b={.source={pcm,bytes}};
 atomic_init(&b.stop,false);atomic_init(&b.done,false);
 pthread_t thread;rc=bootstrap_launch(&thread,&b);
 if(!rc){
  int coordination=0;
  while(!atomic_load_explicit(&b.done,memory_order_acquire)){
   if(!coordination){
    if(is_cancelled(cancel))coordination=-ECANCELED;
    uint64_t current=now_ms();
    if(!coordination&&(current==UINT64_MAX||current<began||current-began>=BOOTSTRAP_TIMEOUT_MS))coordination=-ETIMEDOUT;
    if(coordination)atomic_store(&b.stop,true);
   }
   pause_ms();
  }
  int joined=pthread_join(thread,NULL);
  if(joined){
   /* done already proves PIO cleanup and that borrowed state is no longer
    * referenced. Never leave a worker using a returning stack frame. */
   (void)pthread_detach(thread);rc=-joined;
  }else rc=b.result;
  if(!coordination&&is_cancelled(cancel))coordination=-ECANCELED;
  if(coordination&&(!rc||rc==-ECANCELED))rc=coordination;
 }
done:pthread_mutex_unlock(&bootstrap_lock);return rc;
}
