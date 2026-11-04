#include "k7boot_aliyun_production_authority.h"
#include <errno.h>
#include <string.h>
#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
#include "k7boot_cloud_business_root.h"
#else
#include "k7boot_aliyun_root.h"
#endif
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT
#include "../k7agent/cloud/include/vv_k7_trng_development.h"
#endif
const unsigned char*k7boot_aliyun_production_ca(size_t*length){
 if(length)*length=sizeof(k7boot_aliyun_root_pem);
 return k7boot_aliyun_root_pem;
}
int k7boot_aliyun_development_trng_authority(struct vv_platform_diag_authority*a){
 if(!a)return -EINVAL;
 a->trng_mapping_reset_parent_owner_verified=false;
 a->trng_development_ns_policy=false;
 a->trng_source_approved=0;a->trng_source_arg=0;
#ifdef CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT
 if(!vv_k7_trng_development_approved(0))return -EACCES;
 a->trng_development_ns_policy=true;
 a->trng_source_approved=vv_k7_trng_development_approved;
 return 0;
#else
 return -ENOSYS;
#endif
}
int k7boot_aliyun_production_authority(struct vv_platform_diag_authority *a,bool *ca){
 if(a)memset(a,0,sizeof(*a));
 if(ca)*ca=false;
 if(!a||!ca)return -EINVAL;
#if defined(__NuttX__) && defined(CONFIG_ARCH_CHIP_RK3576) && defined(CONFIG_ARCH_BOARD_KICKPI_K7) && defined(CONFIG_RK3576_UART6) && defined(CONFIG_VELAVISION_K7_PLATFORM_FACTORY) && defined(CONFIG_VELAVISION_RTC_I2C2_PLATFORM) && defined(CONFIG_VELAVISION_LEGACY_RTC_STATUS_DISABLED) && defined(CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT)
 /* Fixed board mapping and exclusive RTC wrapper, confirmed by candidate05
  * RTC_PREPARE rc=0. Factory alone consumes fresh RTC initialization/read;
  * calling native-platform-check here would consume its singleton twice. */
 a->rtc_access_owner_verified=true;
 a->cru_mapping_access_verified=true;
 /* This board's development deployment has explicit UTC source evidence:
  * evidence/cloud-direct-20261014/ram07-rtc-init/result.json set 1791970255
  * (host 1791970260.5125). ram08-platform-check/result.json read 1791970851
  * after software reset (host 1791970856.5989): RTC +596s, host +596.0864s.
  * ram08-rtc-raw/result.json independently retained valid calendar bytes.
  * Factory still reads and rejects VL/STOP/invalid time on every fresh boot.
  * This source approval does NOT claim cold-power/battery retention. */
 a->rtc_trusted_provisioning_verified=true;
 /* Independent normal-world NS policy: successful prior boot observations
  * never substitute for this boot's probe. A failure is not retried. */
 if(vv_k7_trng_development_state()==VV_TD_UNTRIED){
  int rc=vv_k7_trng_development_probe();if(rc)return rc;
 }
 int rc=k7boot_aliyun_development_trng_authority(a);if(rc)return rc;
 /* Public PEM bundle pinned in k7boot_aliyun_root.h: original R46 provenance in
  * evidence/cloud-direct-20261014/aliyun-root-provenance.json, plus the issuer-
  * published R3 in evidence/speech19-20261019/workspace-root-r3-provenance.json.
  * This is CA
  * source approval, not a claim that board TLS has already succeeded. */
 *ca=true;
 return 0;
#else
 return -EACCES;
#endif
}
