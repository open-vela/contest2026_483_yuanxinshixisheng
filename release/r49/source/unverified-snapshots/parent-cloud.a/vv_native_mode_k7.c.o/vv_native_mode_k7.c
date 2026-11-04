#include "vv_native_mode.h"
#include "k7_pipeline.h"
#include <unistd.h>
#include <time.h>
/* Caller cancellation callback is borrowed; no global context/second owner. */
static int request(void *arg,unsigned mode){(void)arg;return k7_pipeline_voice_mode(mode);}
static int status(void *arg,unsigned mode){(void)arg;return k7_pipeline_voice_mode_status(mode);}
static void halt(void *arg){(void)arg;k7_pipeline_halt();}
static void delay(void *arg,unsigned ms){(void)arg;usleep(ms*1000u);}
static uint64_t now(void *arg){(void)arg;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;}
int vv_native_mode_k7_init(struct vv_native_mode *s,void (*cancel)(void *),void (*quiet)(void *),void *arg){struct vv_native_mode_ops o={request,status,halt,cancel,quiet,delay,now,arg};return vv_native_mode_init(s,&o);}
