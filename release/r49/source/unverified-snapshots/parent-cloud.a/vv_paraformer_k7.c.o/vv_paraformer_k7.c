#include "vv_paraformer_k7.h"
#include "vv_aliyun_endpoint.h"
#include "vv_wss_client.h"
#include "capture_stream.h"
#include "stream_ring.h"
#include "command_endpoint.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH
#if !defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION) || !defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP)
#error "K7_PF_WAKE_PREFETCH requires VOICE_SESSION and ADC_OVERLAP"
#endif
#endif
#ifdef CONFIG_SMP
#include <sched.h>
#endif
static void sleep_ms(void*a,unsigned ms){(void)a;struct timespec pause={ms/1000,(long)(ms%1000)*1000000};nanosleep(&pause,NULL);}
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_voice_session.h"
#include "voice_session.h"
/* The manager owns session lifetime. This lock protects its one ASR consumer;
 * none of these callbacks acquire, stop or release the physical audio owner. */
static pthread_mutex_t session_capture_lock=PTHREAD_MUTEX_INITIALIZER;
static struct vv_pf_wake_source wake_pending;
static bool wake_armed;
int vv_paraformer_k7_wake_arm(const struct vv_pf_wake_source *source){
 if(!source||!source->health||!source->take||!source->finish||
    !source->request.id||!source->request.context.owner_epoch||
    !source->request.context.input_epoch||source->request.keyword_end<=source->request.sample_first||
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
    (source->request.keyword_end-source->request.sample_first)%320)return -EINVAL;
#else
    source->request.sample_first%320||source->request.keyword_end%320)return -EINVAL;
