#ifndef VV_CLOUD_PLATFORM_DIAG_H
#define VV_CLOUD_PLATFORM_DIAG_H
#include "vv_rtc_reader.h"
#include "vv_trng_nuttx.h"
#include "vv_rtc_provision.h"
/* Caller must authenticate the source before authorized=true. This is not an
 * authentication API. Fresh sample is consumed before the factory's first
 * RTC access; only successful RTC initialization AND readback establish the
 * current-boot time fact. received_mono_ms uses CLOCK_MONOTONIC milliseconds. */
struct vv_trusted_time_sample {
 bool present,authorized;
 uint64_t utc_seconds,received_mono_ms;
 bool (*cancelled)(void *);void *cancel_arg;
};
int vv_trusted_time_initialize(struct vv_rtc_provision *,
 const struct vv_trusted_time_sample *,uint64_t deadline_ms,
 struct vv_rtc_provision_result *);
enum vv_platform_missing {
 VV_PD_RTC_ACCESS=1u, VV_PD_RTC_VALID=2u, VV_PD_RTC_TRUST=4u,
 VV_PD_CRU_ACCESS=8u, VV_PD_TRNG_GATE=16u, VV_PD_TRNG_OWNERSHIP=32u,
 VV_PD_TRNG_HEALTH=64u
};
struct vv_platform_diag_authority {
 /* These are explicit verified platform facts, never CLI defaults. */
 bool rtc_access_owner_verified;
 bool rtc_trusted_provisioning_verified;
 bool cru_mapping_access_verified;
 bool trng_mapping_reset_parent_owner_verified;
 /* Pure bounded policy query: no hardware access, no fake always-true. */
 bool (*trng_source_approved)(void *);
 void *trng_source_arg;
 /* Explicit development Linux NS-source policy, separate from the strong
  * platform fact assertion above. Requires successful fixed-board one-shot
  * probe in this boot; never inferred from a credential or leaf gate alone.
  * source callback must be the fixed board development approval query. */
 bool trng_development_ns_policy;
};
struct vv_platform_diag_result {int rtc_rc,trng_gate_rc;uint64_t rtc_utc_candidate;unsigned missing;bool trusted_time_initialized;};
struct vv_platform_diag {bool attempted;struct vv_rtc_reader rtc;};
/* One explicit attempt per zero-initialized instance. RTC singleton may only
 * bind once system-wide. No implicit retries/reinitialization or TLS arming.
 * RTC branch may write its own clock/pins only under explicit authority.
 * TRNG branch reads only CRU gate, never raw random state/data or control. */
int vv_cloud_platform_diag_once(struct vv_platform_diag *,
 const struct vv_platform_diag_authority *,uint64_t deadline_ms,
 struct vv_platform_diag_result *);
int vv_cloud_platform_diag_with_trusted_time_once(struct vv_platform_diag *,
 const struct vv_platform_diag_authority *,const struct vv_trusted_time_sample *,
 uint64_t deadline_ms,struct vv_platform_diag_result *);
void vv_cloud_platform_diag_print(const struct vv_platform_diag_result *);
/* Separate explicit entropy connection. No read/seed here. Verifies current
 * CRU gate and external source approval, then configures native provider.
 * Output uses vv_cloud_entropy read_exact callback contract. */
int vv_cloud_platform_diag_bind_entropy(const struct vv_platform_diag_authority *,
 ssize_t (**read_exact)(void *,uint8_t *,size_t,uint64_t));
#endif
