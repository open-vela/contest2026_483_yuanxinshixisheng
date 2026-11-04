#ifndef VV_K7_PLATFORM_FACTORY_H
#define VV_K7_PLATFORM_FACTORY_H
#include "vv_native_platform.h"
#include "vv_cloud_platform_diag.h"
struct vv_k7_platform_boot_config {
 struct vv_platform_diag_authority authority;
 const unsigned char *ca_pem;size_t ca_pem_length;bool ca_approved;
 /* Existing real pipeline callbacks/limits. Borrowed for process lifetime.
  * Factory replaces TLS/UTC/network, preserves business owner/metadata. */
 struct vv_native_platform_config business;
 struct vv_trusted_time_sample trusted_time;
 /* Explicit opt-in; requires TRUST_RENEWAL + ROUGHTIME build support and
  * resident scoped service safe points. Default false is the original lease. */
 bool renew_trusted_time;
 /* Explicit same-boot fixed development-source lease maintenance only. */
 bool renew_entropy_lease;
};
struct vv_k7_secure_login {char credential[1024],version[128],proof[1024];};
typedef int (*vv_k7_secure_load_fn)(void *,struct vv_k7_secure_login *);
/* One process-lifetime factory, owns singleton resolver and platform sources.
 * Explicit caller authority required. On success borrowed cfg stays alive;
 * no recording, cloud login or service start is implicit. On error output is
 * zeroed, missing identifies blockers. No automatic hardware retry. */
int vv_k7_platform_factory_prepare(const struct vv_k7_platform_boot_config *,
 uint64_t deadline_ms,struct vv_native_platform_config *,unsigned *missing);
int vv_k7_platform_factory_login(struct vv_native_platform *,vv_k7_secure_load_fn,
 void *,const struct vv_https_cancel *);
#endif