#endif
 if(pthread_mutex_trylock(&session_capture_lock))return -EBUSY;
 int rc=wake_armed?-EBUSY:0;
 if(!rc){wake_pending=*source;wake_armed=true;}
 pthread_mutex_unlock(&session_capture_lock);return rc;
}
int vv_paraformer_k7_wake_disarm(void){
 if(pthread_mutex_trylock(&session_capture_lock))return -EBUSY;
 int rc=0;
 if(wake_armed){struct vv_pf_wake_source source=wake_pending;
  memset(&wake_pending,0,sizeof(wake_pending));wake_armed=false;
  uint64_t first=0,next=0;rc=source.finish(source.arg,&source.request,true,&first,&next);
 }
 pthread_mutex_unlock(&session_capture_lock);return rc;
}
struct capture {
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH
 uint8_t *prefetch;unsigned prefetched,uploaded;
#endif
 struct vv_pf_wake_source wake;
 bool wake_used,wake_released;
 int wake_error;
 struct k7_voice_session *session;
 const struct vv_https_cancel *cancel;
 unsigned samples,received;uint32_t epoch;
 uint64_t first_sample,next_sample,finish_sample,owner_epoch;
 bool live,done,finish,have_frame,joined_normal;int result;
 vv_https_now_ms_fn now;void *clock_arg;uint64_t gate_deadline;
 atomic_bool admitted;
};
static int session_health(struct capture*c){
 struct k7_voice_session_status s;k7_voice_session_status(c->session,&s);
 if(s.finished||!s.running||!s.ready||s.stop_requested){
  /* Preserve explicit cancellation; other physical-owner failures have a
   * dedicated provenance distinct from network EIO/ENOSPC/EOVERFLOW. The
   * service still checks cancellation and joins/validates physical cleanup
   * before admitting its single replacement. */
  if(s.result==-ECANCELED)return -ECANCELED;
  return VV_ASR_OWNER_UNAVAILABLE;
 }
 if(c->wake_used){
  if(vv_voice_session_owner_epoch(c->session)!=c->wake.request.context.owner_epoch)
   return K7_VOICE_SESSION_STALE;
  if(c->wake_released)return 0;
  int rc=c->wake.health(c->wake.arg,&c->wake.request);
  return rc==-ENOSPC?VV_ASR_WINDOW_OVERRUN:rc;
 }
 if(c->epoch&&s.asr_failed_epoch==c->epoch)return -ENOSPC;
 if(c->live&&s.asr_epoch!=c->epoch)return K7_VOICE_SESSION_STALE;
 return 0;
}
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH
static int prefetch_poll(struct capture *c);
#endif
static bool wake_cancelled(void *arg){
 struct capture*c=arg;
 if(c->cancel&&c->cancel->is_cancelled&&c->cancel->is_cancelled(c->cancel->arg))return true;
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH
 if(c->prefetch){int rc=prefetch_poll(c);if(rc){if(!c->wake_error)c->wake_error=rc;return true;}}
#endif
 if(c->wake_used&&!c->wake_released){
  int rc=c->wake.health(c->wake.arg,&c->wake.request);
  if(rc){if(!c->wake_error)c->wake_error=rc==-ENOSPC?VV_ASR_WINDOW_OVERRUN:rc;return true;}
 }
 return false;
}
static int prepare_capture(void*a){
 struct capture*c=a;c->session=vv_voice_session_audio();
 return c->session?session_health(c):-ENOTCONN;
}
static int finish_capture(struct capture*c,int result){
 if(c->wake_used){
  if(!c->wake_released){uint64_t first=0,next=0;
   bool cancel=result!=0||!c->live;
   int rc=c->wake.finish(c->wake.arg,&c->wake.request,cancel,&first,&next);
   c->wake_released=true;
   if(!result&&rc)result=rc==-ENOSPC?VV_ASR_WINDOW_OVERRUN:rc;
   if(!result&&(first!=c->wake.request.sample_first||next!=c->next_sample||
      next<c->wake.request.keyword_end||!c->received||next-first!=c->received))result=-EPROTO;
  }
  c->live=false;c->done=true;c->result=result;return result;
 }
 if(result==-ENOSPC&&c->session&&c->epoch){
  struct k7_voice_session_status s;k7_voice_session_status(c->session,&s);
  if(s.asr_failed_epoch==c->epoch)result=VV_ASR_WINDOW_OVERRUN;
 }
 /* Disable only our own epoch. A manager shutdown may already have disabled
  * it; an unexpectedly newer epoch must never be closed by old cleanup. */
 if(c->live){
  struct k7_voice_session_status s;k7_voice_session_status(c->session,&s);
  if(s.asr_epoch==c->epoch){int rc=k7_voice_session_asr_epoch(c->session,0);if(!result&&rc)result=rc;}
  else if(s.asr_epoch&&!result)result=K7_VOICE_SESSION_STALE;
 }
 c->live=false;c->done=true;c->result=result;return result;
}
static int start_capture(void*a){
 struct capture*c=a;if(c->live||c->done)return -EALREADY;
 int rc=prepare_capture(c);if(rc)return rc;
 /* Snapshot this admitted window's owner; service keeps that borrowed owner
  * alive through the entire provider call. Provisional ASR epochs below do
  * not replace this physical owner identity. Zero remains ineligible. */
 c->owner_epoch=vv_voice_session_owner_epoch(c->session);
 if(c->wake_used){
  struct k7_voice_session_status status;k7_voice_session_status(c->session,&status);
  if(status.asr_epoch)return finish_capture(c,-EBUSY);
  c->epoch=vv_voice_session_next_asr_epoch();
  if(!c->epoch)return finish_capture(c,-EOVERFLOW);
  c->first_sample=c->wake.request.sample_first;c->live=true;
  return 0;
 }
 /* Drain the preceding window before admitting PCM. A failed full old queue
  * must not poison every future window. If RX races this short drain and
  * overflows the provisional epoch, discard it and use another fresh ID;
  * no PCM from such an epoch has been exposed to the recognizer. */
 for(unsigned attempt=0;attempt<3;attempt++){
  uint32_t next=vv_voice_session_next_asr_epoch();if(!next)return finish_capture(c,-EOVERFLOW);
  rc=k7_voice_session_asr_epoch(c->session,next);if(rc)return finish_capture(c,rc);
  c->epoch=next;
  c->live=true;
  for(unsigned discarded=0;discarded<=K7_VOICE_RING_SLOTS;discarded++){
   struct k7_voice_frame stale;rc=k7_voice_session_take_asr(c->session,c->epoch,&stale);
   if(rc==-EPIPE)continue; /* library discarded one old full queue */
   if(rc<=0)break;
  }
  if(rc==-ENOSPC)continue;
  if(rc)return finish_capture(c,rc<0?rc:-EOVERFLOW);
  struct k7_voice_session_status s;k7_voice_session_status(c->session,&s);
  c->first_sample=s.microphone_samples;
  rc=session_health(c);if(!rc)return 0;
  if(rc!=-ENOSPC)return finish_capture(c,rc);
 }
 return finish_capture(c,-ENOSPC);
}
static int take(void*a,uint8_t pcm[640]){
 struct capture*c=a;if(c->done)return c->result?c->result:0;
 if(!c->live)return -EINVAL;
 int rc=session_health(c);if(rc)return finish_capture(c,rc);
 for(unsigned skipped=0;skipped<K7_VOICE_RING_SLOTS;skipped++){
  struct k7_voice_frame f;
  rc=c->wake_used?c->wake.take(c->wake.arg,&c->wake.request,&f):
     k7_voice_session_take_asr(c->session,c->epoch,&f);
  /* An old full queue may have been drained without yielding a fresh frame.
   * ENOSPC is never recoverable inside this utterance. */
  if(rc==-EPIPE&&!c->have_frame&&!c->wake_used)return 0;
  if(rc<0)return finish_capture(c,c->wake_used&&rc==-ENOSPC?VV_ASR_WINDOW_OVERRUN:rc);
  if(!rc)return 0;
  if(f.epoch!=(c->wake_used?c->wake.request.context.input_epoch:c->epoch)||f.samples!=320||f.flags||f.first_sample>UINT64_MAX-320)
   return finish_capture(c,-EPROTO);
  /* Epoch is stamped when a microphone frame is published. Discard a frame
   * that began before task-started, even if its final samples are fresh. */
  if(c->wake_used&&!c->have_frame&&f.first_sample!=c->first_sample)
   return finish_capture(c,-EPIPE);
  if(f.first_sample<c->first_sample)continue;
  if(c->have_frame&&f.first_sample!=c->next_sample)return finish_capture(c,-EPIPE);
  c->have_frame=true;c->next_sample=f.first_sample+320;
  for(unsigned i=0;i<320;i++){uint16_t v=(uint16_t)f.pcm[i];pcm[2*i]=(uint8_t)v;pcm[2*i+1]=(uint8_t)(v>>8);}
  c->received+=320;
  rc=session_health(c);if(rc)return finish_capture(c,rc);
  if(c->received>=c->samples||(c->finish&&c->next_sample>=c->finish_sample)){
   rc=finish_capture(c,0);if(rc)return rc;
  }
  return 1;
 }
 return 0;
}
static int request_finish_capture(void*a){
 struct capture*c=a;if(c->done)return c->result;if(!c->live)return -EINVAL;
 if(c->finish)return 0;
 int rc=session_health(c);if(rc)return finish_capture(c,rc);
 struct k7_voice_session_status s;k7_voice_session_status(c->session,&s);
 if(s.microphone_samples>UINT64_MAX-319)return finish_capture(c,-EOVERFLOW);
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 if(c->wake_used){
  if(s.microphone_samples<c->first_sample)return finish_capture(c,-EPIPE);
  uint64_t relative=s.microphone_samples-c->first_sample;
  uint64_t padding=(320-relative%320)%320;
  if(s.microphone_samples>UINT64_MAX-padding)return finish_capture(c,-EOVERFLOW);
  c->finish=true;c->finish_sample=s.microphone_samples+padding;
 }else {c->finish=true;c->finish_sample=((s.microphone_samples+319)/320)*320;}
#else
 c->finish=true;c->finish_sample=((s.microphone_samples+319)/320)*320;
#endif
 /* Drain exactly through this whole-frame boundary. RX remains live and a
  * later frame is never replayed into this or the next ASR window. */
 if(c->have_frame&&c->next_sample>=c->finish_sample)return finish_capture(c,0);
 return 0;
}
static int done_capture(void*a){struct capture*c=a;
 if(!c->done&&c->live){int rc=session_health(c);if(rc)return finish_capture(c,rc);}
 return c->done?(c->result?c->result:1):0;
}
static int join_capture(void*a,bool cancel){struct capture*c=a;
 if(!cancel&&!c->done)return -EBUSY;
 int rc=c->live?finish_capture(c,cancel&&c->wake_used?-ECANCELED:0):0;
 if(cancel&&rc==-ECANCELED)rc=0;
 if(!cancel&&!rc&&c->done&&!c->result&&!c->live)c->joined_normal=true;
 return rc;
}
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH
/* Network-owner-only staging. TLS/WS cancellation polling drains the existing
 * source, never another microphone. A bounded32-frame batch keeps cancellation
 * responsive. Total storage is exactly the admitted recording limit. */
