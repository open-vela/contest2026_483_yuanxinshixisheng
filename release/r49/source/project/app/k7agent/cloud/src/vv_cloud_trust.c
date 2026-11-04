#include "vv_cloud_trust.h"
#include <errno.h>
#include <string.h>

void vv_cloud_trust_revoke(struct vv_cloud_trust *s) {
  if (s) memset(s, 0, sizeof(*s));
}
int vv_cloud_trust_arm(struct vv_cloud_trust *s, uint64_t mono,
                       uint64_t utc, uint64_t age, uint64_t drift,
                       bool entropy, bool time, bool ca) {
  if (!s) return -EINVAL;
  vv_cloud_trust_revoke(s);
  if (!utc || !age || !entropy || !time || !ca) return -EINVAL;
  s->anchor_mono_ms = s->last_mono_ms = mono;
  s->anchor_utc_s = s->last_utc_s = utc;
  s->max_age_ms = age;
  s->max_drift_s = drift;
  s->armed = true;
  return 0;
}
bool vv_cloud_trust_check(struct vv_cloud_trust *s, uint64_t mono,
                         uint64_t utc, bool entropy, bool ca) {
  if (!s || !s->armed) return false;
  if (!entropy || !ca || mono < s->last_mono_ms ||
      utc < s->last_utc_s || mono - s->anchor_mono_ms >= s->max_age_ms)
    goto revoked;
  uint64_t delta = (mono - s->anchor_mono_ms) / 1000;
  if (s->anchor_utc_s > UINT64_MAX - delta) goto revoked;
  uint64_t expected = s->anchor_utc_s + delta;
  uint64_t difference = utc > expected ? utc - expected : expected - utc;
  if (difference > s->max_drift_s) goto revoked;
  s->last_mono_ms = mono;
  s->last_utc_s = utc;
  return true;
revoked:
  vv_cloud_trust_revoke(s);
  return false;
}
