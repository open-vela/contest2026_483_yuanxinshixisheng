#ifndef VV_BACKEND_PROTOCOL_H
#define VV_BACKEND_PROTOCOL_H
#include <stddef.h>
#include <stdbool.h>

#define VV_BACKEND_JSON_MAX 16384
#define VV_BACKEND_TOKEN_CAP 2049
enum vv_backend_error { VV_BACKEND_OK=0, VV_BACKEND_INVALID=-1,
  VV_BACKEND_LIMIT=-2, VV_BACKEND_MOCK=-3 };
struct vv_backend_session {
  char token[VV_BACKEND_TOKEN_CAP];
  /* Strict UTC RFC3339 seconds only; fractional seconds/offsets rejected.
   * Unlike the old Python ISO parser, broader formats need backend confirmation.
   * Caller must check current clock/expiry. */
  char expires_at[21];
};
enum vv_backend_task_status { VV_TASK_QUEUED, VV_TASK_ANALYZING,
  VV_TASK_NEEDS_RETAKE, VV_TASK_FAILED, VV_TASK_REPORT_READY };
enum vv_backend_view { VV_VIEW_FRONT=1, VV_VIEW_LEFT=2, VV_VIEW_RIGHT=4 };
struct vv_backend_task {
  char task_id[37], report_id[37], failure_code[129];
  enum vv_backend_task_status status;
  unsigned required_views;
};
/* Pure codecs: no transport, persistence, logging or credentials embedded.
 * Caller handles HTTP status (including 401/409) before invoking parsers.
 * Success never proves a service is real: only explicit mock:true is rejected.
 * Board cJSON is built with CJSON_NESTING_LIMIT=4. The codec's separate depth-16
 * precheck is only an allocation guard, not a promise to accept depth 16.
 * Caller must securely clear credential JSON/session buffers after use. */
int vv_backend_login_json(const char *credential, const char *version,
  const char *proof, char *out, size_t capacity, size_t *written);
int vv_backend_parse_session(const char *json, size_t size,
  struct vv_backend_session *out);
int vv_backend_parse_task(const char *json, size_t size,
  const char *expected_task_id, struct vv_backend_task *out);
/* Valid UTF-8, Unicode White_Space trimmed; 1..1000 Unicode scalars after
 * trim, <=4000 UTF-8 bytes. Input scan bounded to JSON_MAX. No normalization. */
int vv_backend_tts_json(const char *text, char *out, size_t capacity,
  size_t *written);
/* Exact audio/pcm; bare MIME remains accepted for existing service compatibility.
 * Known parameters must match rate=16000, channels=1, encoding=signed-int,
 * bits=16 and cannot repeat (case-insensitive names). Unknown well-formed
 * metadata remains tolerated. MIME never proves actual PCM format or realism. */
bool vv_backend_tts_content_type(const char *value);
#endif
