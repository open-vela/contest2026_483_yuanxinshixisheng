#ifndef VV_CLOUD_TRUST_H
#define VV_CLOUD_TRUST_H
#include <stdbool.h>
#include <stdint.h>

/* Single-owner trust policy, not a time/entropy source. Only a platform whose
 * provisioning has authenticated the time and approved the CA/entropy source
 * may arm it. Never arm from plain NTP, compiler date or an arbitrary boolean
 * from a remote request. Its wall clock must be the same clock TLS uses. */
struct vv_cloud_trust {
  uint64_t anchor_mono_ms, last_mono_ms, anchor_utc_s, last_utc_s;
  uint64_t max_age_ms, max_drift_s;
  bool armed;
};
int vv_cloud_trust_arm(struct vv_cloud_trust *, uint64_t mono_ms,
                       uint64_t utc_s, uint64_t max_age_ms,
                       uint64_t max_drift_s, bool entropy_approved,
                       bool time_authenticated, bool ca_approved);
bool vv_cloud_trust_check(struct vv_cloud_trust *, uint64_t mono_ms,
                         uint64_t utc_s, bool entropy_healthy,
                         bool ca_still_valid);
void vv_cloud_trust_revoke(struct vv_cloud_trust *);
#endif
