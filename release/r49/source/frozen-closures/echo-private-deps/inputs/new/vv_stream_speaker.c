#include "vv_stream_speaker.h"
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "echo_diag_runtime.h"
#endif
#include "vv_reply_scope.h"
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include "k7_echo_reply_identity.h"
#endif

#include "pcm_playback.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_SMP
#include <sched.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_voice_session.h"
#include "voice_session.h"
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
#include "session_capture.h"
#endif
#define TOTAL_MS 90000u
#define CLEANUP_MS 1000u
#define MAX_SAMPLES 480000u
#define DIALOGUE_MAX_SAMPLES 1440000u
#define SESSION_PREBUFFER 8192u
#define SESSION_REBUFFER 16000u
#define SESSION_REFILL_MS 5000u
#define SESSION_VERIFIED_REFILL_MS 20000u
/* No playback worker or second hardware owner. The one existing network
 * worker writes bounded chunks to the manager's generation-tagged TX queue. */
struct speaker {
 struct k7_voice_session *session;atomic_uint generation;
 atomic_bool stop,eof,net_done;atomic_int play_error;
 int net_rc;unsigned produced;uint64_t began;
 bool trace_eligible;
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
 bool capture_active;
#endif
 vv_stream_pcm_fetch fetch;vv_stream_pcm_fetch_ex fetch_ex;
 void *arg;const struct vv_https_cancel *outer;
};
static pthread_mutex_t session_speaker_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t ms(void){struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000)return UINT64_MAX;
 if((uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static void pause_ms(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static int playback_error(struct speaker*s){
 uint32_t g=atomic_load_explicit(&s->generation,memory_order_acquire);if(!g)return 0;
 struct k7_voice_tx_result result;int rc=k7_voice_session_tx_result(s->session,g,&result);
 if(!rc){
  rc=result.result;
  if(!rc&&(!atomic_load_explicit(&s->eof,memory_order_acquire)||result.discarded_words))rc=-EPIPE;
 }else if(rc==-EAGAIN){
  struct k7_voice_session_status status;k7_voice_session_status(s->session,&status);
  rc=status.finished||!status.running||!status.ready||status.stop_requested?(status.result?status.result:-ENOTCONN):0;
 }
 if(rc){int expected=0;(void)atomic_compare_exchange_strong(&s->play_error,&expected,rc);atomic_store(&s->stop,true);}
 return rc;
}
/* Polling TLS sees only atomics/audio snapshots, never a business callback. */
static bool stopped(void*a){struct speaker*s=a;(void)playback_error(s);return atomic_load(&s->stop);}
static int sink(void*a,const unsigned char*data,size_t bytes){
 struct speaker*s=a;if(!data||bytes%2)return -EINVAL;
 if(bytes/2>MAX_SAMPLES-s->produced)return -EOVERFLOW;
 uint32_t g=atomic_load(&s->generation);int16_t pcm[320];
 for(size_t at=0;at<bytes;){
  unsigned count=(unsigned)((bytes-at)/2);if(count>320)count=320;
  for(unsigned i=0;i<count;i++)pcm[i]=(int16_t)((uint16_t)data[at+2*i]|((uint16_t)data[at+2*i+1]<<8));
  for(;;){
   if(stopped(s))return -ECANCELED;
   uint64_t now=ms();if(now<s->began||now-s->began>=TOTAL_MS)return -ETIMEDOUT;
   int rc=k7_voice_session_tx_write(s->session,g,pcm,count);
   if(!rc)break;
   /* Queue pressure admits exactly the same unconsumed chunk later. Refill
    * pauses remain in this generation; a completed -EAGAIN is still failure. */
   if(rc!=-EAGAIN)return rc;
   pause_ms();
  }
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  /* Record each accepted source range once, never an EAGAIN retry. */
  if(s->capture_active)k7_session_capture_source(s->session,g,pcm,count,s->produced);
#endif
  s->produced+=count;at+=(size_t)count*2;
 }
 return 0;
}
static int finish_tx(struct speaker*s,int rc){
 uint32_t g=atomic_load(&s->generation);if(!g)return rc;
 bool cancelling=rc||atomic_load(&s->stop);bool cancel_sent=false;
 uint64_t cleanup_began=0;
 if(!cancelling){
  atomic_store_explicit(&s->eof,true,memory_order_release);
  int end=k7_voice_session_tx_end(s->session,g);if(end){rc=end;cancelling=true;}
 }
 for(;;){
  if(atomic_load(&s->stop))cancelling=true;
  if(cancelling&&!cancel_sent){
   /* Even if completion won the race, only this generation is addressed. */
   (void)k7_voice_session_tx_cancel(s->session,g);cancel_sent=true;cleanup_began=ms();
  }
  struct k7_voice_tx_result result;int got=k7_voice_session_tx_result(s->session,g,&result);
  if(!got){
   int play=atomic_load(&s->play_error);
   if(!rc||rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED){
    if(play)rc=play;else if(result.result)rc=result.result;
    else if(result.discarded_words||result.queued_samples!=s->produced)rc=-EPIPE;
   }
   return rc?rc:(cancelling?-ECANCELED:0);
  }
  if(got!=-EAGAIN)return rc?rc:got;
  uint64_t now=ms();
  if(cancelling&&(now<cleanup_began||now-cleanup_began>=CLEANUP_MS))return -ETIMEDOUT;
  if(!cancelling&&(now<s->began||now-s->began>=TOTAL_MS)){rc=-ETIMEDOUT;cancelling=true;}
  pause_ms();
 }
}
static void *network_worker(void*a){
 struct speaker*s=a;uint32_t g=vv_voice_session_next_tx_generation();
 int rc=g?k7_voice_session_tx_begin_stream(s->session,g,SESSION_PREBUFFER,
                                          SESSION_REBUFFER,SESSION_REFILL_MS):-EOVERFLOW;
 if(!rc){
  atomic_store_explicit(&s->generation,g,memory_order_release);
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  /* The armed diagnostic owns its memory and must never fail the reply. */
  if(s->trace_eligible)s->capture_active=k7_session_capture_begin(s->session,g)==1;
#endif
  struct vv_https_cancel token={stopped,s};struct vv_stream_fetch_result result={0};
  rc=s->fetch_ex?s->fetch_ex(s->arg,sink,s,&token,&result):s->fetch(s->arg,sink,s,&token);
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  int fetch_rc=rc;
#endif
  if(!rc&&!s->produced)rc=-EPROTO;
  bool complete_tail=s->fetch_ex&&rc==VV_HTTPS_EPROTO&&result.http_result==rc&&
   result.terminal_audio_complete&&s->produced&&result.pcm_bytes==(size_t)s->produced*2;
  if(complete_tail){
   int original=rc,blocked=0;
   if(stopped(s)){blocked=atomic_load(&s->play_error);if(!blocked)blocked=-ECANCELED;}
   int drain=finish_tx(s,blocked);
   rc=drain?drain:original;
   printf("TTS_TERMINAL_DRAIN generation=%u http_rc=%d drain_rc=%d samples=%u result=%d\n",g,original,drain,s->produced,rc);fflush(stdout);
  }else{
   if(rc)atomic_store(&s->stop,true);
   rc=finish_tx(s,rc);
  }
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  /* Nonblocking publication after drain/cancel handling. A cleanup error is
   * retained as stream_rc; it must not be labelled a complete audio trace. */
  if(s->capture_active)k7_session_capture_end(s->session,g,fetch_rc,rc);
#endif
 }
 s->net_rc=rc;atomic_store_explicit(&s->net_done,true,memory_order_release);return NULL;
}
static int launch(pthread_t*t,void *(*entry)(void*),void*arg){
 pthread_attr_t a;int e=pthread_attr_init(&a);if(e)return -e;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_OPUS
 e=pthread_attr_setstacksize(&a,131072);
#else
 e=pthread_attr_setstacksize(&a,65536);
#endif
#ifdef CONFIG_SMP
 if(!e){cpu_set_t set;CPU_ZERO(&set);
#if defined(K7_CPU_ROLE_SPLIT) && CONFIG_SMP_NCPUS > 7
  /* Fetch includes synchronous Opus decoding. Keep it on A72 with JPEG,
   * away from the CPU3 control clock; audio PIO remains on CPU1. */
  CPU_SET(5,&set);
#elif CONFIG_SMP_NCPUS > 3
  CPU_SET(3,&set);
#else
  CPU_SET(0,&set);
#endif
  e=pthread_attr_setaffinity_np(&a,sizeof(set),&set);}
#endif
 if(!e)e=pthread_create(t,&a,entry,arg);
 pthread_attr_destroy(&a);return -e;
}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
/* TX is already terminal and its producer joined. Even an already-canceled
 * business token must finish physical retirement before another prompt starts;
 * this bounded cleanup does not change the caller's original cancel/error. */
static int prompt_finish(struct k7_voice_session *session,uint32_t generation){
 uint64_t owner=vv_voice_session_owner_epoch(session),began=ms();
 if(!owner)return -ESTALE;
 if(began==UINT64_MAX)return -ETIMEDOUT;
 for(unsigned i=0;i<=1000;i++){
  uint64_t now=ms();if(now<began||now-began>=1000)return -ETIMEDOUT;
  int rc=k7_echo_runtime_prompt_finish_owner(owner,generation);if(rc!=-EAGAIN)return rc;
  pause_ms();
 }
 return -ETIMEDOUT;
}
#endif
static int session_run(vv_stream_pcm_fetch fetch,vv_stream_pcm_fetch_ex fetch_ex,void*arg,const struct vv_https_cancel*cancel,bool trace_eligible){
 if(!fetch&&!fetch_ex)return -EINVAL;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 if(pthread_mutex_trylock(&session_speaker_lock))return -EBUSY;
 struct k7_voice_session*session=vv_voice_session_audio();
 if(!session){pthread_mutex_unlock(&session_speaker_lock);return -ENOTCONN;}
 struct speaker*s=calloc(1,sizeof(*s));if(!s){pthread_mutex_unlock(&session_speaker_lock);return -ENOMEM;}
 s->session=session;s->fetch=fetch;s->fetch_ex=fetch_ex;s->arg=arg;s->outer=cancel;s->began=ms();s->trace_eligible=trace_eligible;
 atomic_init(&s->generation,0);atomic_init(&s->stop,false);atomic_init(&s->eof,false);
 atomic_init(&s->net_done,false);atomic_init(&s->play_error,0);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 bool network_joined=false;
#endif
 pthread_t thread;int rc=launch(&thread,network_worker,s);
 if(!rc){
  int coordination=0;
  while(!atomic_load_explicit(&s->net_done,memory_order_acquire)){
   if(!coordination){
    if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))coordination=-ECANCELED;
    uint64_t now=ms();if(!coordination&&(now<s->began||now-s->began>=TOTAL_MS))coordination=-ETIMEDOUT;
    if(coordination)atomic_store(&s->stop,true);
   }
   (void)playback_error(s);pause_ms();
  }
  int e=pthread_join(thread,NULL);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(e){(void)pthread_detach(thread);rc=-e;}else {network_joined=true;rc=s->net_rc;}
#else
  if(e){(void)pthread_detach(thread);rc=-e;}else rc=s->net_rc;
#endif
  if(coordination&&(!rc||rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED))rc=coordination;
 }
 struct k7_voice_tx_result finished={0};unsigned generation=atomic_load(&s->generation);
 int result_rc=generation?k7_voice_session_tx_result(session,generation,&finished):-ENODATA;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(network_joined&&generation&&!result_rc&&(!finished.result||finished.result==-ECANCELED)){
  int retirement=prompt_finish(session,generation);if(retirement)rc=retirement;
  else if(!rc&&cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))rc=-ECANCELED;
 }
#endif
 printf("TTS_SESSION generation=%u rc=%d samples=%u produced=%u queued=%u discarded_words=%u tx_result=%d result_rc=%d pauses=%u total_ms=%llu\n",generation,rc,s->produced,s->produced,finished.queued_samples,finished.discarded_words,finished.result,result_rc,finished.pauses,(unsigned long long)(ms()-s->began));fflush(stdout);
 vv_https_secure_zero(s,sizeof(*s));free(s);pthread_mutex_unlock(&session_speaker_lock);return rc;
}
int vv_stream_speaker_run(vv_stream_pcm_fetch fetch,void*arg,const struct vv_https_cancel*cancel){
 return session_run(fetch,NULL,arg,cancel,false);
}
int vv_stream_speaker_run_real_tts(vv_stream_pcm_fetch fetch,void*arg,const struct vv_https_cancel*cancel){
 return session_run(fetch,NULL,arg,cancel,true);
}
/* The legacy natural entry keeps its append-only buffer. Dialogue streams
 * reuse the same allocation as a bounded SPSC ring. Only their producer may
 * wait for capacity; the caller always feeds TX and polls the business guard. */
