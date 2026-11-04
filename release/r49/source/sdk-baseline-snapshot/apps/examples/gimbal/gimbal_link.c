/* SPDX-License-Identifier: Apache-2.0 */
#include "gimbal_link.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#ifndef GIMBAL_PORT
#define GIMBAL_PORT "/dev/ttyS1"
#endif
static pthread_mutex_t g_link_lock=PTHREAD_MUTEX_INITIALIZER;
static int g_fd=-1,g_last_x,g_last_y;
static bool g_failed;
static uint64_t ms(void)
{
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int fail(int e) {errno=e;return -e;}
int gimbal_link_adopt(int x,int y)
{
  if(x < -800 || x > 800 || y < -120 || y > 1030) return fail(EINVAL);
  int ret=pthread_mutex_trylock(&g_link_lock);
  if(ret)return fail(ret);
  g_last_x=x;g_last_y=y;
  pthread_mutex_unlock(&g_link_lock);
  return 0;
}
int gimbal_link_pack(uint8_t data[14], int x, int y)
{
  /* Extended wire range is for manually stepped calibration. The tracker
   * and legacy set command retain their own existing bounds.
   */
  if (!data || x < -800 || x > 800 || y < -200 || y > 1030)
    return fail(EINVAL);
  for(int i=0;i<2;i++)
    {
      uint16_t v=(uint16_t)(int16_t)(i ? y:x);
      uint8_t *p=data+i*7;
      p[0]=0x55;p[1]=0xaa;p[2]=i?0xff:0;p[3]=v;p[4]=v>>8;p[5]=0;p[6]=0xfa;
    }
  return 0;
}
int gimbal_link_open(int *x,int *y)
{
  if(!x||!y)return fail(EINVAL);
  int ret=pthread_mutex_trylock(&g_link_lock);
  if(ret)return fail(ret);
  int fd=open(GIMBAL_PORT,O_RDWR|O_NONBLOCK);
  if(fd<0){ret=errno;pthread_mutex_unlock(&g_link_lock);return fail(ret);}
  g_fd=fd;g_failed=false;*x=g_last_x;*y=g_last_y;
  return fd;
}
int gimbal_link_send(int fd,int x,int y)
{
  uint8_t data[14];
  if(fd<0||fd!=g_fd)return fail(EBADF);
  if(g_failed)return fail(EIO);
  int ret=gimbal_link_pack(data,x,y);
  if(ret<0)return ret;
  size_t offset=0;uint64_t deadline=ms()+50;
  while(offset<sizeof(data))
    {
      if(ms()>=deadline){ret=ETIMEDOUT;goto fault;}
      ssize_t n=write(fd,data+offset,sizeof(data)-offset);
      if(n>0){offset+=n;continue;}
      if(n<0&&errno!=EAGAIN&&errno!=EINTR){ret=errno;goto fault;}
      struct pollfd p={fd,POLLOUT,0};
      int ready=poll(&p,1,2);
      if(ready<0&&errno!=EINTR){ret=errno;goto fault;}
      if(p.revents&(POLLERR|POLLHUP|POLLNVAL)){ret=EIO;goto fault;}
    }
  /* Last successfully queued command, NOT measured position or MCU ACK. */
  g_last_x=x;g_last_y=y;
  return 0;
fault:
  g_failed=true;
  return fail(ret);
}
void gimbal_link_close(int fd)
{
  /* Same parent task opens/closes; its worker may send until joined. */
  if(fd>=0&&fd==g_fd)
    {close(fd);g_fd=-1;pthread_mutex_unlock(&g_link_lock);}
}
