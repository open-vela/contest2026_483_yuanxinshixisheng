#include "vv_paraformer.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
#define PF_MAX_FRAMES 500u
#define PF_CAPTURE_PHASE_MS 25000u
#else
#define PF_MAX_FRAMES 150u
#define PF_CAPTURE_PHASE_MS 20000u
#endif
struct run {
 const struct vv_pf_run_config *c;struct vv_pf_protocol protocol;struct vv_wsi ws;
 unsigned char rx[VV_PF_RX_CAP];
 bool progress;
};
static ssize_t read_io(void*a,void*p,size_t n){struct run*s=a;ssize_t rc=s->c->io.read(s->c->io.arg,p,n);if(rc>0)s->progress=true;return rc;}
static ssize_t write_io(void*a,const void*p,size_t n){struct run*s=a;ssize_t rc=s->c->io.write(s->c->io.arg,p,n);if(rc>0)s->progress=true;return rc;}
static int random_io(void*a,uint8_t*p,size_t n){struct run*s=a;return s->c->io.random(s->c->io.arg,p,n);}
static void close_io(void*a){struct run*s=a;s->c->io.close(s->c->io.arg);}
static int message(void*a,unsigned op,const void*p,size_t n){struct run*s=a;return op==1?vv_pf_event(&s->protocol,p,n):-EPROTO;}
static bool busy(const struct run*s){return s->ws.tx_length||s->ws.pending_opcode||s->ws.close_after||s->ws.tx_close;}
int vv_pf_run(const struct vv_pf_run_config*c,const struct vv_https_cancel*cancel,struct vv_asr_batch_result*out){
 if(out)memset(out,0,sizeof(*out));
 if(!c||!out||!c->io.read||!c->io.write||!c->io.random||!c->io.close||
    !c->capture.start||!c->capture.take||!c->capture.done||!c->capture.join||!c->now||!c->sleep_ms)return -EINVAL;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
 if(!c->capture.request_finish)return -EINVAL;
 bool finish_requested=false;
 unsigned drain_burst=0;uint64_t drain_burst_at=0;
#endif
 struct run*s=calloc(1,sizeof(*s));if(!s){c->io.close(c->io.arg);return -ENOMEM;}s->c=c;
 uint64_t began=c->now(c->clock_arg),last=began,phase=began+15000,tx_until=0;
 int rc=0,stage=1;bool started=false,joined=false,owned=false;unsigned frames=0;
 uint64_t ready_at=0,first_pcm_at=0,finish_at=0;
 /* Observation-only loop timestamps: sampled before the nonblocking pump,
  * not exact server arrival times. Bits 0..5 mark available fields below. */
 unsigned timing_mask=0;
 uint64_t final_loop=0,finish_request_loop=0,drained_loop=0;
 uint64_t finish_queue_loop=0,task_finished_loop=0;
 uint8_t random[16],pcm[640];char control[768];size_t length=0;
 if(began>UINT64_MAX-60000){rc=-ETIMEDOUT;goto done;}
 struct vv_wsi_ops ops={read_io,write_io,random_io,close_io,message,s};
 rc=vv_wsi_init(&s->ws,&ops,s->rx,sizeof(s->rx),began,began+60000);if(rc)goto done;owned=true;
 rc=c->io.random(c->io.arg,random,sizeof(random));if(rc)goto done;
 rc=vv_pf_start(&s->protocol,random,control,sizeof(control),&length);if(rc)goto done;
 rc=vv_wsi_queue(&s->ws,1,control,length);if(rc)goto done;tx_until=began+10000;
 for(;;){
  uint64_t now=c->now(c->clock_arg);
  if(now<last||now>=began+60000||now>=phase||(tx_until&&now>=tx_until)){rc=-ETIMEDOUT;break;}last=now;
  if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg)){rc=-ECANCELED;break;}
  s->progress=false;
  rc=vv_wsi_pump(&s->ws,now,false,1);
  if(rc)break;
  if(s->protocol.finals&&!(timing_mask&1u)){final_loop=now-began;timing_mask|=1u;}
  if(s->protocol.state==3){task_finished_loop=now-began;timing_mask|=16u;if(!joined||!frames)rc=-EPROTO;break;}
  if(!s->ws.active){rc=-ECONNRESET;break;}
  if(!busy(s))tx_until=0;else if(!tx_until)tx_until=now+10000;
  if(s->protocol.state==1&&!started){
   stage=2;ready_at=now-began;rc=c->capture.start(c->capture.arg);if(rc)break;started=true;phase=now+PF_CAPTURE_PHASE_MS;
  }
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
  bool utterance_done=s->protocol.command_seen;
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
  /* A cloud-final sentence can finish an ordinary question too. Partial text
   * remains ineligible; task-finished and final command aggregation still gate
   * dispatch, including a stop/negation received while draining the socket. */
  utterance_done=utterance_done||(s->protocol.finals&&s->protocol.used&&!s->protocol.pending_partial);