#define SPOOL_PAUSE_OBSERVATIONS 8u
struct spool_pause_observation {
 unsigned index,state,received,submitted;
 bool net_done,resumed;
 uint64_t at,resume_at;
 unsigned resume_received,resume_submitted;
};
struct spooled_speaker {
 struct speaker play;
 unsigned char *pcm;
 bool dialogue,scoped;
 struct vv_reply_scope_token lease;
 atomic_uint received,submitted;
 atomic_int sink_error;
 struct vv_stream_fetch_result fetched;bool fetch_invoked;
 int fetch_rc;
 /* Receive-worker-only fields; read only after join. No callback logging or
  * per-block allocation. Dialogue's capacity wait remains cancellable. */
 uint64_t first_received,last_received,max_receive_gap,network_finished;
 unsigned receive_blocks,max_unsubmitted;
 /* Coordinator observations are not physical PIO timestamps. The public
  * session status has no live FIFO counter; do not invent one from submitted.
  * Keep a bounded set, and report skipped/overflowed observations explicitly. */
 struct spool_pause_observation pauses[SPOOL_PAUSE_OBSERVATIONS];
 unsigned pause_records,pause_count_seen,pause_observations_lost;
 uint64_t first_running,tx_finished;
};
static uint64_t spool_elapsed(const struct spooled_speaker *s,uint64_t at){
 return !at||at==UINT64_MAX||at<s->play.began?0:at-s->play.began;
}
static void spool_observe_tx(struct spooled_speaker *s,uint64_t now,unsigned received,bool net_done){
 struct speaker *p=&s->play;uint32_t g=atomic_load(&p->generation);
 if(!g)return;
 struct k7_voice_session_status status;k7_voice_session_status(p->session,&status);
 if(status.tx_generation!=g)return;
 if(status.tx_state==K7_VOICE_TX_RUNNING&&!s->first_running)s->first_running=now;
 if(status.tx_pauses>s->pause_count_seen){
  /* A coordinator can miss a whole short pause/resume while descheduled. */
  s->pause_observations_lost+=status.tx_pauses-s->pause_count_seen-1;
  s->pause_count_seen=status.tx_pauses;
  if(s->pause_records<SPOOL_PAUSE_OBSERVATIONS){
   struct spool_pause_observation *o=&s->pauses[s->pause_records++];
   *o=(struct spool_pause_observation){.index=status.tx_pauses,.state=status.tx_state,
    .received=received,.submitted=p->produced,.net_done=net_done,.at=now};
  }else ++s->pause_observations_lost;
 }
 if(s->pause_records&&status.tx_state==K7_VOICE_TX_RUNNING){
  struct spool_pause_observation *o=&s->pauses[s->pause_records-1];
  /* RUNNING at the first observation cannot establish a resume boundary. */
  if(o->index==status.tx_pauses&&o->state==K7_VOICE_TX_PAUSED&&!o->resumed){
   o->resumed=true;o->resume_at=now;o->resume_received=received;o->resume_submitted=p->produced;
  }
 }
}
static void spool_diagnostics(const struct spooled_speaker *s,unsigned received){
 const struct speaker *p=&s->play;unsigned g=atomic_load(&p->generation);
 uint64_t tail=s->last_received&&s->last_received!=UINT64_MAX&&s->network_finished!=UINT64_MAX&&s->network_finished>=s->last_received?
  s->network_finished-s->last_received:0;
 printf("TTS_SPOOL_TIMING generation=%u pcm_blocks=%u first_pcm_ms=%llu last_pcm_ms=%llu max_pcm_gap_ms=%llu max_unsubmitted=%u post_pcm_fetch_ms=%llu first_running_observed_ms=%llu tx_finished_observed_ms=%llu pause_records=%u pause_observations_lost=%u terminal_complete=%u\n",
  g,s->receive_blocks,(unsigned long long)spool_elapsed(s,s->first_received),
  (unsigned long long)spool_elapsed(s,s->last_received),(unsigned long long)s->max_receive_gap,
  s->max_unsubmitted,(unsigned long long)tail,(unsigned long long)spool_elapsed(s,s->first_running),
  (unsigned long long)spool_elapsed(s,s->tx_finished),s->pause_records,s->pause_observations_lost,
  (unsigned)s->fetched.terminal_audio_complete);
 for(unsigned i=0;i<s->pause_records;i++){
  const struct spool_pause_observation *o=&s->pauses[i];
  printf("TTS_SPOOL_PAUSE generation=%u index=%u observed_ms=%llu state=%u received=%u submitted=%u queued=0 queued_valid=0 net_done=%u pcm_after=%u resumed_observed=%u resume_observed_ms=%llu resume_received=%u resume_submitted=%u\n",
   g,o->index,(unsigned long long)spool_elapsed(s,o->at),o->state,o->received,o->submitted,
   (unsigned)o->net_done,received>=o->received?received-o->received:0,(unsigned)o->resumed,
   (unsigned long long)spool_elapsed(s,o->resume_at),o->resume_received,o->resume_submitted);
 }
}
static int receive_pcm(void *arg,const unsigned char *data,size_t bytes){
 struct spooled_speaker *s=arg;struct speaker *p=&s->play;
 int rc=0;unsigned at=atomic_load_explicit(&s->received,memory_order_relaxed);
 if(!data||bytes%2)rc=-EINVAL;
 else if(bytes/2>MAX_SAMPLES-at)rc=-EOVERFLOW;
 else if(stopped(p))rc=-ECANCELED;
 else {uint64_t now=ms();if(now==UINT64_MAX||now<p->began||now-p->began>=TOTAL_MS)rc=-ETIMEDOUT;}
 if(rc){int expected=0;(void)atomic_compare_exchange_strong(&s->sink_error,&expected,rc);return rc;}
 if(bytes){
  memcpy(s->pcm+(size_t)at*2,data,bytes);
  uint64_t now=ms();unsigned end=at+(unsigned)(bytes/2);
  if(!at)s->first_received=now;
  else if(now!=UINT64_MAX&&s->last_received!=UINT64_MAX&&now>=s->last_received){
   uint64_t gap=now-s->last_received;if(gap>s->max_receive_gap)s->max_receive_gap=gap;
  }
  s->last_received=now;++s->receive_blocks;
  unsigned submitted=atomic_load_explicit(&s->submitted,memory_order_acquire);
  if(end>=submitted&&end-submitted>s->max_unsubmitted)s->max_unsubmitted=end-submitted;
  atomic_store_explicit(&s->received,end,memory_order_release);
 }
 return 0;
}
static int receive_dialogue_pcm(void *arg,const unsigned char *data,size_t bytes){
 struct spooled_speaker *s=arg;struct speaker *p=&s->play;
 unsigned at=atomic_load_explicit(&s->received,memory_order_relaxed);int rc=0;
 if(!data||bytes%2)rc=-EINVAL;
 else if(at>DIALOGUE_MAX_SAMPLES||bytes/2>DIALOGUE_MAX_SAMPLES-at)rc=-EOVERFLOW;
 size_t copied=0;
 while(!rc){
  if(stopped(p)){rc=-ECANCELED;break;}
  uint64_t now=ms();
  if(now==UINT64_MAX||now<p->began||now-p->began>=TOTAL_MS){rc=-ETIMEDOUT;break;}
  if(copied==bytes)break;
  /* The coordinator releases a slot only after tx_write copied its samples.
   * Acquire prevents overwriting source bytes still being read into TX. */
  unsigned submitted=atomic_load_explicit(&s->submitted,memory_order_acquire);
  if(submitted>at||at-submitted>MAX_SAMPLES){rc=-EIO;break;}
  unsigned available=MAX_SAMPLES-(at-submitted);
  if(!available){pause_ms();continue;}
  unsigned offset=at%MAX_SAMPLES;
  unsigned count=(unsigned)((bytes-copied)/2);
  if(count>available)count=available;
  if(count>MAX_SAMPLES-offset)count=MAX_SAMPLES-offset;
  memcpy(s->pcm+(size_t)offset*2,data+copied,(size_t)count*2);
  if(!at)s->first_received=now;
  else if(s->last_received!=UINT64_MAX&&now>=s->last_received){
   uint64_t gap=now-s->last_received;if(gap>s->max_receive_gap)s->max_receive_gap=gap;
  }
  s->last_received=now;++s->receive_blocks;at+=count;copied+=(size_t)count*2;
  if(at-submitted>s->max_unsubmitted)s->max_unsubmitted=at-submitted;
  /* Every published span is immutable until the consumer's release above. */
  atomic_store_explicit(&s->received,at,memory_order_release);
 }
 if(rc){int expected=0;(void)atomic_compare_exchange_strong(&s->sink_error,&expected,rc);}
 return rc;
}
static void *receive_worker(void *arg){
 struct spooled_speaker *s=arg;struct speaker *p=&s->play;
 uint32_t g=vv_voice_session_next_tx_generation();
 int rc=stopped(p)?-ECANCELED:!g?-EOVERFLOW:
  k7_voice_session_tx_begin_verified_stream(p->session,g,SESSION_PREBUFFER,SESSION_REBUFFER,
                                          SESSION_VERIFIED_REFILL_MS);
 if(!rc){
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  p->capture_active=k7_session_capture_begin(p->session,g)==1;
#endif
  /* Publication includes capture admission; source/end stay on the feeder. */
  atomic_store_explicit(&p->generation,g,memory_order_release);
  struct vv_https_cancel token={stopped,p};
  if(s->scoped)rc=vv_reply_scope_bind(&s->lease,vv_voice_session_owner_epoch(p->session),g);
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  if(!rc&&s->scoped)(void)k7_echo_reply_bind(s->lease.incarnation,s->lease.serial,
    vv_voice_session_owner_epoch(p->session),k7_voice_session_input_epoch(p->session),g);
#endif

  if(!rc){
   s->fetch_invoked=true;
   rc=p->fetch_ex(p->arg,s->dialogue?receive_dialogue_pcm:receive_pcm,s,&token,&s->fetched);
  }
 }
 s->fetch_rc=rc;p->net_rc=rc;s->network_finished=ms();
 atomic_store_explicit(&p->net_done,true,memory_order_release);return NULL;
}
static int spooled_run(vv_stream_pcm_fetch_ex fetch,void *arg,const struct vv_https_cancel *cancel,
                      bool dialogue,const struct vv_reply_scope_token *lease){
 if(!fetch)return -EINVAL;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 if(pthread_mutex_trylock(&session_speaker_lock))return -EBUSY;
 struct k7_voice_session *session=vv_voice_session_audio();
 if(!session){pthread_mutex_unlock(&session_speaker_lock);return -ENOTCONN;}
 struct spooled_speaker *s=calloc(1,sizeof(*s));
 if(!s){pthread_mutex_unlock(&session_speaker_lock);return -ENOMEM;}
 s->dialogue=dialogue;
 if(lease){s->lease=*lease;s->scoped=true;}
 /* All allocation precedes both tx_begin and the cloud request. */
 s->pcm=malloc((size_t)MAX_SAMPLES*2);
 if(!s->pcm){free(s);pthread_mutex_unlock(&session_speaker_lock);return -ENOMEM;}
 struct speaker *p=&s->play;p->session=session;p->fetch_ex=fetch;p->arg=arg;
 p->outer=cancel;p->began=ms();
 atomic_init(&p->generation,0);atomic_init(&p->stop,false);atomic_init(&p->eof,false);
 atomic_init(&p->net_done,false);atomic_init(&p->play_error,0);
 atomic_init(&s->received,0);atomic_init(&s->submitted,0);atomic_init(&s->sink_error,0);
 bool launched=false,checked=false,admitted=false,tx_done=false,cancel_sent=false,cancel_accepted=false;
 int local_rc=0,terminal_rc=0;uint64_t cleanup_began=0,first_submitted=0;
 struct k7_voice_tx_result finished={0};int result_rc=-ENODATA;pthread_t thread;
 if(p->began==UINT64_MAX)local_rc=-ETIMEDOUT;
 if(!local_rc&&s->scoped)local_rc=vv_reply_scope_speaker_begin(&s->lease);
 bool scope_started=s->scoped&&!local_rc;
 if(!local_rc){local_rc=launch(&thread,receive_worker,s);launched=!local_rc;}
 if(launched)for(;;){
  /* This loop must remain the cancellation coordinator even during drain. */
  if(!local_rc&&cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))local_rc=-ECANCELED;
  uint64_t now=ms();
  if(!local_rc&&(now==UINT64_MAX||now<p->began||now-p->began>=TOTAL_MS))local_rc=-ETIMEDOUT;
  bool net_done=atomic_load_explicit(&p->net_done,memory_order_acquire);
  uint32_t g=atomic_load_explicit(&p->generation,memory_order_acquire);
  unsigned received=atomic_load_explicit(&s->received,memory_order_acquire);
  spool_observe_tx(s,now,received,net_done);
  int sink_rc=atomic_load(&s->sink_error);
  if(sink_rc&&sink_rc!=-ECANCELED&&!local_rc)local_rc=sink_rc;
  if(!tx_done){
   int play=playback_error(p);
   if(play&&!local_rc&&((play!=-ECANCELED&&play!=VV_HTTPS_ECANCELLED)||!cancel_sent))local_rc=play;
  }
  if(net_done&&!checked){
   checked=true;terminal_rc=p->net_rc;
   bool exact=received&&s->fetched.pcm_bytes==(size_t)received*2&&!sink_rc;
   bool success=!terminal_rc&&!s->fetched.http_result&&s->fetched.terminal_audio_complete&&exact;
   bool tail=!dialogue&&terminal_rc==VV_HTTPS_EPROTO&&s->fetched.http_result==terminal_rc&&exact&&
    (s->fetched.terminal_audio_complete||s->fetched.validated_prefix);
   admitted=success||tail;
   if(!terminal_rc&&!success)terminal_rc=-EPROTO;
   if(!admitted)atomic_store(&p->stop,true);
  }
  if(local_rc)atomic_store(&p->stop,true);
  if(g&&!tx_done&&!atomic_load(&p->stop)){
   int16_t pcm[320];
   /* Bound each turn of the coordinator; no wait on TX or network here. */
   for(unsigned batch=0;batch<8&&p->produced<received;batch++){
    unsigned count=received-p->produced;if(count>320)count=320;
    unsigned offset=dialogue?p->produced%MAX_SAMPLES:p->produced;
    if(dialogue&&count>MAX_SAMPLES-offset)count=MAX_SAMPLES-offset;
    const unsigned char *from=s->pcm+(size_t)offset*2;
    for(unsigned i=0;i<count;i++)pcm[i]=(int16_t)((uint16_t)from[2*i]|((uint16_t)from[2*i+1]<<8));
    int rc=k7_voice_session_tx_write(session,g,pcm,count);
    if(rc==-EAGAIN)break;
    if(rc){if(!local_rc)local_rc=rc;atomic_store(&p->stop,true);break;}
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
    if(p->capture_active)k7_session_capture_source(session,g,pcm,count,p->produced);
#endif
    if(!p->produced)first_submitted=ms();
    p->produced+=count;
    atomic_store_explicit(&s->submitted,p->produced,memory_order_release);
   }
   if(net_done&&admitted&&p->produced==received&&!atomic_load(&p->stop)&&!atomic_load(&p->eof)){
    atomic_store_explicit(&p->eof,true,memory_order_release);
    int rc=k7_voice_session_tx_end(session,g);
    if(rc){if(!local_rc)local_rc=rc;atomic_store(&p->stop,true);}
   }
  }
  if(g&&!tx_done){
   if(atomic_load(&p->stop)&&!cancel_sent){
    cancel_accepted=k7_voice_session_tx_cancel(session,g)==0;cancel_sent=true;cleanup_began=now;
   }
   result_rc=k7_voice_session_tx_result(session,g,&finished);
   if(!result_rc){
    tx_done=true;s->tx_finished=ms();
    /* A closed TX ring can race tx_write and first report EPIPE. Preserve the
     * owner's actual failure rather than that secondary queue symptom. */
    if(finished.result&&finished.result!=-ECANCELED&&finished.result!=VV_HTTPS_ECANCELLED)local_rc=finished.result;
    else if(finished.result&&!cancel_accepted)local_rc=finished.result;
    if(!local_rc&&!atomic_load(&p->stop)&&
       (finished.result||!atomic_load(&p->eof)||finished.discarded_words||finished.queued_samples!=p->produced))
     local_rc=finished.result?finished.result:-EPIPE;
    if(local_rc)atomic_store(&p->stop,true);
   }else if(result_rc!=-EAGAIN){
    if(!local_rc)local_rc=result_rc;
    atomic_store(&p->stop,true);tx_done=true;
   }else if(cancel_sent&&(now==UINT64_MAX||now<cleanup_began||now-cleanup_began>=CLEANUP_MS)){
    local_rc=-ETIMEDOUT;tx_done=true;
   }
  }else if(net_done&&!g)tx_done=true;
  if(net_done&&tx_done)break;
  pause_ms();
 }
 bool quarantined=false,network_joined=!launched;
 if(launched){
  /* net_done is the worker's last shared access; join precedes every free. */
  int e=pthread_join(thread,NULL);network_joined=!e;
  if(e&&dialogue){
   /* Never detach a dialogue producer whose join cannot be established.
    * net_done has already ended all borrowed callback access. One retry
    * handles a transient join failure; persistent failure retains the state
    * and speaker lock so neither reuse nor freeing can race that thread. */
   atomic_store(&p->stop,true);pause_ms();
   quarantined=pthread_join(thread,NULL)!=0;network_joined=!quarantined;
   if(!local_rc&&!terminal_rc)local_rc=-e;
   printf("TTS_DIALOGUE_JOIN error=%d quarantined=%u\n",e,(unsigned)quarantined);
  }else if(e){(void)pthread_detach(thread);if(!local_rc)local_rc=-e;}
 }
 int rc=local_rc?local_rc:terminal_rc;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(!s->scoped&&network_joined&&atomic_load(&p->generation)&&!result_rc&&
    (!finished.result||finished.result==-ECANCELED)){
  int retirement=prompt_finish(session,atomic_load(&p->generation));if(retirement)rc=retirement;
  else if(!rc&&cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))rc=-ECANCELED;
 }
