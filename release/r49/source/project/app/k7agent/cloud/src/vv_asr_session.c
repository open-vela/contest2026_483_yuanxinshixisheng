#include "vv_asr_session.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
static uint64_t after(uint64_t t,uint64_t d){return t>UINT64_MAX-d?UINT64_MAX:t+d;}
static void close_once(struct vv_asr_session *s){if(!s->closed){s->closed=1;s->io.close(s->io.arg);}s->active=0;}
void vv_asr_session_abort(struct vv_asr_session *s){if(s&&s->active){vv_asr_stream_abort(&s->stream);close_once(s);}}
static int failure(struct vv_asr_session *s,int e){vv_asr_session_abort(s);return e;}
int vv_asr_session_tick(struct vv_asr_session *s,uint64_t now){
 if(!s||!s->active)return -EINVAL;
 if(now<s->last_now)return failure(s,-EINVAL);
 s->last_now=now;
 if(now>=after(s->started,80000)||now>=s->phase_deadline)return failure(s,-ETIMEDOUT);
 if(s->stream.state==VV_ASR_STREAMING&&s->stream.sent>s->stream.acknowledged&&now>=s->ack_deadline[s->stream.acknowledged%VV_ASR_WINDOW])return failure(s,-ETIMEDOUT);
 return 0;
}
static int send_once(struct vv_asr_session *s,unsigned op,const void *p,size_t n,uint64_t now){
 uint64_t d=after(now,10000),total=after(s->started,80000);
 if(d>total)d=total;
 if(d>s->phase_deadline)d=s->phase_deadline;
 int rc=s->io.send(s->io.arg,op,p,n,d);
 return rc?failure(s,rc<0?rc:-EIO):0;
}
int vv_asr_session_start(struct vv_asr_session *s,const struct vv_asr_session_io *io,uint64_t now){
 if(!s||!io||!io->send||!io->close)return -EINVAL;
 /* Caller supplies zero initialized storage; an active session cannot be replaced. */
 if(s->active)return -EBUSY;
 memset(s,0,sizeof(*s));s->io=*io;s->started=s->last_now=now;s->active=1;
 s->phase_deadline=after(now,15000);vv_asr_stream_init(&s->stream);
 char b[256];size_t n;int rc=vv_asr_stream_start(&s->stream,b,sizeof(b),&n);
 if(rc)return failure(s,rc);
 return send_once(s,1,b,n,now);
}
int vv_asr_session_audio(struct vv_asr_session *s,const void *pcm,size_t n,uint64_t now){
 int rc=vv_asr_session_tick(s,now);if(rc)return rc;
 if(s->stream.state!=VV_ASR_STREAMING)return -EINVAL;
 if(now<s->next_audio)return -EAGAIN;
 unsigned char packet[VV_ASR_PACKET_BYTES];uint32_t seq=s->stream.sent;
 rc=vv_asr_stream_audio(&s->stream,pcm,n,packet,sizeof(packet));if(rc)return rc;
 s->ack_deadline[seq%VV_ASR_WINDOW]=after(now,10000);s->next_audio=after(now,20);
 rc=send_once(s,2,packet,sizeof(packet),now);
 volatile unsigned char *v=packet;for(size_t i=0;i<sizeof(packet);i++)v[i]=0;
 return rc;
}
static int control(struct vv_asr_session *s,uint64_t now,int cancel){
 int rc=vv_asr_session_tick(s,now);if(rc)return rc;
 char b[128];size_t n;
 rc=cancel?vv_asr_stream_cancel(&s->stream,b,sizeof(b),&n):vv_asr_stream_stop(&s->stream,b,sizeof(b),&n);
 if(rc)return rc;
 s->phase_deadline=after(now,cancel?10000:20000);
 return send_once(s,1,b,n,now);
}
int vv_asr_session_stop(struct vv_asr_session *s,uint64_t now){return control(s,now,0);}
int vv_asr_session_cancel(struct vv_asr_session *s,uint64_t now){
 if(s&&s->active&&s->stream.state==VV_ASR_WAIT_READY){vv_asr_session_abort(s);return 0;}
 return control(s,now,1);
}
int vv_asr_session_message(struct vv_asr_session *s,unsigned op,const void *p,size_t n,uint64_t now,struct vv_asr_event *ev){
 if(!ev)return -EINVAL;
 memset(ev,0,sizeof(*ev));int rc=vv_asr_session_tick(s,now);if(rc)return rc;
 if(op!=1)return failure(s,-EPROTO);
 rc=vv_asr_stream_receive(&s->stream,p,n,ev);if(rc)return failure(s,rc);
 if(s->stream.state==VV_ASR_FAILED){close_once(s);return -EPROTO;}
 if(ev->kind==VV_ASR_READY){s->phase_deadline=UINT64_MAX;s->next_audio=now;}
 if(s->stream.state==VV_ASR_DONE||s->stream.state==VV_ASR_CANCELLED)close_once(s);
 return 0;
}
