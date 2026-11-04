#include "vv_native_audio.h"
#include <errno.h>
#include <string.h>
int vv_native_audio_run(struct vv_asr_tls *net,const struct vv_native_capture *cap,const struct vv_native_audio_clock *clk,unsigned frames){
 if(!net||!cap||!clk||!cap->start||!cap->take||!cap->done||!cap->stop_join||!clk->now||!clk->wait||!frames||frames>150)return -EINVAL;
 uint8_t pcm[640];uint32_t expected=0,seq=0;int started=0,joined=0,join_attempted=0,pending=0,stopped=0,rc=0;
 while(net->asr.session.active){
  uint64_t now=clk->now(clk->arg);
  if(clk->cancelled&&clk->cancelled(clk->arg)){rc=-ECANCELED;break;}
  rc=vv_asr_tls_tick(net,now,false,8);if(rc<0)break;
  if(!net->asr.session.active)break;
  if(!started&&net->asr.session.stream.state==VV_ASR_STREAMING){rc=cap->start(cap->arg);if(rc)break;started=1;}
  if(started&&!stopped){
   /* Producer error must cancel even while an unsent frame is backpressured. */
   int capture_result=0;int capture_done=cap->done(cap->arg,&capture_result);
   if(capture_done<0){rc=capture_done;break;}
   if(capture_done&&capture_result){rc=capture_result;break;}
   if(!pending){rc=cap->take(cap->arg,pcm,&seq);if(rc<0)break;if(rc==1){if(seq!=expected||expected>=frames){rc=-EIO;break;}pending=1;}}
   if(pending){rc=vv_asr_tls_audio(net,pcm,sizeof(pcm),now);if(rc&&rc!=-EAGAIN)break;if(!rc){pending=0;expected++;}}
   if(!pending){int result=0;int done=cap->done(cap->arg,&result);if(done<0){rc=done;break;}
    if(done){if(result){rc=result;break;}
     /* Completion is release/acquire; recheck ring after observing done. */
     rc=cap->take(cap->arg,pcm,&seq);if(rc<0)break;
     if(rc==1){if(seq!=expected||expected>=frames){rc=-EIO;break;}pending=1;}
     else {if(expected!=frames){rc=-EIO;break;}join_attempted=1;rc=cap->stop_join(cap->arg);if(rc)break;joined=1;stopped=1;}
    }
   }
  }
  if(stopped&&net->asr.session.stream.state==VV_ASR_STREAMING){rc=vv_asr_tls_stop(net,now);if(rc&&rc!=-EAGAIN)break;}
  rc=clk->wait(clk->arg,vv_asr_tls_fd(net),vv_asr_tls_wanted(net),5);if(rc<0)break;
 }
 if(started&&!joined&&!join_attempted){int e=cap->stop_join(cap->arg);if(e)rc=e;}
 vv_asr_tls_close(net);
 volatile uint8_t *v=pcm;for(size_t i=0;i<sizeof(pcm);i++)v[i]=0;
 if(rc<0)return rc;
 return stopped&&net->asr.session.stream.state==VV_ASR_DONE?0:-EIO;
}