#endif
 if(scope_started){
  unsigned generation=atomic_load(&p->generation);
  struct k7_voice_session_status status;k7_voice_session_status(session,&status);
  bool tx_retired=!generation||(!result_rc&&finished.generation==generation);
  bool rx_live=status.running&&status.ready&&!status.finished&&!status.stop_requested&&
    vv_voice_session_owner_epoch(session)==s->lease.owner_epoch;
  /* Outer receive-worker join is not a proof about fetch's child workers.
   * Before fetch invocation the control path itself proves none exist.
   * After invocation, an absent/failed explicit proof must keep quarantine. */
  bool nested_joined=!s->fetch_invoked||
    (s->fetched.retirement_proof_valid&&s->fetched.nested_workers_joined);
  int end=vv_reply_scope_speaker_end(&s->lease,generation,tx_retired,
                                    network_joined&&nested_joined,rx_live);
  if(!rc&&end)rc=end;
 }
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
 if(atomic_load(&p->generation)&&p->capture_active)
  k7_session_capture_end(session,atomic_load(&p->generation),s->fetch_rc,rc);
#endif
 unsigned received=atomic_load(&s->received);
 if(!result_rc&&finished.pauses>s->pause_count_seen)
  s->pause_observations_lost+=finished.pauses-s->pause_count_seen;
 spool_diagnostics(s,received);
 if(dialogue)printf("TTS_DIALOGUE_BUFFER generation=%u capacity_samples=%u limit_samples=%u received=%u reclaimed=%u peak_pending=%u\n",
  atomic_load(&p->generation),MAX_SAMPLES,DIALOGUE_MAX_SAMPLES,received,
  atomic_load(&s->submitted),s->max_unsubmitted);
 printf("TTS_SPOOL generation=%u rc=%d fetch_rc=%d received=%u submitted=%u unsubmitted=%u queued=%u discarded_words=%u tx_result=%d result_rc=%d pauses=%u first_received_ms=%llu first_submitted_ms=%llu net_done_ms=%llu total_ms=%llu\n",
  atomic_load(&p->generation),rc,s->fetch_rc,received,p->produced,received-p->produced,finished.queued_samples,finished.discarded_words,finished.result,result_rc,finished.pauses,
  (unsigned long long)(s->first_received?s->first_received-p->began:0),(unsigned long long)(first_submitted?first_submitted-p->began:0),
  (unsigned long long)(s->network_finished?s->network_finished-p->began:0),(unsigned long long)(ms()-p->began));fflush(stdout);
 if(quarantined)return rc;
 vv_https_secure_zero(s->pcm,(size_t)MAX_SAMPLES*2);free(s->pcm);
 vv_https_secure_zero(s,sizeof(*s));free(s);pthread_mutex_unlock(&session_speaker_lock);return rc;
}
int vv_stream_speaker_run_real_tts_ex(vv_stream_pcm_fetch_ex fetch,void*arg,const struct vv_https_cancel*cancel){
 return spooled_run(fetch,arg,cancel,false,NULL);
}
int vv_stream_speaker_run_dialogue(vv_stream_pcm_fetch_ex fetch,void*arg,const struct vv_https_cancel*cancel){
 return spooled_run(fetch,arg,cancel,true,NULL);
}
int vv_stream_speaker_run_dialogue_scoped(vv_stream_pcm_fetch_ex fetch,void*arg,
 const struct vv_https_cancel*cancel,const struct vv_reply_scope_token*lease){
 if(!lease||!lease->scope)return -EINVAL;
 return spooled_run(fetch,arg,cancel,true,lease);
}
#else
#define CAP 65536u
#define PREBUFFER 8192u
#define REBUFFER 16384u
#define REBUFFER_MS 5000u
#define TOTAL_MS 90000u
#define MAX_SAMPLES 480000u
struct speaker {
 int16_t pcm[CAP];atomic_uint produced,consumed;
 atomic_bool stop,eof,play_done,net_done;
 pthread_t play_thread;bool play_live;
 int play_rc,net_rc;uint64_t began,first;
 unsigned pauses;bool refill_requested;int pull_rc;
 vv_stream_pcm_fetch fetch;void *arg;const struct vv_https_cancel *outer;
};
static uint64_t ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static void pause_ms(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
/* Only run's calling thread evaluates the borrowed, possibly stateful guard. */
static bool stopped(void *arg){struct speaker*s=arg;return atomic_load(&s->stop);}
static int play_cancel(void *arg){struct speaker*s=arg;uint64_t now=ms();if(now<s->began||now-s->began>=TOTAL_MS){s->pull_rc=-ETIMEDOUT;return 1;}return stopped(s);}
static int pull(void *arg,int16_t *value){
 struct speaker*s=arg;if(stopped(s))return s->pull_rc=-ECANCELED;
 unsigned c=atomic_load_explicit(&s->consumed,memory_order_relaxed),p=atomic_load_explicit(&s->produced,memory_order_acquire);
 if(c==p){
  if(!atomic_load_explicit(&s->eof,memory_order_acquire)){
   /* End this hardware run cleanly. Its FIFO drains before the worker waits;
    * no generated silence, sample replay or blocking inside active PIO. */
   s->refill_requested=true;return 0;
  }
  p=atomic_load_explicit(&s->produced,memory_order_acquire);if(c==p)return 0;
 }
 *value=s->pcm[c%CAP];atomic_store_explicit(&s->consumed,c+1,memory_order_release);return 1;
}
static void *play_worker(void *arg){
 struct speaker*s=arg;s->first=ms();
 for(;;){
  s->refill_requested=false;
  s->play_rc=k7sound_speak_stream16(pull,s,play_cancel,s);
  if(s->pull_rc==-ETIMEDOUT)s->play_rc=-ETIMEDOUT;
  if(s->play_rc||stopped(s))break;
  bool final=atomic_load_explicit(&s->eof,memory_order_acquire);
  unsigned p=atomic_load_explicit(&s->produced,memory_order_acquire);
  unsigned c=atomic_load_explicit(&s->consumed,memory_order_acquire);
  if(final&&p==c)break;
  if(!s->refill_requested){s->play_rc=-EPIPE;break;}
  if(s->pauses==3){s->play_rc=-EAGAIN;break;}
  ++s->pauses;
  uint64_t began=ms();
  for(;;){
   if(stopped(s)){s->play_rc=-ECANCELED;break;}
   uint64_t now=ms();
   if(now<began||now-began>=REBUFFER_MS||now<s->began||now-s->began>=TOTAL_MS){s->play_rc=-ETIMEDOUT;break;}
   p=atomic_load_explicit(&s->produced,memory_order_acquire);
   c=atomic_load_explicit(&s->consumed,memory_order_acquire);
   final=atomic_load_explicit(&s->eof,memory_order_acquire);
   if(final){p=atomic_load_explicit(&s->produced,memory_order_acquire);break;}
   if(p-c>=REBUFFER)break;
   pause_ms();
  }
  printf("TTS_REBUFFER pause=%u queued=%u eof=%u rc=%d wait_ms=%llu\n",s->pauses,p-c,(unsigned)atomic_load(&s->eof),s->play_rc,(unsigned long long)(ms()-began));fflush(stdout);
  if(s->play_rc||(final&&p==c))break;
 }
 if(stopped(s)&&!s->play_rc)s->play_rc=-ECANCELED;
 if(s->play_rc)atomic_store(&s->stop,true);
 atomic_store_explicit(&s->play_done,true,memory_order_release);return NULL;
}
static int launch(pthread_t*t,void *(*entry)(void*),void*arg,unsigned cpu,unsigned stack){
 pthread_attr_t a;int e=pthread_attr_init(&a);if(e)return -e;
 e=pthread_attr_setstacksize(&a,stack);
#ifdef CONFIG_SMP
 if(!e){cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);e=pthread_attr_setaffinity_np(&a,sizeof(set),&set);}
#else
 (void)cpu;
#endif
 if(!e)e=pthread_create(t,&a,entry,arg);
 pthread_attr_destroy(&a);return -e;
}
static int start_play(struct speaker*s){if(s->play_live)return 0;int rc=launch(&s->play_thread,play_worker,s,1,32768);if(!rc)s->play_live=true;return rc;}
static int sink(void *arg,const unsigned char *data,size_t bytes){
 struct speaker*s=arg;if(!data||bytes%2)return -EINVAL;
 unsigned p=atomic_load(&s->produced);if(bytes/2>MAX_SAMPLES-p)return -EOVERFLOW;
 for(size_t i=0;i<bytes;i+=2){
  for(;;){
   if(stopped(s))return -ECANCELED;
   uint64_t now=ms();if(now<s->began||now-s->began>=TOTAL_MS)return -ETIMEDOUT;
   if(s->play_live&&atomic_load_explicit(&s->play_done,memory_order_acquire))return s->play_rc?s->play_rc:-EPIPE;
   if(p-atomic_load_explicit(&s->consumed,memory_order_acquire)<CAP)break;
   pause_ms();
  }
  s->pcm[p%CAP]=(int16_t)((uint16_t)data[i]|((uint16_t)data[i+1]<<8));
  atomic_store_explicit(&s->produced,++p,memory_order_release);
  if(!s->play_live&&p>=PREBUFFER){int rc=start_play(s);if(rc)return rc;}
 }
 return 0;
}
static void *network_worker(void *arg){
 struct speaker*s=arg;struct vv_https_cancel token={stopped,s};
 int rc=s->fetch(s->arg,sink,s,&token);
 if(!rc&&!atomic_load(&s->produced))rc=-EPROTO;
 if(rc)atomic_store(&s->stop,true);
 atomic_store_explicit(&s->eof,true,memory_order_release);
 if(!rc&&!s->play_live)rc=start_play(s);
 int fetch_rc=rc;
 if(s->play_live){int e=pthread_join(s->play_thread,NULL);if(e){atomic_store(&s->stop,true);while(!atomic_load_explicit(&s->play_done,memory_order_acquire))pause_ms();(void)pthread_detach(s->play_thread);rc=-e;}else if(s->play_rc&&(!rc||rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED))rc=s->play_rc;}
 if(rc)printf("TTS_STREAM_FAILURE fetch_rc=%d play_rc=%d pull_rc=%d pauses=%u\n",fetch_rc,s->play_rc,s->pull_rc,s->pauses);
 if(stopped(s)&&!rc)rc=-ECANCELED;
 s->net_rc=rc;atomic_store_explicit(&s->net_done,true,memory_order_release);return NULL;
}
int vv_stream_speaker_run(vv_stream_pcm_fetch fetch,void *arg,const struct vv_https_cancel *cancel){
 if(!fetch)return -EINVAL;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 struct speaker*s=calloc(1,sizeof(*s));if(!s)return -ENOMEM;
 atomic_init(&s->produced,0);atomic_init(&s->consumed,0);atomic_init(&s->stop,false);atomic_init(&s->eof,false);atomic_init(&s->play_done,false);atomic_init(&s->net_done,false);
 s->fetch=fetch;s->arg=arg;s->outer=cancel;s->began=ms();pthread_t thread;
 int rc=launch(&thread,network_worker,s,0,65536);
 if(!rc){
  int coordination=0;
  while(!atomic_load_explicit(&s->net_done,memory_order_acquire)){
   if(!coordination){
    if(s->outer&&s->outer->is_cancelled&&s->outer->is_cancelled(s->outer->arg))coordination=-ECANCELED;
    uint64_t now=ms();
    if(!coordination&&(now<s->began||now-s->began>=TOTAL_MS))coordination=-ETIMEDOUT;
    if(coordination)atomic_store(&s->stop,true);
   }
   pause_ms();
  }
  int e=pthread_join(thread,NULL);
  if(e){atomic_store(&s->stop,true);(void)pthread_detach(thread);rc=-e;}
  else{
   rc=s->net_rc;
   if(coordination&&(!rc||rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED))rc=coordination;
  }
 }
 printf("TTS_STREAM rc=%d produced=%u consumed=%u first_play_ms=%llu total_ms=%llu\n",rc,atomic_load(&s->produced),atomic_load(&s->consumed),(unsigned long long)(s->first?s->first-s->began:0),(unsigned long long)(ms()-s->began));fflush(stdout);
 vv_https_secure_zero(s,sizeof(*s));free(s);return rc;
}
int vv_stream_speaker_run_real_tts(vv_stream_pcm_fetch fetch,void*arg,const struct vv_https_cancel*cancel){
 return vv_stream_speaker_run(fetch,arg,cancel);
}
struct legacy_fetch_ex {vv_stream_pcm_fetch_ex fetch;void *arg;};
static int legacy_fetch(void *arg,vv_stream_pcm_sink sink,void *sink_arg,const struct vv_https_cancel *cancel){
 struct legacy_fetch_ex *f=arg;struct vv_stream_fetch_result result={0};
 return f->fetch(f->arg,sink,sink_arg,cancel,&result);
}
int vv_stream_speaker_run_real_tts_ex(vv_stream_pcm_fetch_ex fetch,void*arg,const struct vv_https_cancel*cancel){
 if(!fetch)return -EINVAL;
 struct legacy_fetch_ex f={fetch,arg};return vv_stream_speaker_run(legacy_fetch,&f,cancel);
}
int vv_stream_speaker_run_dialogue(vv_stream_pcm_fetch_ex fetch,void*arg,const struct vv_https_cancel*cancel){
 (void)fetch;(void)arg;(void)cancel;return -ENOTSUP;
}
int vv_stream_speaker_run_dialogue_scoped(vv_stream_pcm_fetch_ex fetch,void*arg,
 const struct vv_https_cancel*cancel,const struct vv_reply_scope_token*lease){
 (void)fetch;(void)arg;(void)cancel;(void)lease;return -ENOTSUP;
}
#endif /* CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION */
