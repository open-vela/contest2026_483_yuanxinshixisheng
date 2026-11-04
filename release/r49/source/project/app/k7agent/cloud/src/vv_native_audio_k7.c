#include "vv_native_audio.h"
#include "capture_stream.h"
#include "stream_ring.h"
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
/* Covers the whole adapter lifetime, including a failed join. A retained
 * producer cannot be superseded by a second recording session. */
static int capture_owner;
struct capture {struct k7_stream_ring ring;pthread_t thread;unsigned samples;int done,result,started,joined;};
static void *producer(void *a){struct capture *c=a;c->result=k7sound_capture_stream_cued(c->samples,k7_stream_ring_sample,&c->ring);__atomic_store_n(&c->done,1,__ATOMIC_RELEASE);return NULL;}
static int start(void *a){struct capture *c=a;pthread_attr_t attr;int e=pthread_attr_init(&attr);if(e)return -e;e=pthread_attr_setstacksize(&attr,32768);if(!e)e=pthread_create(&c->thread,&attr,producer,c);pthread_attr_destroy(&attr);if(!e)c->started=1;return -e;}
static int take(void *a,uint8_t *p,uint32_t *seq){return k7_stream_ring_take(&((struct capture *)a)->ring,p,seq);}
static int done(void *a,int *result){struct capture *c=a;if(!__atomic_load_n(&c->done,__ATOMIC_ACQUIRE))return 0;*result=c->result;return 1;}
static int stop_join(void *a){struct capture *c=a;if(!c->started||c->joined)return 0;k7_stream_ring_cancel(&c->ring);int e=pthread_join(c->thread,NULL);if(!e)c->joined=1;return -e;}
int vv_native_audio_k7_run(struct vv_asr_tls *net,unsigned samples,const struct vv_native_audio_clock *clock){
 if(!net||!clock||samples<320||samples>48000||samples%320)return -EINVAL;
 int expected=0;if(!__atomic_compare_exchange_n(&capture_owner,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -EBUSY;
 struct capture *c=calloc(1,sizeof(*c));if(!c){__atomic_store_n(&capture_owner,0,__ATOMIC_RELEASE);return -ENOMEM;}c->samples=samples;k7_stream_ring_init(&c->ring);
 struct vv_native_capture cap={start,take,done,stop_join,c};int rc=vv_native_audio_run(net,&cap,clock,samples/320);
 /* Failed join retains producer-owned memory, never free live callback storage. */
 if(!c->started||c->joined){free(c);__atomic_store_n(&capture_owner,0,__ATOMIC_RELEASE);}
 return rc;
}

uint64_t vv_native_audio_posix_now(void *unused){struct timespec t;(void)unused;if(clock_gettime(CLOCK_MONOTONIC,&t))return UINT64_MAX;return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;}
int vv_native_audio_posix_wait(void *unused,int fd,short events,unsigned ms){(void)unused;if(ms>5)ms=5;struct pollfd p={fd,events,0};int rc=poll(&p,1,(int)ms);if(rc<0&&errno!=EINTR)return -errno;if(rc>0&&(p.revents&(POLLERR|POLLNVAL)))return -EIO;return 0;}
