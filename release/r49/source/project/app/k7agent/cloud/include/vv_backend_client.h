#ifndef VV_BACKEND_CLIENT_H
#define VV_BACKEND_CLIENT_H
#include "vv_backend_protocol.h"
#include "vv_https_client.h"
#include "assessment_multipart.h"
#include "vv_chat_stream.h"

enum vv_backend_client_error {
  VV_BACKEND_CLIENT_INVALID=-2000, VV_BACKEND_CLIENT_MEMORY,
  VV_BACKEND_CLIENT_TIME, VV_BACKEND_CLIENT_AUTH,
  VV_BACKEND_CLIENT_HTTP, VV_BACKEND_CLIENT_REPLACED,
  VV_BACKEND_CLIENT_PROTOCOL, VV_BACKEND_CLIENT_MOCK,
  VV_BACKEND_CLIENT_AUDIO,
  VV_BACKEND_CLIENT_REPORT_MISSING
};
struct vv_backend_endpoint { const char *host; uint16_t port; const char *path; };
struct vv_backend_client {
  struct vv_https_client *https;
  const char *host;
  uint16_t port;
  const char *base_path; /* "" or /prefix without trailing slash. */
  bool (*utc_now)(void *arg, uint64_t *seconds); /* false = untrusted clock */
  void *clock_arg;
  char token[VV_BACKEND_TOKEN_CAP];
  uint64_t expires_at, last_utc;
  int last_http_status;
  bool development_lan_http; /* Explicit fixed-origin development only. */
};
/* Caller owns/zero-initializes client and keeps configuration alive. Calls are
 * synchronous and must be serialized. No automatic requests, retries, refresh,
 * playback, logging, or credential persistence. Token is rejected <=60s before
 * expiry. Untrusted/backwards time and HTTP401 clear token; login is explicit.
 * Underlying vv_https errors are preserved. HTTP failures never replay writes. */
void vv_backend_client_clear_session(struct vv_backend_client *client);
int vv_backend_client_login(struct vv_backend_client *client,
  const char *credential, const char *version, const char *proof,
  const struct vv_https_cancel *cancel);
/* One same-origin POST to the existing stateless gimbal-ai SSE endpoint.
 * Uses the existing device session; no replay, implicit login or history.
 * Question is limited to the existing Java DTO's 2000 UTF-16 units; oversize
 * returns -E2BIG before sending, without truncating the current question.
 * Returns only completed.answerText after complete HTTP framing. Provisional
 * deltas are never played. answer is caller-private scratch and is cleared on
 * every failure; capacity must be 1..VV_CHAT_ANSWER_MAX+1. In addition to client
 * and transport errors this can return vv_chat_error (including dependency
 * timeout/unavailable). Caller serializes with all other client operations. */
int vv_backend_client_chat(struct vv_backend_client *client,const char *text,
  char *answer,size_t capacity,const struct vv_https_cancel *cancel);
int vv_backend_client_task(struct vv_backend_client *client,const char *task_id,
  struct vv_backend_task *out,const struct vv_https_cancel *cancel);
/* Exactly one POST; caller retains immutable pictures/refs/idempotency key
 * through uncertain outcomes. Never creates consent, a key, or a retry.
 * images order: front,left,right. Uses existing multipart encoder's 1 MiB per
 * image and ASCII identifier limits (stricter than host's 10 MiB/Unicode refs).
 * Only 200/202 queued + photoVersion "1" + UUID yields accepted_task_id. */
int vv_backend_client_upload(struct vv_backend_client *client,
  const char *capture_session,const char *consent_ref,const char *boundary_hex,
  const char *idempotency_key,const struct k7_upload_image images[3],
  char accepted_task_id[37],const struct vv_https_cancel *cancel);
struct vv_backend_brief {
  char report_id[37];
  char conclusion[513],description[4097];
};
/* V3 explicit long-poll GET, wait_seconds 0..25. Flat same-response task and
 * brief identities are strictly validated. READY without a valid nonempty
 * brief fails; no implicit GET, login or replay. All outputs clear on failure.
 * Only complete HTTP502/503/504 maps to VV_HTTPS_EIO for bounded caller retry.
 * Request-local timeout is at least wait+15s; shared limits remain unchanged. */
int vv_backend_client_task_wait(struct vv_backend_client *,const char *,unsigned,
 struct vv_backend_task *,struct vv_backend_brief *,const struct vv_https_cancel *);
/* Minimal known projection only: require matching reportId and view="brief";
 * conclusion/description may be absent/null, otherwise bounded strings. Other
 * report fields are not interpreted. Rejects explicit mock:true, but absence
 * does not establish real analysis. Caller rechecks task identity before play.
 * The strict required identity/view is narrower than the legacy host client. */
int vv_backend_client_brief(struct vv_backend_client *client,const char *report_id,
  struct vv_backend_brief *out,const struct vv_https_cancel *cancel);
/* PCM buffer is PRIVATE scratch until return 0; on failure it is cleared and
 * *pcm_size=0. This buffers the complete response; never call a speaker sink.
 * Endpoint and auth mode must come from a confirmed service contract. Device
 * Bearer can only be sent to the exact API host/port. No vendor key belongs here.
 * Max 960000 bytes. MIME alone cannot prove sample format or non-mock audio. */
int vv_backend_client_tts(struct vv_backend_client *client,
  const struct vv_backend_endpoint *endpoint,bool device_bearer,const char *text,
  unsigned char *pcm,size_t capacity,size_t *pcm_size,
  const struct vv_https_cancel *cancel);
#endif
