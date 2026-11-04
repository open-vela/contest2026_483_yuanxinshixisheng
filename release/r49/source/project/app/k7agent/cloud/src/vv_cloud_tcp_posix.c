#include "vv_cloud_tcp.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
static int open_fd(void *arg)
{ int fd; (void)arg; fd=socket(AF_INET,SOCK_STREAM,0); return fd<0 ? -errno:fd; }
static int nonblock(void *arg,int fd)
{ int flags; (void)arg; flags=fcntl(fd,F_GETFL,0); if(flags<0)return -errno;
  return fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 ? -errno:0; }
static int connect_fd(void *arg,int fd,uint32_t ip,uint16_t port)
{ struct sockaddr_in sa; (void)arg; memset(&sa,0,sizeof(sa));
  sa.sin_family=AF_INET; sa.sin_addr.s_addr=ip; sa.sin_port=htons(port);
  return connect(fd,(struct sockaddr *)&sa,sizeof(sa))<0 ? -errno:0; }
static int wait_fd(void *arg,int fd,unsigned ms)
{ struct pollfd p={fd,POLLOUT,0}; int rc; (void)arg; rc=poll(&p,1,(int)ms);
  if(rc<0)return -errno;
  if(!rc)return 0;
  if(p.revents&POLLNVAL)return -EBADF;
  return p.revents&(POLLOUT|POLLERR|POLLHUP) ? 1:-EIO; }
static int error_fd(void *arg,int fd)
{ int error=0; socklen_t size=sizeof(error); (void)arg;
  if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)return -errno;
  return size!=sizeof(error) ? -EIO : (error ? -error:0); }
/* Never retry close(EINTR): ownership is released and fd reuse is possible. */
static void close_fd(void *arg,int fd) { (void)arg; (void)close(fd); }
const struct vv_cloud_tcp_ops *vv_cloud_tcp_posix_ops(void)
{ static const struct vv_cloud_tcp_ops ops={open_fd,nonblock,connect_fd,
    wait_fd,error_fd,close_fd}; return &ops; }