static int prefetch_poll(struct capture*c){
 if(!c->prefetch)return 0;
 if(c->done)return c->result;
 for(unsigned i=0;i<32;i++){
  if(c->prefetched>=c->samples/320)return finish_capture(c,-EOVERFLOW);
  int rc=take(c,c->prefetch+(size_t)c->prefetched*640);
  if(rc<0)return rc;
  if(!rc)return 0;
  c->prefetched++;
  if(c->done)return c->result;
 }
 return 0;
}
static int network_start(void*a){struct capture*c=a;return c->prefetch?c->result:start_capture(a);}
static int network_take(void*a,uint8_t pcm[640]){
 struct capture*c=a;if(!c->prefetch)return take(a,pcm);
 int rc=prefetch_poll(c);if(rc)return rc;
 if(c->uploaded==c->prefetched)return 0;
 memcpy(pcm,c->prefetch+(size_t)c->uploaded*640,640);c->uploaded++;return 1;
}
static int network_done(void*a){struct capture*c=a;
 if(!c->prefetch)return done_capture(a);
 int rc=prefetch_poll(c);if(rc)return rc;
 return c->done&&c->uploaded==c->prefetched?1:0;
}
static int network_join(void*a,bool cancel){struct capture*c=a;
 if(c->prefetch&&!cancel&&c->uploaded!=c->prefetched)return -EBUSY;
 return join_capture(a,cancel);
}
#endif

