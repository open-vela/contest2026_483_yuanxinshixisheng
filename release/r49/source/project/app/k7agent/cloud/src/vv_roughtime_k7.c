#include "vv_roughtime_k7.h"
#include "vv_roughtime.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
struct exchange {const struct vv_roughtime_io *io;const struct vv_https_cancel *cancel;uint64_t last,deadline;};
static int check(struct exchange*x){
 if(x->cancel&&x->cancel->is_cancelled&&x->cancel->is_cancelled(x->cancel->arg))return -ECANCELED;
 uint64_t n=x->io->now_ms(x->io->arg);
 if(n==UINT64_MAX||n<x->last)return -EIO;
 x->last=n;return n>=x->deadline?-ETIMEDOUT:0;
}
static int wait_ready(struct exchange*x,int fd,bool writing){
 for(;;){int rc=check(x);if(rc)return rc;
  unsigned ms=x->deadline-x->last>50?50:(unsigned)(x->deadline-x->last);
  rc=x->io->wait(x->io->arg,fd,writing,ms);
  if(rc<0&&rc!=-EINTR)return rc;
  int gate=check(x);if(gate)return gate;
  if(rc>0)return 0;
 }
}
int vv_roughtime_fetch_io(const struct vv_roughtime_io*io,const struct vv_https_cancel*c,
 struct vv_trusted_time_sample*out){
 if(out)memset(out,0,sizeof(*out));
 if(!out||!io||!io->now_ms||!io->nonce||!io->resolve||!io->open_peer||!io->wait||!io->send||!io->receive||!io->close)return -EINVAL;
 uint64_t begin=io->now_ms(io->arg);
 if(begin==UINT64_MAX||begin>UINT64_MAX-10000)return -EIO;
 struct exchange x={io,c,begin,begin+10000};
 uint8_t nonce[64]={0},request[1024],response[4096];
 int fd=-1,rc=check(&x),stage=1;uint32_t ipv4=0;size_t response_bytes=0;
 if(rc)goto done;
 rc=io->nonce(io->arg,nonce,x.deadline);if(rc)goto done;
 if((rc=check(&x)))goto done;
 rc=vv_roughtime_request(nonce,request);if(rc)goto done;
 stage=2;rc=io->resolve(io->arg,&ipv4,x.deadline);if(rc)goto done;
 if((rc=check(&x)))goto done;
 if(!ipv4){rc=-EHOSTUNREACH;goto done;}
 stage=3;fd=io->open_peer(io->arg,ipv4,2003);if(fd<0){rc=fd;goto done;}
 stage=4;
 for(;;){
  if((rc=check(&x)))goto done;
  ssize_t n=io->send(io->arg,fd,request,sizeof(request));
  if(n==(ssize_t)sizeof(request))break;
  if(n>=0){rc=-EIO;goto done;}
  if(n!=-EAGAIN&&n!=-EWOULDBLOCK&&n!=-EINTR){rc=(int)n;goto done;}
  if((rc=wait_ready(&x,fd,true)))goto done;
 }
 stage=5;
 for(;;){
  if((rc=wait_ready(&x,fd,false)))goto done;
  ssize_t n=io->receive(io->arg,fd,response,sizeof(response));
  if(n==-EAGAIN||n==-EWOULDBLOCK||n==-EINTR)continue;
  if(n<=0||n>=(ssize_t)sizeof(response)){rc=n<0?(int)n:-EBADMSG;goto done;}
  response_bytes=(size_t)n;break;
 }
 if((rc=check(&x)))goto done;
 uint64_t received=x.last;
 stage=6;struct vv_roughtime_result verified={0};
 rc=vv_roughtime_verify(response,response_bytes,nonce,&verified);if(rc)goto done;
 if((rc=check(&x)))goto done;
 /* Whole exchange is capped at 10s; no unauthenticated fallback or date bypass. */
 out->present=true;out->authorized=true;out->utc_seconds=verified.midpoint_us/1000000;
 out->received_mono_ms=received;
 if(c){out->cancelled=c->is_cancelled;out->cancel_arg=c->arg;}
 stage=7;
 done:
 if(fd>=0)io->close(io->arg,fd);
 memset(nonce,0,sizeof(nonce));
 if(rc)memset(out,0,sizeof(*out));
 printf("ROUGHTIME stage=%d rc=%d bytes=%u elapsed_ms=%llu\n",stage,rc,(unsigned)response_bytes,(unsigned long long)(x.last-begin));
 return rc;
}
#if defined(__NuttX__) && defined(CONFIG_EXAMPLES_K7CLOUD_ROUGHTIME)
#include "vv_dns_nuttx.h"
#include "vv_cloud_tcp.h"
#include "vv_k7_trng_development.h"
#include "k7_radio_service.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
struct network {struct vv_dns_backend dns;struct vv_cloud_tcp *shared;const struct vv_https_cancel *cancel;};
static uint64_t net_now(void*p){(void)p;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||(uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int net_nonce(void*p,uint8_t out[64],uint64_t d){
 if(!vv_k7_trng_development_approved(0))return -EACCES;
 uint64_t now=net_now(p);if(now==UINT64_MAX||now>=d||now>UINT64_MAX-100)return -ETIMEDOUT;
 if(d>now+100)d=now+100; /* Existing source rejects longer raw-read deadlines. */
 ssize_t n=vv_k7_trng_development_read(0,out,64,d);return n==64?0:n<0?(int)n:-EIO;
}
static int net_resolve(void*p,uint32_t*out,uint64_t d){struct network*n=p;uint64_t now=net_now(p);if(now==UINT64_MAX||now>=d)return -ETIMEDOUT;int code=0;
 if(n->shared)return vv_cloud_tcp_resolve_ipv4(n->shared,"roughtime.cloudflare.com",d,n->cancel,out);
 enum vv_dns_status status=n->dns.resolve_ipv4(n->dns.context,"roughtime.cloudflare.com",(uint32_t)(d-now),out,&code);
 printf("ROUGHTIME_DNS status=%d backend_code=%d\n",(int)status,code);
 return status==VV_DNS_OK?0:status==VV_DNS_DEADLINE_EXCEEDED?-ETIMEDOUT:status==VV_DNS_TEMPORARY_FAILURE?-EAGAIN:-EHOSTUNREACH;
}
static int net_open(void*p,uint32_t ip,uint16_t port){(void)p;int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return -errno;
 int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){int e=errno;close(fd);return -e;}
 struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=ip;
 if(connect(fd,(struct sockaddr*)&a,sizeof(a))<0){int e=errno;close(fd);return -e;}return fd;
}
static int net_wait(void*p,int fd,bool writing,unsigned ms){(void)p;struct pollfd f={fd,writing?POLLOUT:POLLIN,0};int rc=poll(&f,1,(int)ms);if(rc<0)return -errno;if(!rc)return 0;if(f.revents&(POLLERR|POLLHUP|POLLNVAL))return -EIO;return (f.revents&(writing?POLLOUT:POLLIN))?1:0;}
static ssize_t net_send(void*p,int fd,const uint8_t*b,size_t n){(void)p;ssize_t rc=send(fd,b,n,0);return rc<0?-errno:rc;}
static ssize_t net_receive(void*p,int fd,uint8_t*b,size_t n){(void)p;ssize_t rc=recv(fd,b,n,0);return rc<0?-errno:rc;}
static void net_close(void*p,int fd){(void)p;close(fd);}
int vv_roughtime_k7_fetch(const struct vv_platform_diag_authority*a,const struct vv_https_cancel*c,struct vv_trusted_time_sample*out){
 if(out)memset(out,0,sizeof(*out));
 if(!a||!out)return -EINVAL;
 if(!a->trng_development_ns_policy||a->trng_source_approved!=vv_k7_trng_development_approved||a->trng_source_arg||!vv_k7_trng_development_approved(0))return -EACCES;
 struct k7radio_link_snapshot link={0};int rc=k7radio_get_link_snapshot(&link);
 if(rc||!link.wifi_connected||!link.ipv4_ready)return -ENETDOWN;
 struct network n={0};
 /* Serial pre-factory use; same NULL cancel context as later factory init.
  * getaddrinfo obeys the existing compile-time DNS call bound; cancellation
  * is checked immediately after it returns, never by another worker thread. */
 rc=vv_dns_nuttx_backend_init(&n.dns,0);if(rc)return rc;
 struct vv_roughtime_io io={&n,net_now,net_nonce,net_resolve,net_open,net_wait,net_send,net_receive,net_close};
 return vv_roughtime_fetch_io(&io,c,out);
}
int vv_roughtime_k7_fetch_runtime(const struct vv_platform_diag_authority *a,
 struct vv_cloud_tcp *shared,const struct vv_https_cancel *c,struct vv_trusted_time_sample *out){
 if(out)memset(out,0,sizeof(*out));
 if(!a||!shared||!out)return -EINVAL;
 if(!a->trng_development_ns_policy||a->trng_source_approved!=vv_k7_trng_development_approved||
    a->trng_source_arg||!vv_k7_trng_development_approved(0))return -EACCES;
 struct k7radio_link_snapshot link={0};int rc=k7radio_get_link_snapshot(&link);
 if(rc||!link.wifi_connected||!link.ipv4_ready)return -ENETDOWN;
 struct network n={.shared=shared,.cancel=c};
 struct vv_roughtime_io io={&n,net_now,net_nonce,net_resolve,net_open,net_wait,net_send,net_receive,net_close};
 return vv_roughtime_fetch_io(&io,c,out);
}
#else
int vv_roughtime_k7_fetch_runtime(const struct vv_platform_diag_authority *a,
 struct vv_cloud_tcp *t,const struct vv_https_cancel *c,struct vv_trusted_time_sample *out){
 (void)a;(void)t;(void)c;if(out)memset(out,0,sizeof(*out));return -ENOSYS;
}
int vv_roughtime_k7_fetch(const struct vv_platform_diag_authority*a,const struct vv_https_cancel*c,struct vv_trusted_time_sample*out){(void)a;(void)c;if(out)memset(out,0,sizeof(*out));return -ENOSYS;}
#endif
