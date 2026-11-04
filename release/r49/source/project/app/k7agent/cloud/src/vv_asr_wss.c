#include "vv_asr_wss.h"
#include <errno.h>
#include <string.h>
static ssize_t rd(void *a,void *p,size_t n){struct vv_asr_wss *c=a;return c->transport.read(c->transport.arg,p,n);}
static ssize_t wr(void *a,const void *p,size_t n){struct vv_asr_wss *c=a;return c->transport.write(c->transport.arg,p,n);}
static int rng(void *a,uint8_t *p,size_t n){struct vv_asr_wss *c=a;return c->transport.random(c->transport.arg,p,n);}
static void closed(void *a){struct vv_asr_wss *c=a;c->transport.close(c->transport.arg);}
static void session_close(void *a){struct vv_asr_wss *c=a;vv_wsi_abort(&c->ws);}
static int enqueue(void *a,unsigned op,const void *p,size_t n,uint64_t deadline){
 struct vv_asr_wss *c=a;int rc=vv_wsi_queue(&c->ws,op,p,n);if(!rc)c->tx_deadline=deadline;return rc;
}
static int delivered(void *a,unsigned op,const void *p,size_t n){
 struct vv_asr_wss *c=a;struct vv_asr_event ev;
 int rc=vv_asr_session_message(&c->session,op,p,n,c->now,&ev);
 if(c->event&&ev.kind!=VV_ASR_NO_EVENT)c->event(c->event_arg,&ev);
 return rc;
}
static int busy(const struct vv_asr_wss *c){return c->ws.tx_length||c->ws.pending_opcode||c->ws.close_after||c->ws.tx_close;}
static int enter(struct vv_asr_wss *c,uint64_t now){
 if(!c||c->entered)return -EBUSY;
 if(!c->session.active||!c->ws.active)return -EINVAL;
 c->entered=true;c->now=now;return 0;
}
static int leave(struct vv_asr_wss *c,int rc){c->entered=false;return rc;}
int vv_asr_wss_start(struct vv_asr_wss *c,const struct vv_wsi_ops *io,void *storage,size_t capacity,uint64_t now,void (*event)(void *,const struct vv_asr_event *),void *arg){
 if(!c||!io||!io->read||!io->write||!io->random||!io->close||now>UINT64_MAX-80000)return -EINVAL;
 if(c->entered||c->session.active||c->ws.active)return -EBUSY;
 memset(c,0,sizeof(*c));c->transport=*io;c->event=event;c->event_arg=arg;c->now=now;c->entered=true;
 struct vv_wsi_ops ops={rd,wr,rng,closed,delivered,c};int rc=vv_wsi_init(&c->ws,&ops,storage,capacity,now,now+80000);
 if(rc)return leave(c,rc);
 struct vv_asr_session_io sio={enqueue,session_close,c};rc=vv_asr_session_start(&c->session,&sio,now);
 if(rc)vv_wsi_abort(&c->ws);
 return leave(c,rc);
}
int vv_asr_wss_tick(struct vv_asr_wss *c,uint64_t now,bool cancel,unsigned budget){
 int rc=enter(c,now);if(rc)return rc;
 rc=vv_asr_session_tick(&c->session,now);
 if(!rc&&busy(c)&&c->tx_deadline&&now>=c->tx_deadline)rc=-ETIMEDOUT;
 if(!rc)rc=vv_wsi_pump(&c->ws,now,cancel,budget);
 if(!busy(c))c->tx_deadline=0;
 else if(!c->tx_deadline)c->tx_deadline=now>UINT64_MAX-10000?UINT64_MAX:now+10000;
 if(rc||(!c->ws.active&&c->session.active))vv_asr_session_abort(&c->session);
 return leave(c,rc);
}
int vv_asr_wss_audio(struct vv_asr_wss *c,const void *pcm,size_t n,uint64_t now){
 int rc=enter(c,now);if(rc)return rc;
 rc=vv_asr_session_tick(&c->session,now);
 if(!rc)rc=busy(c)?-EAGAIN:vv_asr_session_audio(&c->session,pcm,n,now);
 return leave(c,rc);
}
static int control(struct vv_asr_wss *c,uint64_t now,bool cancel){
 int rc=enter(c,now);if(rc)return rc;
 rc=vv_asr_session_tick(&c->session,now);
 if(!rc)rc=busy(c)?-EAGAIN:(cancel?vv_asr_session_cancel(&c->session,now):vv_asr_session_stop(&c->session,now));
 return leave(c,rc);
}
int vv_asr_wss_stop(struct vv_asr_wss *c,uint64_t now){return control(c,now,false);}
int vv_asr_wss_cancel(struct vv_asr_wss *c,uint64_t now){return control(c,now,true);}
void vv_asr_wss_abort(struct vv_asr_wss *c){if(c&&!c->entered){vv_asr_session_abort(&c->session);vv_wsi_abort(&c->ws);}}