static void publish_capture_identity(const struct capture*c,int rc,
                                     struct vv_asr_batch_result*out){
 memset(&out->capture_identity,0,sizeof(out->capture_identity));
 if(rc||!out->command_finalized||!c->done||c->live||c->result||
    !c->joined_normal||!c->have_frame||!c->received||!c->epoch||!c->owner_epoch||
    c->received%320||c->next_sample<c->received||
    c->next_sample-c->received<c->first_sample)return;
 if(c->cancel&&c->cancel->is_cancelled&&c->cancel->is_cancelled(c->cancel->arg))return;
 if(vv_voice_session_owner_epoch(c->session)!=c->owner_epoch)return;
 /* first_sample in capture is only an admission lower bound. Continuity was
  * checked frame-by-frame; subtraction yields the actual first sent frame. */
 out->capture_identity=(struct vv_asr_capture_identity){.valid=true,
   .owner_epoch=c->owner_epoch,.asr_epoch=c->epoch,
   .first_sample=c->next_sample-c->received,.next_sample=c->next_sample,
   .received_samples=c->received};
}
#else
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) &&     defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
#define K7_PF_WARM_CAPTURE 1
#endif
struct capture {
 struct k7_stream_ring ring;struct k7_command_endpoint endpoint;
 pthread_t thread;atomic_bool done,stop;
#ifdef K7_PF_WARM_CAPTURE
 atomic_bool exited;atomic_uint handoff;int worker_result;
#endif
 const struct vv_https_cancel *cancel;unsigned samples;int result,error;
 bool live,ended;
 uint64_t abs_sum;int64_t signed_sum;unsigned sample_count,peak,clipped;
 atomic_bool admitted,finish;vv_https_now_ms_fn now;void *clock_arg;uint64_t gate_deadline;
};
static int sample(void*a,uint32_t left,uint32_t right){
 struct capture*c=a;
 /* Outer cancellation may take business locks: only the network worker polls it. */
 if(atomic_load(&c->stop))return c->error=-ECANCELED;
 int rc=k7_stream_ring_sample(&c->ring,left,right);if(rc)return c->error=rc;
 int32_t value=(int16_t)(left>>16);unsigned magnitude=(unsigned)(value<0?-value:value);
 c->abs_sum+=magnitude;c->signed_sum+=value;c->sample_count++;
 if(value==32767||value==-32768)c->clipped++;
 if(magnitude>c->peak)c->peak=magnitude;
 k7_endpoint_sample(&c->endpoint,(int16_t)value);
 if(!c->ring.partial_samples){
  int endpoint=k7_endpoint_frame(&c->endpoint);
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
  (void)endpoint; /* Keep measurements; only a completed command ends early. */
  if(atomic_load_explicit(&c->finish,memory_order_acquire)){c->ended=true;return 1;}
#else
  if(endpoint){c->ended=true;return 1;}
#endif
 }
 return 0;
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP
static int prepared_gate(void*a){
 struct capture*c=a;
 for(;;){
  /* Outer cancellation may take business locks; the network worker polls it. */
 if(atomic_load(&c->stop))return c->error=-ECANCELED;
  if(c->now(c->clock_arg)>=c->gate_deadline)return c->error=-ETIMEDOUT;
  if(atomic_load_explicit(&c->admitted,memory_order_acquire))return 0;
  sleep_ms(NULL,1);
 }
}
#endif
static int capture_complete(struct capture*c,int rc){
 c->result=c->error?c->error:rc;
 if(!c->result&&(c->ring.partial_samples||!c->ring.produced||
   c->ring.produced>c->samples/320||(!c->ended&&c->ring.produced!=c->samples/320)))c->result=-EIO;
 printf("PARAFORMER endpoint frames=%u voiced=%u quiet=%u last_active=%u ended=%u rc=%d\n",
  c->endpoint.frames,c->endpoint.voiced,c->endpoint.quiet,c->endpoint.last_active,(unsigned)c->ended,c->result);fflush(stdout);
 printf("PARAFORMER audio samples=%u abs_mean=%u peak=%u\n",c->sample_count,
  c->sample_count?(unsigned)(c->abs_sum/c->sample_count):0,c->peak);fflush(stdout);
 printf("PARAFORMER audio_dc signed_mean=%d clipped=%u\n",
  c->sample_count?(int)(c->signed_sum/c->sample_count):0,c->clipped);fflush(stdout);
 int result=c->result;
 atomic_store_explicit(&c->done,true,memory_order_release);return result;
}
#ifdef K7_PF_WARM_CAPTURE
/* Handoff owns every cross-window reset. The capture worker never releases
 * owner to another thread while ADC bias/clocks remain prepared. */
enum warm_handoff { WARM_RUN, WARM_WAIT, WARM_KEEP, WARM_CLAIMED, WARM_NEXT, WARM_CLOSE };
static pthread_mutex_t warm_lock=PTHREAD_MUTEX_INITIALIZER;
static struct capture *warm_capture;
static uint64_t idle_now(void){struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000)return UINT64_MAX;
 if((uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static int capture_cancelled(void*a){return atomic_load(&((struct capture*)a)->stop);}
static int next_capture(void*a){
 struct capture*c=a;uint64_t began=idle_now(),idle=UINT64_MAX;
 atomic_store_explicit(&c->handoff,WARM_WAIT,memory_order_release);
 int completed=capture_complete(c,0);if(completed)return completed;
 for(;;){
  if(atomic_load(&c->stop))return -ECANCELED;
  unsigned state=atomic_load_explicit(&c->handoff,memory_order_acquire);
  if(state==WARM_CLOSE)return 0;
  if(state==WARM_NEXT){atomic_store_explicit(&c->handoff,WARM_RUN,memory_order_release);return 1;}
  uint64_t now=idle_now();
  if(state==WARM_KEEP&&idle==UINT64_MAX)idle=now;
  bool expired=now==UINT64_MAX||began==UINT64_MAX||now<began||
    (state==WARM_WAIT&&now-began>=30000)||
    (state==WARM_KEEP&&(now<idle||now-idle>=500));
  /* CLAIMED protects the nonblocking ring reset from idle-timeout cleanup. */
  if(expired&&state!=WARM_CLAIMED){
   unsigned expected=state;
   if(atomic_compare_exchange_strong(&c->handoff,&expected,WARM_CLOSE))
    return state==WARM_KEEP?0:-ETIMEDOUT;
  }
  sleep_ms(NULL,1);
 }
}
#endif
static void *record(void*a){
 struct capture*c=a;
#ifdef K7_PF_WARM_CAPTURE
 int rc=k7sound_capture_stream_session(c->samples,sample,c,prepared_gate,c,
                                        next_capture,c,capture_cancelled,c);
 c->worker_result=rc;
 if(!atomic_load_explicit(&c->done,memory_order_acquire))capture_complete(c,rc);
 atomic_store_explicit(&c->exited,true,memory_order_release);
#else
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP
 int rc=k7sound_capture_stream_prepared(c->samples,sample,c,prepared_gate,c);
#else
 int rc=k7sound_capture_stream_cued(c->samples,sample,c);
#endif
 capture_complete(c,rc);
#endif
 return NULL;
}
static int prepare_capture(void*a){
 struct capture*c=a;
#ifdef K7_PF_WARM_CAPTURE
 if(c->live)return 0;
#endif
 pthread_attr_t attr;int err=pthread_attr_init(&attr);if(err)return -err;
 err=pthread_attr_setstacksize(&attr,32768);
#ifdef CONFIG_SMP
 if(!err){cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(1,&cpus);err=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);}
#endif
 if(!err)err=pthread_create(&c->thread,&attr,record,c);
 pthread_attr_destroy(&attr);if(!err)c->live=true;return -err;
}
static int start_capture(void*a){
 struct capture*c=a;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP
 if(!c->live)return -EIO;
 if(atomic_load_explicit(&c->done,memory_order_acquire))return c->result?c->result:-EIO;
 atomic_store_explicit(&c->admitted,true,memory_order_release);return 0;
#else
 return prepare_capture(c);
#endif
}
static int take(void*a,uint8_t pcm[640]){struct capture*c=a;uint32_t seq;return k7_stream_ring_take(&c->ring,pcm,&seq);}
static int request_finish_capture(void*a){struct capture*c=a;atomic_store_explicit(&c->finish,true,memory_order_release);return 0;}
static int done_capture(void*a){struct capture*c=a;return atomic_load_explicit(&c->done,memory_order_acquire)?(c->result?c->result:1):0;}
static int join_capture(void*a,bool cancel){
 struct capture*c=a;if(!c->live)return 0;
#ifdef K7_PF_WARM_CAPTURE
 if(!cancel){
  while(!atomic_load_explicit(&c->done,memory_order_acquire))sleep_ms(NULL,1);
  return c->result;
 }
 atomic_store(&c->stop,true);
 atomic_store_explicit(&c->handoff,WARM_CLOSE,memory_order_release);
#else
 if(cancel)atomic_store(&c->stop,true);
#endif
 int err=pthread_join(c->thread,NULL);
 if(err){
#ifdef K7_PF_WARM_CAPTURE
  while(!atomic_load_explicit(&c->exited,memory_order_acquire))sleep_ms(NULL,1);
#else
  while(!atomic_load_explicit(&c->done,memory_order_acquire))sleep_ms(NULL,1);
#endif
  (void)pthread_detach(c->thread);
 }
 c->live=false;if(err)return -err;
#ifdef K7_PF_WARM_CAPTURE
 return c->worker_result==-ECANCELED?0:c->worker_result;
#else
 return cancel&&c->result==-ECANCELED?0:c->result;
#endif
}
#ifdef K7_PF_WARM_CAPTURE
static int close_warm_locked(void){
 struct capture*c=warm_capture;if(!c)return 0;
 int rc=join_capture(c,true);warm_capture=NULL;
 vv_https_secure_zero(c,sizeof(*c));free(c);
 printf("PARAFORMER capture_session_closed rc=%d\n",rc);fflush(stdout);
 return rc;
}
#endif
#endif /* CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION */
int vv_paraformer_k7_close(void){
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 return vv_paraformer_k7_wake_disarm();
#elif defined(K7_PF_WARM_CAPTURE)
 if(pthread_mutex_trylock(&warm_lock))return -EBUSY;
 int rc=close_warm_locked();pthread_mutex_unlock(&warm_lock);return rc;
#else
 return 0;
#endif
}
struct transport {struct vv_https_mbedtls*tls;bool live;};
static ssize_t read_io(void*a,void*p,size_t n){struct transport*t=a;short wanted=0;ssize_t rc=vv_https_mbedtls_try_read(t->tls,p,n,&wanted);return rc==-EAGAIN?VV_WSI_BLOCK:rc;}
static ssize_t write_io(void*a,const void*p,size_t n){struct transport*t=a;short wanted=0;ssize_t rc=vv_https_mbedtls_try_write(t->tls,p,n,&wanted);return rc==-EAGAIN?VV_WSI_BLOCK:rc;}
static int random_io(void*a,uint8_t*p,size_t n){return vv_https_mbedtls_random(((struct transport*)a)->tls,p,n);}
static void close_io(void*a){struct transport*t=a;if(t->live){t->live=false;vv_https_mbedtls_ops()->close(t->tls);}}
static int network_listen(struct vv_https_mbedtls*tls,const char*host,const char*key,vv_https_now_ms_fn now,void*clock_arg,unsigned samples,const struct vv_https_cancel*cancel,struct vv_asr_batch_result*out){
 if(out)memset(out,0,sizeof(*out));
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
 const unsigned max_samples=160000;
#else
 const unsigned max_samples=48000;
#endif
 if(!tls||!key||!now||!out||samples<320||samples>max_samples||samples%320||strlen(key)>2048)return -EINVAL;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg)){
  int cleanup_rc=vv_paraformer_k7_close();return cleanup_rc?cleanup_rc:-ECANCELED;
 }
 struct capture*c=NULL;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 if(pthread_mutex_trylock(&session_capture_lock))return -EBUSY;
#endif
#ifdef K7_PF_WARM_CAPTURE
 if(pthread_mutex_trylock(&warm_lock))return -EBUSY;
 bool reused=false;
 if(warm_capture){
  unsigned expected=WARM_KEEP;
  if(warm_capture->samples==samples&&
     !atomic_load_explicit(&warm_capture->exited,memory_order_acquire)&&
     atomic_compare_exchange_strong(&warm_capture->handoff,&expected,WARM_CLAIMED)){
   c=warm_capture;reused=true;
  }else{
   int closed=close_warm_locked();
   if(closed){pthread_mutex_unlock(&warm_lock);return closed;}
  }
 }
#endif
 if(!c)c=calloc(1,sizeof(*c));
 if(!c){
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
  pthread_mutex_unlock(&session_capture_lock);
#endif
#ifdef K7_PF_WARM_CAPTURE
  pthread_mutex_unlock(&warm_lock);
#endif
  return -ENOMEM;
 }
 c->samples=samples;c->cancel=cancel;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 if(wake_armed){c->wake=wake_pending;c->wake_used=true;
  memset(&wake_pending,0,sizeof(wake_pending));wake_armed=false;}
#endif
#ifndef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 c->result=c->error=0;c->ended=false;
 c->abs_sum=0;c->signed_sum=0;c->sample_count=c->peak=c->clipped=0;
 memset(&c->endpoint,0,sizeof(c->endpoint));
 atomic_store(&c->done,false);atomic_store(&c->stop,false);atomic_store(&c->finish,false);
 k7_stream_ring_init(&c->ring);
#endif
#ifdef K7_PF_WARM_CAPTURE
 if(!reused){atomic_init(&c->exited,false);atomic_init(&c->handoff,WARM_RUN);warm_capture=c;}
#endif
 const struct vv_https_cancel *operation_cancel=cancel;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 struct vv_https_cancel wake_token={wake_cancelled,c};
 if(c->wake_used)operation_cancel=&wake_token;
#endif
 struct transport t={tls,false};char authorization[2056];snprintf(authorization,sizeof(authorization),"Bearer %s",key);
 struct vv_wss_client handshake={.io=vv_https_mbedtls_ops(),.io_arg=tls,.now=now,.now_arg=clock_arg,
  .random=vv_https_mbedtls_random,.random_arg=tls,.sha1=vv_https_mbedtls_sha1,.max_message=VV_PF_RX_CAP};
 uint64_t began=now(clock_arg);int rc=-ETIMEDOUT;
 atomic_store(&c->admitted,false);c->now=now;c->clock_arg=clock_arg;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP
 if(began>UINT64_MAX-30000)goto cleanup;
 c->gate_deadline=began+30000;
#ifdef K7_PF_WARM_CAPTURE
 if(reused)atomic_store_explicit(&c->handoff,WARM_NEXT,memory_order_release);
 printf("PARAFORMER capture_session_reused=%u\n",(unsigned)reused);fflush(stdout);
#endif
 rc=prepare_capture(c);if(rc)goto cleanup;
#endif
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 if(c->wake_used){
  c->prefetch=calloc(c->samples,2);if(!c->prefetch){rc=-ENOMEM;goto cleanup;}
  rc=start_capture(c);if(rc)goto cleanup;
  rc=prefetch_poll(c);if(rc)goto cleanup;
 }
#endif
 if(began<=UINT64_MAX-15000)rc=vv_wss_open(&handshake,host,443,"/api-ws/v1/inference",authorization,began+15000,operation_cancel);
 vv_https_secure_zero(authorization,sizeof(authorization));
 if(!rc){
  t.live=true;handshake.open=false;
  struct vv_pf_run_config cfg={.io={read_io,write_io,random_io,close_io,NULL,&t},
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
    .capture={network_start,network_take,network_done,network_join,c,request_finish_capture},.now=now,.clock_arg=clock_arg,.sleep_ms=sleep_ms};
#else
    .capture={start_capture,take,done_capture,join_capture,c,request_finish_capture},.now=now,.clock_arg=clock_arg,.sleep_ms=sleep_ms};
#endif
  rc=vv_pf_run(&cfg,operation_cancel,out);
 }
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP
cleanup:
#endif
 /* Covers failures before coordinator takes capture ownership, including DNS,
  * TLS/upgrade, allocation/run-task/ready failures and cancellation. */
 vv_https_secure_zero(authorization,sizeof(authorization));
 close_io(&t);
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 /* A local staging overrun/expiry can cancel a slow TLS handshake. Keep its
  * exact provenance; user cancellation and TLS trust/auth errors still win. */
 if(c->wake_error&&(rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&
    !(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg)))rc=c->wake_error;
