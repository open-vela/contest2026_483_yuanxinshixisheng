#include "vv_trng_nuttx.h"
#include "vv_rk3576_trng.h"
#include <errno.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
static void wipe(uint8_t*p,size_t n){if(p){volatile uint8_t*q=p;while(n--)*q++=0;}}
#if defined(__NuttX__) && defined(CONFIG_VELAVISION_TRNG_NS_PLATFORM)
#include <nuttx/arch.h>
#include <time.h>
#define TRNG_NS ((uintptr_t)0x2a410000)
#define NS_GATE ((uintptr_t)0x27200c48)
static struct vv_rk3576_trng core;
static volatile int owner;
static bool approved;
static bool (*health)(void *);
static void *health_arg;
static bool development_reserved,configured_once;
static void *development_owner;
static bool permitted(void*p){
 (void)p;
 /* Check approval BEFORE touching CRU or TRNG; ready is not health. */
 if(!approved||!health||!health(health_arg))return false;
 return ((*(volatile const uint32_t *)NS_GATE)&(1u<<13))==0;
}
static int read32(void*p,uint32_t off,uint32_t*out){
 (void)p;if(!out)return -EINVAL;*out=0;
 if(off!=VV_RK3576_TRNG_STATE&&
    (off<VV_RK3576_TRNG_DATA||off>VV_RK3576_TRNG_DATA+28||(off&3)))return -EINVAL;
 if(!permitted(0))return -EACCES;
 *out=*(volatile const uint32_t *)(TRNG_NS+off);return 0;
}
static int write32(void*p,uint32_t off,uint32_t v){
 (void)p;
 /* Only raw TRNG bit4; reject DRNG control/data and arbitrary writes. */
 if(!((off==VV_RK3576_TRNG_CTRL&&v==0x00100010)||
      (off==VV_RK3576_TRNG_STATE&&v==0x10)))return -EINVAL;
 if(!permitted(0))return -EACCES;
 *(volatile uint32_t *)(TRNG_NS+off)=v;__asm__ volatile("dsb sy":::"memory");return 0;
}
static uint64_t now_us(void*p){struct timespec t;(void)p;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000)return UINT64_MAX;
 if((uint64_t)t.tv_sec>(UINT64_MAX-999999)/1000000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static int wait_us(void*p,uint32_t n){(void)p;if(n>100)return -EINVAL;up_udelay(n);return 0;}
int vv_trng_nuttx_configure(bool allow,bool(*health_fn)(void*),void*arg){
 if(__sync_lock_test_and_set(&owner,1))return -EBUSY;
 if(development_reserved){__sync_lock_release(&owner);return -EBUSY;}
 /* Explicit configure starts a new generation; never used as auto retry. */
 approved=false;vv_rk3576_trng_disable(&core);health=health_fn;health_arg=arg;
 int rc=-EACCES;
 if(allow&&health_fn){
  configured_once=true;
  approved=true;
  const struct vv_rk3576_trng_ops ops={read32,write32,now_us,wait_us,permitted,0};
  rc=vv_rk3576_trng_configure(&core,&ops);
  if(rc)approved=false;
 }
 __sync_lock_release(&owner);return rc;
}
ssize_t vv_trng_nuttx_read_exact(void*unused,uint8_t*out,size_t n,uint64_t d){
 (void)unused;
 if(!out||!n)return -EINVAL;
 if(__sync_lock_test_and_set(&owner,1)){wipe(out,n);return -EBUSY;}
 if(development_reserved){__sync_lock_release(&owner);wipe(out,n);return -EACCES;}
 ssize_t rc=vv_rk3576_trng_read_exact(&core,out,n,d);
 if(rc<0)approved=false;
 __sync_lock_release(&owner);return rc;
}
int vv_trng_nuttx_disable(void){
 if(__sync_lock_test_and_set(&owner,1))return -EBUSY;
 if(development_reserved){__sync_lock_release(&owner);return -EBUSY;}
 approved=false;vv_rk3576_trng_disable(&core);health=0;health_arg=0;
 __sync_lock_release(&owner);return 0;
}
int vv_trng_nuttx_development_configure(bool(*admitted)(void*),void*policy){
#ifdef CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT
 if(!admitted||!policy)return -EINVAL;
 if(__sync_lock_test_and_set(&owner,1))return -EBUSY;
 if(configured_once||development_reserved){__sync_lock_release(&owner);return -EBUSY;}
 /* Reservation remains even after failure: no cross-mode retry this boot. */
 development_reserved=true;development_owner=policy;configured_once=true;
 health=admitted;health_arg=policy;approved=true;
 const struct vv_rk3576_trng_ops ops={read32,write32,now_us,wait_us,permitted,0};
 int rc=vv_rk3576_trng_configure(&core,&ops);if(rc)approved=false;
 __sync_lock_release(&owner);return rc;
#else
 (void)admitted;(void)policy;return -ENOSYS;
#endif
}
ssize_t vv_trng_nuttx_development_read_exact(void*policy,uint8_t*out,size_t n,uint64_t d){
 if(!out||!n)return -EINVAL;
 if(__sync_lock_test_and_set(&owner,1)){wipe(out,n);return -EBUSY;}
 if(!development_reserved||!policy||policy!=development_owner){
  __sync_lock_release(&owner);wipe(out,n);return -EACCES;
 }
 ssize_t rc=vv_rk3576_trng_read_exact(&core,out,n,d);if(rc<0)approved=false;
 __sync_lock_release(&owner);return rc;
}
#else
int vv_trng_nuttx_configure(bool allow,bool(*health)(void*),void*arg){(void)allow;(void)health;(void)arg;return -ENOSYS;}
ssize_t vv_trng_nuttx_read_exact(void*p,uint8_t*out,size_t n,uint64_t d){(void)p;(void)d;wipe(out,n);return -ENOSYS;}
int vv_trng_nuttx_disable(void){return 0;}
int vv_trng_nuttx_development_configure(bool(*admitted)(void*),void*p){(void)admitted;(void)p;return -ENOSYS;}
ssize_t vv_trng_nuttx_development_read_exact(void*p,uint8_t*b,size_t n,uint64_t d){(void)p;(void)d;wipe(b,n);return -ENOSYS;}
#endif
