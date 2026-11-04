#include "vv_k7_platform_check.h"
#include "vv_cloud_platform_diag.h"
#include <errno.h>
#include <stdio.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#if defined(__NuttX__) && defined(CONFIG_ARCH_CHIP_RK3576) && defined(CONFIG_ARCH_BOARD_KICKPI_K7) && defined(CONFIG_RK3576_UART6) && defined(CONFIG_VELAVISION_CLOUD_PLATFORM_DIAG) && defined(CONFIG_VELAVISION_RTC_I2C2_PLATFORM) && defined(CONFIG_VELAVISION_LEGACY_RTC_STATUS_DISABLED)
static struct vv_platform_diag diagnostic;
static volatile int owner;
int vv_k7_platform_check(void){
 if(__sync_lock_test_and_set(&owner,1)){puts("K7CLOUD platform_check=busy");return 1;}
 struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||(uint64_t)t.tv_sec>(UINT64_MAX-2000)/1000){
  __sync_lock_release(&owner);puts("K7CLOUD platform_check=monotonic_unavailable");return 1;
 }
 uint64_t deadline=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000+1000;
 /* These two permissions follow this exact K7 build's mapped CRU/IOC and
  * DEVICE_REGION, existing board access, disabled legacy RTC command and
  * the new exclusive I2C2 owner. They do NOT grant TRNG data ownership or
  * trust RTC contents. No guessed secure-register access occurs. */
 const struct vv_platform_diag_authority a={
  .rtc_access_owner_verified=true,
  .rtc_trusted_provisioning_verified=false,
  .cru_mapping_access_verified=true,
  .trng_mapping_reset_parent_owner_verified=false,
  .trng_source_approved=0,.trng_source_arg=0
 };
 struct vv_platform_diag_result r;
 int rc=vv_cloud_platform_diag_once(&diagnostic,&a,deadline,&r);
 vv_cloud_platform_diag_print(&r);
 printf("K7CLOUD platform_check_result=%d cloud_ready=0 wall_clock_changed=0 trng_data_read=0\n",rc);
 __sync_lock_release(&owner);
 return (r.rtc_rc||r.trng_gate_rc)?1:0;
}
#else
int vv_k7_platform_check(void){puts("K7CLOUD platform_check=disabled missing=k7_mapping_rtc_owner_configuration");return 1;}
#endif