#endif
#ifdef K7_PF_WARM_CAPTURE
 bool kept=false;
 if(!rc&&out->command_finalized&&!out->command_intent&&!c->result&&
    atomic_load_explicit(&c->done,memory_order_acquire)&&
    !atomic_load_explicit(&c->exited,memory_order_acquire)&&!atomic_load(&c->stop)){
  unsigned expected=WARM_WAIT;
  kept=atomic_compare_exchange_strong(&c->handoff,&expected,WARM_KEEP);
 }
 if(!kept){int cleanup_rc=close_warm_locked();if(cleanup_rc)rc=cleanup_rc;}
 pthread_mutex_unlock(&warm_lock);
#else
 if(c->live){int cleanup_rc=join_capture(c,true);if(cleanup_rc)rc=cleanup_rc;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 if(c->wake_used&&!c->wake_released){
  int cleanup_rc=finish_capture(c,rc?rc:-ECANCELED);
  if(!rc&&cleanup_rc)rc=cleanup_rc;
 }
 publish_capture_identity(c,rc,out);
#endif
#if defined(K7_PF_WAKE_PREFETCH) && K7_PF_WAKE_PREFETCH && defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
 if(c->prefetch){vv_https_secure_zero(c->prefetch,(size_t)c->samples*2);free(c->prefetch);}
#endif
 vv_https_secure_zero(c,sizeof(*c));free(c);
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
 pthread_mutex_unlock(&session_capture_lock);
#endif
#endif
 return rc;
}

