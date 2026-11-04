#include "vv_http_development.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
static ssize_t failed_io(struct vv_http_development *t,bool writing,int stage,
 int result,int saved_errno,int revents) {
 int prior_errno=errno;
 int socket_error=0;socklen_t size=sizeof(socket_error);
 int rc=getsockopt(t->fd,SOL_SOCKET,SO_ERROR,&socket_error,&size);
 int option_errno=rc<0?errno:0;
 printf("LAN_IO writing=%u stage=%d rc=%d os_errno=%d revents=%d so_rc=%d so_errno=%d so_error=%d\n",
  (unsigned)writing,stage,result,saved_errno,revents,rc,option_errno,socket_error);
 fflush(stdout);errno=prior_errno;return result;
}
static int gate(struct vv_http_development *t,uint64_t deadline,
 const struct vv_https_cancel *cancel) {
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 return t->config.now_ms(t->config.now_arg)>=deadline?-ETIMEDOUT:0;
}
void vv_http_development_close(struct vv_http_development *t) {
 if(t&&t->fd>=0){int fd=t->fd;t->fd=-1;(void)close(fd);}
}
static void finish(void *ctx){vv_http_development_close(ctx);}
static int connect_to(void *ctx,const char *host,uint16_t port,uint64_t deadline,
 const struct vv_https_cancel *cancel,struct vv_https_peer_security *security) {
 struct vv_http_development *t=ctx;
 if(security)memset(security,0,sizeof(*security));
 if(!t||!host||strcmp(host,"10.3.3.170")||port!=18085||
    !t->config.tcp_connect||!t->config.now_ms)return -EINVAL;
 vv_http_development_close(t);
 int rc=gate(t,deadline,cancel);if(rc)return rc;
 t->fd=t->config.tcp_connect(t->config.tcp_arg,host,port,deadline,cancel);
 if(t->fd<0)return t->fd;
 rc=gate(t,deadline,cancel);if(rc)vv_http_development_close(t);
 return rc;
}
static ssize_t transfer(struct vv_http_development *t,void *data,size_t len,
 uint64_t deadline,const struct vv_https_cancel *cancel,bool writing) {
 if(!t||t->fd<0)return -EBADF;
 for(;;){
  int rc=gate(t,deadline,cancel);if(rc)return rc;
  ssize_t n=writing?send(t->fd,data,len,0):recv(t->fd,data,len,0);
  if(n>=0){if(!n&&writing)return failed_io(t,writing,1,0,0,0);return n;}
  int error=errno;if(error==EINTR)continue;
  if(error!=EAGAIN&&error!=EWOULDBLOCK)return failed_io(t,writing,1,-error,error,0);
  uint64_t now=t->config.now_ms(t->config.now_arg);
  if(now>=deadline)return -ETIMEDOUT;
  unsigned wait=deadline-now>50?50:(unsigned)(deadline-now);
  struct pollfd p={t->fd,writing?POLLOUT:POLLIN,0};
  rc=poll(&p,1,(int)wait);
  if(rc<0){int e=errno;if(e==EINTR)continue;return failed_io(t,writing,2,-e,e,p.revents);}
  if(p.revents&POLLNVAL)return failed_io(t,writing,3,-EBADF,0,p.revents);
  if(p.revents&POLLERR)return failed_io(t,writing,3,-EIO,0,p.revents);
  /* POLLIN+HUP can still contain response bytes; recv drains them. */
  if((p.revents&POLLHUP)&&!(p.revents&POLLIN))return writing?failed_io(t,writing,3,-EPIPE,0,p.revents):0;
 }
}
static ssize_t write_bytes(void *ctx,const void *data,size_t len,uint64_t deadline,
 const struct vv_https_cancel *cancel){return transfer(ctx,(void *)data,len,deadline,cancel,true);}
static ssize_t read_bytes(void *ctx,void *data,size_t len,uint64_t deadline,
 const struct vv_https_cancel *cancel){return transfer(ctx,data,len,deadline,cancel,false);}
const struct vv_https_transport_ops *vv_http_development_ops(void){
 static const struct vv_https_transport_ops ops={connect_to,write_bytes,read_bytes,finish};return &ops;
}
