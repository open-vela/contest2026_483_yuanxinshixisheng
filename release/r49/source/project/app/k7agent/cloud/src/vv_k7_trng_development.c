#include "vv_k7_trng_development.h"
#include "vv_trng_nuttx.h"
#include <errno.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#ifdef CONFIG_ARCH_CHIP_RK3576
/* Same exported chip header used by rk3576_boot.c for DEVICE_REGION. */
#include <arch/chip/chip.h>
#endif
#endif
/* New explicit option is intentionally independent of the production fact
 * assertion. DEVICE_REGION maps the complete NS window; UART6 startup maps
 * CRU. Actual NS firewall/secure-world arbitration remains a development
 * source-policy assumption supported by the frozen upstream Linux driver. */
#if defined(__NuttX__) && defined(CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT) && defined(CONFIG_VELAVISION_TRNG_NS_PLATFORM) && defined(CONFIG_ARCH_CHIP_RK3576) && defined(CONFIG_ARCH_BOARD_KICKPI_K7) && defined(CONFIG_RK3576_UART6) && defined(CONFIG_DEVICEIO_BASEADDR) && defined(CONFIG_DEVICEIO_SIZE)
#include <time.h>
#if CONFIG_DEVICEIO_BASEADDR > 0x2a410000 || CONFIG_DEVICEIO_SIZE < 0x200 || CONFIG_DEVICEIO_BASEADDR + CONFIG_DEVICEIO_SIZE < 0x2a410200
#error "TRNG development requires the complete fixed NS window mapped"
#endif
static struct vv_trng_development_policy policy;
/* Static process-lifetime generation resets only with this flat kernel boot;
 * no persisted policy object or generation can be imported from credentials. */
static const uint64_t boot_generation=1;
static bool context(void*a,uint64_t generation){
 (void)a;
 return generation==boot_generation&&
  ((*(volatile const uint32_t *)(uintptr_t)0x27200c48)&(1u<<13))==0;
}
static uint64_t now(void*a){
 struct timespec t;(void)a;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||
    (uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static int begin(void*a,bool(*admitted)(void*),void*p){(void)a;return vv_trng_nuttx_development_configure(admitted,p);}
static ssize_t read_raw(void*a,uint8_t*b,size_t n,uint64_t d){(void)a;return vv_trng_nuttx_development_read_exact(&policy,b,n,d);}
int vv_k7_trng_development_probe(void){
 const struct vv_trng_development_ops ops={context,now,begin,read_raw,0};
 uint64_t t=now(0);if(t>UINT64_MAX-100)return -EIO;
 return vv_trng_development_probe(&policy,&ops,boot_generation,t+100);
}
bool vv_k7_trng_development_approved(void*a){(void)a;return vv_trng_development_approved(&policy);}
ssize_t vv_k7_trng_development_read(void*a,uint8_t*b,size_t n,uint64_t d){(void)a;return vv_trng_development_read(&policy,b,n,d);}
void vv_k7_trng_development_revoke(void){vv_trng_development_revoke(&policy);}
int vv_k7_trng_development_state(void){return atomic_load(&policy.state);}
int vv_k7_trng_development_snapshot(struct vv_trng_development_lease*out){return vv_trng_development_snapshot(&policy,out);}
int vv_k7_trng_development_renew(uint64_t g,uint64_t d){return vv_trng_development_renew(&policy,g,d);}
#else
int vv_k7_trng_development_probe(void){return -ENOSYS;}
bool vv_k7_trng_development_approved(void*a){(void)a;return false;}
ssize_t vv_k7_trng_development_read(void*a,uint8_t*b,size_t n,uint64_t d){
 (void)a;(void)d;if(b){volatile uint8_t*p=b;while(n--)*p++=0;}return -ENOSYS;
}
void vv_k7_trng_development_revoke(void){}
int vv_k7_trng_development_state(void){return VV_TD_UNTRIED;}
int vv_k7_trng_development_snapshot(struct vv_trng_development_lease*out){(void)out;return -ENOSYS;}
int vv_k7_trng_development_renew(uint64_t g,uint64_t d){(void)g;(void)d;return -ENOSYS;}
#endif