struct network_job {
 struct vv_https_mbedtls *tls;const char *key;vv_https_now_ms_fn now;void *clock_arg;
 unsigned samples;const struct vv_https_cancel *cancel;struct vv_asr_batch_result *out;
 atomic_bool done;int result;char host[129];
};
static void *network_worker(void*a){struct network_job*j=a;
 j->result=network_listen(j->tls,j->host,j->key,j->now,j->clock_arg,j->samples,j->cancel,j->out);
 atomic_store_explicit(&j->done,true,memory_order_release);return NULL;
}
int vv_paraformer_k7_listen(struct vv_https_mbedtls*tls,const char*key,vv_https_now_ms_fn now,void*clock_arg,unsigned samples,const struct vv_https_cancel*cancel,struct vv_asr_batch_result*out){
 return vv_paraformer_k7_listen_at_host(tls,VV_ALIYUN_LEGACY_HOST,key,now,clock_arg,samples,cancel,out);
}
int vv_paraformer_k7_listen_at_host(struct vv_https_mbedtls*tls,const char*host,const char*key,vv_https_now_ms_fn now,void*clock_arg,unsigned samples,const struct vv_https_cancel*cancel,struct vv_asr_batch_result*out){
 if(out)memset(out,0,sizeof(*out));
 if(!vv_aliyun_speech_host_valid(host))return -EINVAL;
 struct network_job j={.tls=tls,.key=key,.now=now,.clock_arg=clock_arg,.samples=samples,.cancel=cancel,.out=out};
 memcpy(j.host,host,strlen(host)+1);
 atomic_init(&j.done,false);if(out)memset(out,0,sizeof(*out));
 pthread_attr_t attr;pthread_t thread;int err=pthread_attr_init(&attr);if(err)return -err;
 err=pthread_attr_setstacksize(&attr,65536);
#ifdef CONFIG_SMP
 if(!err){cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(0,&cpus);err=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);}
#endif
 if(!err)err=pthread_create(&thread,&attr,network_worker,&j);
 pthread_attr_destroy(&attr);if(err)return -err;
 err=pthread_join(thread,NULL);
 if(err){while(!atomic_load_explicit(&j.done,memory_order_acquire))sleep_ms(NULL,1);(void)pthread_detach(thread);if(out)memset(out,0,sizeof(*out));return -err;}
 return j.result;
}
