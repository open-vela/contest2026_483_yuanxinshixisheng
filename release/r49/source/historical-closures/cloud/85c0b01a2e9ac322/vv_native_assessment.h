#ifndef VV_NATIVE_ASSESSMENT_H
#define VV_NATIVE_ASSESSMENT_H
#include "vv_backend_client.h"
#include <stdatomic.h>
enum vv_assessment_outcome { VV_ASSESSMENT_READY=0, VV_ASSESSMENT_INVALID,
  VV_ASSESSMENT_BUSY, VV_ASSESSMENT_CANCELLED, VV_ASSESSMENT_TIMEOUT,
  VV_ASSESSMENT_POLL_LIMIT, VV_ASSESSMENT_NEEDS_RETAKE, VV_ASSESSMENT_FAILED,
  VV_ASSESSMENT_REPLACED, VV_ASSESSMENT_BACKEND_ERROR, VV_ASSESSMENT_PROTOCOL,
  VV_ASSESSMENT_MEMORY, VV_ASSESSMENT_UPLOADED };
struct vv_assessment_request {
  const char *capture_session,*consent_ref,*boundary_hex,*idempotency_key;
  struct k7_upload_image images[3];
  unsigned poll_interval_ms,max_polls,total_timeout_ms;
};
struct vv_assessment_result {
  enum vv_assessment_outcome outcome;
  char task_id[37],report_id[37],failure_code[129];
  unsigned required_views,polls,task_checks;
  bool upload_attempted; /* On error retain original key/images; no automatic replay. */
  int backend_error;
  size_t pcm_size;
};
struct vv_assessment_ops {
  int (*upload)(void *,const struct vv_assessment_request *,char [37],const struct vv_https_cancel *);
  int (*task)(void *,const char *,struct vv_backend_task *,const struct vv_https_cancel *);
  int (*brief)(void *,const char *,struct vv_backend_brief *,const struct vv_https_cancel *);
  /* MUST validate HTTP status/MIME before returning; no speaker callback. */
  int (*tts)(void *,const char *,unsigned char *,size_t,size_t *,const struct vv_https_cancel *);
};
struct vv_native_assessment {
  atomic_bool busy;
  const struct vv_assessment_ops *ops;void *ops_arg;
  uint64_t (*now_ms)(void *);void *clock_arg;
  /* Must wait at most ms, honor cancel, and never move clock backwards. */
  void (*wait_ms)(void *,unsigned,const struct vv_https_cancel *);void *wait_arg;
};
/* Init once before sharing; never reinitialize while running. Each owner admits
 * one transaction. No login, credential storage, auto POST retries or playback.
 * Request refs/photos/key stay immutable throughout the call and any recovery.
 * PCM is scratch until READY; admitted non-READY transactions clear it.
 * INVALID/BUSY preflight returns leave result and PCM untouched. READY means only
 * validated bytes + final matching task check, not real-world report acceptance.
 * Injected calls MUST honor combined cancellation/deadline. No worker spawned. */
void vv_native_assessment_init(struct vv_native_assessment *);
enum vv_assessment_outcome vv_native_assessment_run(struct vv_native_assessment *,
  const struct vv_assessment_request *,unsigned char *,size_t,
  const struct vv_https_cancel *,struct vv_assessment_result *);

/* Upload exactly once, poll and fetch brief, then recheck task/report identity.
 * No TTS callback or PCM allocation: returns conclusion + optional punctuation
 * + description for vv_report_audio segmented playback. 4612 bytes holds the
 * largest known brief text including NUL. Missing both fields fails closed.
 * Call this INSTEAD OF run, never after run (each is a new upload transaction).
 * task/report IDs are in result, text_bytes excludes NUL. Admitted failures
 * return empty text; INVALID/BUSY preserve caller memory. Recheck identities
 * before each eventual playback using the report_audio fresh callback. */
enum vv_assessment_outcome vv_native_assessment_prepare(struct vv_native_assessment *,
  const struct vv_assessment_request *,char *,size_t,size_t *,
  const struct vv_https_cancel *,struct vv_assessment_result *);
/* Optional synchronous progress after successful upload + validated task ID,
 * before first poll. Borrowed callback/arg live only for this call. Failure
 * terminates this one-shot transaction; no upload replay. */
enum vv_assessment_outcome vv_native_assessment_prepare_progress(struct vv_native_assessment *,
  const struct vv_assessment_request *,char *,size_t,size_t *,
  const struct vv_https_cancel *,struct vv_assessment_result *,
  int (*uploaded)(void *,const struct vv_https_cancel *),void *);

/* Exactly one validated upload. UPLOADED means accepted task UUID only:
 * no task/brief/TTS calls, no report-ready assertion, no automatic POST retry.
 * Uses total_timeout_ms; polling fields and wait_ms are not required.
 * INVALID/BUSY preserve result. Admitted calls reset result and retain attempt. */
enum vv_assessment_outcome vv_native_assessment_upload_only(struct vv_native_assessment *,
  const struct vv_assessment_request *,const struct vv_https_cancel *,
  struct vv_assessment_result *);

/* Explicit adapter to backend client; providing it performs no requests. */
struct vv_assessment_backend_adapter {
  struct vv_backend_client *client;
  struct vv_backend_endpoint tts_endpoint;bool tts_device_bearer;
};
extern const struct vv_assessment_ops vv_assessment_backend_ops;
#endif
