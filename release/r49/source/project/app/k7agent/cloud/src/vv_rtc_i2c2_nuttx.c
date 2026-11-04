#include "vv_rtc_i2c2.h"
#include "vv_rtc_provision_i2c2.h"
#include <errno.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#if defined(CONFIG_VELAVISION_RTC_I2C2_PLATFORM) && !defined(CONFIG_VELAVISION_LEGACY_RTC_STATUS_DISABLED)
#error "Disable or route legacy rtc-status before enabling I2C2 RTC platform"
#endif
#if defined(__NuttX__) && defined(CONFIG_VELAVISION_RTC_I2C2_PLATFORM) && defined(CONFIG_VELAVISION_LEGACY_RTC_STATUS_DISABLED)
#include <nuttx/arch.h>
#include <time.h>
#include <stdio.h>
#include <inttypes.h>
static struct vv_rtc_i2c2 singleton;
int vv_rtc_i2c2_platform_provision_ops(struct vv_rtc_provision_ops*out){
 if(!singleton.approved||singleton.poisoned)return -EACCES;
 return vv_rtc_provision_i2c2_bind(&singleton,out);
}
static uint32_t hw_read(void*p,uintptr_t a){(void)p;return *(volatile const uint32_t *)a;}
static void hw_write(void*p,uintptr_t a,uint32_t v){(void)p;*(volatile uint32_t *)a=v;__asm__ volatile("dsb sy":::"memory");}
static uint64_t hw_now(void*p){struct timespec t;(void)p;if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000)return UINT64_MAX;if((uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static void hw_wait(void*p,unsigned n){(void)p;up_udelay(n);}
int vv_rtc_i2c2_platform_prepare(bool approved,uint64_t deadline){
 int rc=vv_rtc_i2c2_prepare(&singleton,approved,deadline);
 /* Startup caller serializes prepare; logging reads cached data only. */
 const struct vv_rtc_i2c2_diag*q=&singleton.diag;
 printf("RTC_PREPARE rc=%d phase=%u blocked=0x%x controller_valid=%u owner=%d poisoned=%u\n",
        rc,q->phase,q->blocked,(unsigned)q->controller_valid,singleton.owner,(unsigned)singleton.poisoned);
 fflush(stdout);up_udelay(5000);
 printf("RTC_PREFLIGHT root_sel=%08" PRIx32 " root_gate=%08" PRIx32 " bus_gate=%08" PRIx32 "\n",
        q->root_select,q->root_gate,q->bus_gate);
 fflush(stdout);up_udelay(5000);
 printf("RTC_PINS reset=%08" PRIx32 " scl=%08" PRIx32 " sda=%08" PRIx32 "\n",
        q->reset,q->scl,q->sda);
 fflush(stdout);up_udelay(5000);
 printf("RTC_CONTROLLER pclk=%08" PRIx32 " con=%08" PRIx32 " ien=%08" PRIx32 "\n",q->pclk,q->con,q->ien);
 fflush(stdout);up_udelay(5000);
 return rc;
}
int vv_rtc_i2c2_platform_ops(bool approved,struct vv_rtc_reader_ops*out){
 /* Call once at startup before publishing ops; no poison reset/reinit. */
 if(singleton.io.read32)return -EALREADY;
 singleton.io=(struct vv_rtc_i2c2_io){hw_read,hw_write,hw_now,hw_wait,0,0};singleton.approved=approved;
 return vv_rtc_i2c2_bind(&singleton,out);
}
#else
int vv_rtc_i2c2_platform_provision_ops(struct vv_rtc_provision_ops*out){(void)out;return -ENOSYS;}
int vv_rtc_i2c2_platform_prepare(bool approved,uint64_t deadline){(void)approved;(void)deadline;return -ENOSYS;}
int vv_rtc_i2c2_platform_ops(bool approved,struct vv_rtc_reader_ops*out){(void)approved;(void)out;return -ENOSYS;}
#endif
