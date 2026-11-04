/* SPDX-License-Identifier: Apache-2.0 */
#include "includes.h"
#include "common.h"
#include "eloop.h"
#include <time.h>
#include "skw_supplicant.h"

int (*skw_supplicant_random)(uint8_t *,size_t);
int skw_supplicant_timer_error;
int os_get_random(unsigned char *buf,size_t len)
{
  if(!buf || !skw_supplicant_random)return -1;
  int ret=skw_supplicant_random(buf,len);
  if(ret)skw_wpa_wipe(buf,len);
  return ret? -1:0;
}
void *os_zalloc(size_t size){return calloc(1,size);}
void *os_memdup(const void *src,size_t len)
{void *p=malloc(len);if(p)memcpy(p,src,len);return p;}
int os_get_reltime(struct os_reltime *out)
{
  struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return -1;
  out->sec=ts.tv_sec;out->usec=ts.tv_nsec/1000;return 0;
}
int os_get_time(struct os_time *out)
{
  struct timespec ts;if(clock_gettime(CLOCK_REALTIME,&ts))return -1;
  out->sec=ts.tv_sec;out->usec=ts.tv_nsec/1000;return 0;
}
int os_memcmp_const(const void *a,const void *b,size_t n)
{
  const volatile uint8_t *x=a,*y=b;uint8_t different=0;
  for(size_t i=0;i<n;i++)different|=x[i]^y[i];
  return different;
}
size_t os_strlcpy(char *out,const char *in,size_t cap)
{
  size_t n=strlen(in);if(cap){size_t copy=n<cap-1?n:cap-1;memcpy(out,in,copy);out[copy]=0;}return n;
}
struct timer_entry {uint64_t due;eloop_timeout_handler fn;void *ctx,*arg;};
static struct timer_entry timers[16];
static uint64_t monotonic_us(void)
{
  struct os_reltime t;if(os_get_reltime(&t)){skw_supplicant_timer_error=-EIO;return 0;}
  return (uint64_t)t.sec*1000000+t.usec;
}
int eloop_register_timeout(unsigned int sec,unsigned int usec,
  eloop_timeout_handler fn,void *ctx,void *arg)
{
  for(unsigned int i=0;i<16;i++)if(!timers[i].fn)
    {timers[i]=(struct timer_entry){monotonic_us()+(uint64_t)sec*1000000+usec,fn,ctx,arg};return 0;}
  skw_supplicant_timer_error=-ENOSPC;return -1;
}
int eloop_cancel_timeout(eloop_timeout_handler fn,void *ctx,void *arg)
{
  int count=0;for(unsigned int i=0;i<16;i++)
    if(timers[i].fn==fn && (ctx==ELOOP_ALL_CTX || timers[i].ctx==ctx) &&
       (arg==ELOOP_ALL_CTX || timers[i].arg==arg))
      {memset(&timers[i],0,sizeof(timers[i]));count++;}
  return count;
}
void skw_supplicant_timers(void)
{
  uint64_t now=monotonic_us();
  for(unsigned int i=0;i<16;i++)if(timers[i].fn && now>=timers[i].due)
    {struct timer_entry timer=timers[i];memset(&timers[i],0,sizeof(timers[i]));timer.fn(timer.ctx,timer.arg);}
}
