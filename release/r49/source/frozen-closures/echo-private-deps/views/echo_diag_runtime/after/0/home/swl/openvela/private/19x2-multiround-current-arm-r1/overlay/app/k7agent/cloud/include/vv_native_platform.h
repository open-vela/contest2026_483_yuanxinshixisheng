#ifndef VV_NATIVE_PLATFORM_H
#define VV_NATIVE_PLATFORM_H
#include "vv_native_service.h"
#include "vv_confirmed_wake.h"
struct vv_barge_driver;
struct vv_native_platform_config {
 struct vv_https_mbedtls_config tls;
 struct vv_https_limits limits;
 bool (*utc_now)(void *,uint64_t *);void *clock_arg;
 /* Thread-safe, nonblocking actual platform evidence. No preset success flags.
  * Report network/time/CA/entropy/preview/owner/metadata missing bits.
  * AUTH/ENDPOINT are owned here and ignored from this external callback.
  * Metadata means factory/consent configuration, not yet-created photo IDs;
  * preview means available pipeline, not permanently held while tracking. */
 unsigned (*missing)(void *);
 int (*claim)(void *);void (*release)(void *);
 int (*make_request)(void *,struct vv_assessment_request *);
 void (*release_request)(void *,struct vv_assessment_request *);
 void (*sleep_ms)(void *,unsigned);void *arg;
 unsigned samples;size_t pcm_capacity;
 const struct vv_speech_provider *speech; /* optional independent ASR/TTS provider */
 bool development_lan_http; /* Explicit fixed-origin HTTP; requires separate speech provider. */
 /* Default false. Explicit resident diagnostic identity routing only; requires
  * successful capture receipts and scoped provider. Does not enable barge-in. */
 bool scoped_dialogue;
 /* Optional idle KWS -> preserved raw PCM -> whole cloud ASR confirmation.
  * All wake/skin/stop candidates are eligible. TRACK/PHOTO keep the old loop.
  * Independent of acoustic barge-in; no clean qualification is claimed. */
 bool always_wake;
 /* Default false. Only an explicit independently qualified full-wake ticket
  * may cancel the exact active natural reply. This does not enable raw KWS
  * dispatch, idle wake, motion, PHOTO cancellation, or automatic recovery.
  * consume must be short/nonblocking/non-reentrant and revalidate current
  * clean/model/calibration epochs, consume a ticket at most once, and return
  * 1 only for valid CLEAN_FULL_KWS. All borrowed state outlives destroy. */
 bool confirmed_barge;
 /* Default off. Natural onset is independent of full-wake confirmation.
  * Requires an externally qualified driver and scoped dialogue. Driver must
  * join all its observers on detach; no acoustic provider ships here. */
 bool onset_barge;
 const struct vv_barge_driver *onset_driver;
 int (*consume_confirmed_wake)(void *,const struct vv_confirmed_wake_event *,
                               const struct vv_confirmed_wake_context *,uint64_t);
 void *confirmed_wake_arg;
 /* Optional same-factory time maintenance. status: 0=current and not due,
  * 1=due but valid, 2=expired, negative=integrity/configuration failure.
  * refresh: 0=ordinary calls may proceed, 1=wait with no ordinary cloud IO,
  * negative=stop. Platform supplies actual serial/scope/audio exclusion.
  * Both callbacks and their arg live as long as this platform. NULL is old. */
 int (*trusted_time_status)(void *);
 int (*trusted_time_refresh)(void *,const struct vv_https_cancel *);
 void *trusted_time_arg;
 /* Optional independent source lease. status:0=valid/not due,1=due/valid,
  * -EBUSY=read-only entry contention (wait), other negative=stop.
  * refresh:0=renewed/current,1=wait without provider IO, negative=stop.
  * Pair must both be NULL or both present; no raw read in status/TLS. */
 int (*entropy_status)(void *);
 int (*entropy_refresh)(void *,const struct vv_https_cancel *);
 void *entropy_arg;
};
struct vv_native_platform;
/* Uses confirmed eveaisia.com:443 /openvela/asr,/openvela/tts; backend adds
 * /api/v1 to base /openvela. No credentials embedded, no DNS/login/recording.
 * TLS creation may initialize approved entropy. All config memory is borrowed. */
struct vv_native_platform *vv_native_platform_create(const struct vv_native_platform_config *);
/* Explicit HTTPS login BEFORE binding. Never prints or retains input secrets.
 * Login failure clears local ready marker; backend validates token/expiration. */
int vv_native_platform_login(struct vv_native_platform *,const char *,const char *,const char *,const struct vv_https_cancel *);
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
struct vv_k7_secure_login;
/* Register the existing private loader BEFORE binding. Retains only callback
 * and arg, never the loaded secret. The service owner renews idle business
 * sessions with bounded monotonic backoff; no automatic business POST replay.
 * Loader and arg must remain alive until successful platform destruction. */
int vv_native_platform_set_business_loader(struct vv_native_platform *,
 int (*)(void *,struct vv_k7_secure_login *),void *);
#endif
int vv_native_platform_bind(struct vv_native_platform *);
/* Refuses while service runs, preserving all borrowed objects. */
int vv_native_platform_destroy(struct vv_native_platform *);
unsigned vv_native_platform_missing(struct vv_native_platform *);
#endif
