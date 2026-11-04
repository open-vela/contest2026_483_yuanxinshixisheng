#ifndef VV_NEAR_EXPERIMENT_H
#define VV_NEAR_EXPERIMENT_H
#include "near_driver.h"
#include "echo_observer.h"
struct vv_near_experiment;
struct vv_near_experiment_policy {
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 struct k7_apm21_nominal_identity apm_nominal; /* kind0 retains legacy lag39 */
#endif
 bool enabled; /* explicit controlled experiment permission, not calibration */
 uint64_t revision;
 uint8_t capture_sha256[32];
 unsigned lag_samples;
 float probability;
 unsigned consecutive_windows;
 uint64_t warmup_samples,maximum_samples;
};
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
void vv_near_experiment_apm_nominal_policy(struct vv_near_experiment_policy *);
#endif
int vv_near_experiment_create(const struct vv_near_experiment_policy *,
 int (*observe)(void *,struct vv_barge_observation *),void *,struct vv_near_experiment **);
void vv_near_experiment_export(struct vv_near_experiment *,struct vv_barge_driver *);
int vv_near_experiment_destroy(struct vv_near_experiment *);
#endif

#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
int vv_near_experiment_rearm(struct vv_near_experiment *,int(*)(void*),void*);
#endif
