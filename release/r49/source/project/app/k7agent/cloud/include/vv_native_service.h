#ifndef VV_NATIVE_SERVICE_H
#define VV_NATIVE_SERVICE_H
#include "vv_native_runtime.h"
/* Platform reports missing prerequisites; zero only with fresh verified state.
 * This API does not store or print credentials. Check must be bounded/nonblocking. */
enum vv_native_missing { VV_NATIVE_CONFIG=1u,VV_NATIVE_NETWORK=2u,VV_NATIVE_TIME=4u,
 VV_NATIVE_CA=8u,VV_NATIVE_ENTROPY=16u,VV_NATIVE_ENDPOINT=32u,VV_NATIVE_AUTH=64u,
 VV_NATIVE_PREVIEW=128u,VV_NATIVE_OWNER=256u,VV_NATIVE_METADATA=512u,VV_NATIVE_BUSINESS_AUTH=1024u };
struct vv_native_service_config {
 struct vv_native_runtime *runtime;
 unsigned (*missing)(void *);
 /* Claim shared legacy/native/audio exclusion; fail if another owner exists. */
 int (*claim)(void *);void (*release)(void *);
 /* Stable request strings remain owned until release_request. No placeholder
  * consent or automatically reused idempotency keys are allowed. Factory must
  * persist recovery metadata before returning; release only frees borrowed
  * memory and must not erase persistent keys or RAM photos. */
 int (*make_request)(void *,struct vv_assessment_request *);
 void (*release_request)(void *,struct vv_assessment_request *);
 void (*sleep_ms)(void *,unsigned);void *arg;
 unsigned samples;
 /* Optional serial-worker admission. NULL/NULL preserves the original loop.
  * Return 0 to wait without starting cloud ASR, 1 to permit the original
  * listen call, or a negative error to stop. The callback selects idle only;
  * TRACK/PHOTO retain their original route. It may arm one borrowed staged
  * input. Every return 1 is paired with listen_release, including stop races
  * and failures before provider entry. Callbacks must be bounded/nonblocking,
  * honor cancellation and must not dispatch mode or reenter this service. */
 int (*listen_admit)(void *,const struct vv_https_cancel *);
 void (*listen_release)(void *);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 /* Optional serial worker maintenance, only without a retained/active photo
  * request. Never called under the service lock. Errors do not replay work or
  * retire speech; implementation owns retry policy and must honor cancellation. */
 int (*maintenance)(void *,const struct vv_https_cancel *);
#endif
 /* Optional serial callback before readiness and all ordinary cloud calls.
  * safe=false forbids renewal IO (retained PHOTO/request); implementation may
  * still observe trust. 0=continue, 1=wait, negative=stop. No hidden retries.
  * Original NULL route never calls it. All provider calls have returned. */
 int (*time_maintenance)(void *,bool safe,const struct vv_https_cancel *);
};
struct vv_native_service_status {bool configured,running,stopping;unsigned missing;int last_error;
 bool report_available;int report_error;struct vv_assessment_result assessment;struct vv_report_audio_result audio;};
/* Singleton registration borrows all state until successful unregister. No
 * startup/registration runs hardware. check/claim/release must not reenter. */
int vv_native_service_register(const struct vv_native_service_config *);
int vv_native_service_unregister(void);
int vv_native_service_status(struct vv_native_service_status *);
int vv_native_service_run(void);
/* Signals cancellation, returns without joining an in-flight IO callback.
 * Status remains running until worker cleanup/release has actually completed. */
int vv_native_service_stop(void);
#endif