#endif
  if(started&&!joined&&!finish_requested&&utterance_done){
   finish_request_loop=now-began;timing_mask|=2u;
   rc=c->capture.request_finish(c->capture.arg);if(rc)break;finish_requested=true;
  }
#endif
  if(started&&!joined&&!busy(s)){
   stage=3;int got=c->capture.take(c->capture.arg,pcm);if(got<0){rc=got;break;}
   if(got){
    if(got!=1||frames>=PF_MAX_FRAMES){rc=-EOVERFLOW;break;}
    rc=vv_wsi_queue(&s->ws,2,pcm,sizeof(pcm));if(rc)break;s->progress=true;if(!frames)first_pcm_at=now-began;frames++;tx_until=now+10000;
   }else{
    int finished=c->capture.done(c->capture.arg);if(finished<0){rc=finished;break;}
    if(finished){
     /* Recheck after acquire(done): the last frame may have published since take. */
     got=c->capture.take(c->capture.arg,pcm);if(got<0){rc=got;break;}
     if(got){if(frames>=PF_MAX_FRAMES){rc=-EOVERFLOW;break;}rc=vv_wsi_queue(&s->ws,2,pcm,sizeof(pcm));if(rc)break;s->progress=true;if(!frames)first_pcm_at=now-began;frames++;tx_until=now+10000;}
     else{
      drained_loop=now-began;timing_mask|=4u;
      stage=4;finish_at=now-began;rc=c->capture.join(c->capture.arg,false);joined=true;if(rc||!frames){if(!rc)rc=-EIO;break;}
      rc=vv_pf_finish(&s->protocol,control,sizeof(control),&length);if(rc)break;
      rc=vv_wsi_queue(&s->ws,1,control,length);if(rc)break;tx_until=now+10000;phase=now+20000;
      finish_queue_loop=now-began;timing_mask|=8u;
     }
    }
   }
  }
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
  /* Bounded catch-up only after the original capture endpoint was frozen.
   * Re-enter all clock/cancel/RX checks; blocked rounds yield immediately. */
  if(finish_requested&&!joined&&s->progress){
   if(!drain_burst)drain_burst_at=now;
   if(++drain_burst<4u&&now-drain_burst_at<4u)continue;
  }
  drain_burst=0;
#endif
  c->sleep_ms(c->sleep_arg,1);
 }
done:
 if(started&&!joined){int cleanup=c->capture.join(c->capture.arg,true);if(cleanup)rc=cleanup;}
 if(owned)vv_wsi_abort(&s->ws);else c->io.close(c->io.arg);
 if(!rc&&s->protocol.state==3){memcpy(out->text,s->protocol.text,sizeof(out->text));out->http_status=101;}
 else if(!rc)rc=-EPROTO;
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
 if(!rc){out->command_intent=vv_pf_command(&s->protocol);out->command_finalized=true;}
#endif
 printf("PARAFORMER stage=%d rc=%d frames=%u finals=%u\n",stage,rc,frames,s->protocol.finals);fflush(stdout);
 printf("PARAFORMER_TEXT merged_bytes=%lu output_bytes=%lu\n",
  (unsigned long)s->protocol.used,(unsigned long)strlen(out->text));fflush(stdout);
 printf("PARAFORMER ready_ms=%llu first_pcm_queued_ms=%llu finish_ms=%llu\n",
  (unsigned long long)ready_at,(unsigned long long)first_pcm_at,(unsigned long long)finish_at);fflush(stdout);
 vv_https_secure_zero(random,sizeof(random));vv_https_secure_zero(pcm,sizeof(pcm));vv_https_secure_zero(control,sizeof(control));
 vv_https_secure_zero(s,sizeof(*s));free(s);
 /* The sole extra clock read is after resource cleanup and all decisions.
  * Invalid/rollback observations stay unavailable and never alter rc. */
 uint64_t timing_end=c->now(c->clock_arg),cleanup_done=0;
 unsigned timing_clock_invalid=timing_end==UINT64_MAX||timing_end<last||timing_end<began;
 if(!timing_clock_invalid){cleanup_done=timing_end-began;timing_mask|=32u;}
 printf("PF_TIMING valid_mask=%u clock_invalid=%u final_loop_ms=%llu finish_request_loop_ms=%llu drained_loop_ms=%llu finish_queue_loop_ms=%llu task_finished_loop_ms=%llu cleanup_done_ms=%llu rc=%d\n",
  timing_mask,timing_clock_invalid,(unsigned long long)final_loop,
  (unsigned long long)finish_request_loop,(unsigned long long)drained_loop,
  (unsigned long long)finish_queue_loop,(unsigned long long)task_finished_loop,
  (unsigned long long)cleanup_done,rc);fflush(stdout);
 return rc;
}
