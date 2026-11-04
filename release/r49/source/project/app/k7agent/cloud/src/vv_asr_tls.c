#include "vv_asr_tls.h"
#include "vv_wss_client.h"
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif
#include <string.h>
static ssize_t readfn(void *a,void *p,size_t n){struct vv_asr_tls *s=a;short want=0;ssize_t r=vv_https_mbedtls_try_read(s->tls,p,n,&want);if(r==-EAGAIN){s->wanted_events|=want;return VV_WSI_BLOCK;}return r;}
static ssize_t writefn(void *a,const void *p,size_t n){struct vv_asr_tls *s=a;short want=0;ssize_t r=vv_https_mbedtls_try_write(s->tls,p,n,&want);if(r==-EAGAIN){s->wanted_events|=want;return VV_WSI_BLOCK;}return r;}
static int randomfn(void *a,uint8_t *p,size_t n){return vv_https_mbedtls_random(((struct vv_asr_tls *)a)->tls,p,n);}
static void closefn(void *a){struct vv_asr_tls *s=a;if(s->transport_live){s->transport_live=false;vv_https_mbedtls_ops()->close(s->tls);}s->wanted_events=0;}
int vv_asr_tls_open(struct vv_asr_tls *s,struct vv_https_mbedtls *tls,const char *host,uint16_t port,const char *path,const char *auth,vv_https_now_ms_fn clock,void *clock_arg,uint64_t deadline,const struct vv_https_cancel *cancel,void *storage,size_t cap,void (*event)(void *,const struct vv_asr_event *),void *arg){
 if(!s||!tls||!clock||!storage||!cap)return -EINVAL;
 if(s->transport_live||s->asr.entered||s->asr.ws.active||s->asr.session.active)return -EBUSY;
 memset(s,0,sizeof(*s));s->tls=tls;
 struct vv_wss_client handshake={0};handshake.io=vv_https_mbedtls_ops();handshake.io_arg=tls;handshake.now=clock;handshake.now_arg=clock_arg;handshake.random=vv_https_mbedtls_random;handshake.random_arg=tls;handshake.sha1=vv_https_mbedtls_sha1;handshake.max_message=cap;
 int rc=vv_wss_open(&handshake,host,port,path,auth,deadline,cancel);
 if(rc)return rc; /* Handshake owns failure cleanup, including connect failure. */
 s->transport_live=true;handshake.open=false; /* Transfer one close owner. */
 struct vv_wsi_ops ops={readfn,writefn,randomfn,closefn,NULL,s};
 rc=vv_asr_wss_start(&s->asr,&ops,storage,cap,clock(clock_arg),event,arg);
 if(rc)closefn(s);else s->wanted_events=POLLOUT;
 return rc;
}
int vv_asr_tls_tick(struct vv_asr_tls *s,uint64_t now,bool cancel,unsigned budget){
 if(!s||!s->transport_live)return -EINVAL;
 s->wanted_events=0;int rc=vv_asr_wss_tick(&s->asr,now,cancel,budget);
 if(!s->transport_live)s->wanted_events=0;
 else if(!s->wanted_events)s->wanted_events=s->asr.ws.tx_length?POLLOUT:POLLIN;
 return rc;
}
int vv_asr_tls_audio(struct vv_asr_tls *s,const void *p,size_t n,uint64_t now){if(!s||!s->transport_live)return -EINVAL;int r=vv_asr_wss_audio(&s->asr,p,n,now);if(!r)s->wanted_events|=POLLOUT;return r;}
int vv_asr_tls_stop(struct vv_asr_tls *s,uint64_t now){if(!s||!s->transport_live)return -EINVAL;int r=vv_asr_wss_stop(&s->asr,now);if(!r)s->wanted_events|=POLLOUT;return r;}
int vv_asr_tls_cancel(struct vv_asr_tls *s,uint64_t now){if(!s||!s->transport_live)return -EINVAL;int r=vv_asr_wss_cancel(&s->asr,now);if(!r&&s->transport_live)s->wanted_events|=POLLOUT;return r;}
int vv_asr_tls_fd(const struct vv_asr_tls *s){return s&&s->transport_live?vv_https_mbedtls_fd(s->tls):-1;}
short vv_asr_tls_wanted(const struct vv_asr_tls *s){return s&&s->transport_live?s->wanted_events:0;}
void vv_asr_tls_close(struct vv_asr_tls *s){if(s&&!s->asr.entered){vv_asr_wss_abort(&s->asr);closefn(s);}}
