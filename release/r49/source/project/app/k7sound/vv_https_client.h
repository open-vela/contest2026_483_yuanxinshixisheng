#ifndef VV_HTTPS_CLIENT_H
#define VV_HTTPS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vv_https_error {
  VV_HTTPS_OK = 0,
  VV_HTTPS_EINVAL = -1000,
  VV_HTTPS_ECANCELLED,
  VV_HTTPS_ETIMEOUT,
  VV_HTTPS_ETLS_VERIFY,
  VV_HTTPS_EIO,
  VV_HTTPS_EPROTO,
  VV_HTTPS_ELIMIT,
  VV_HTTPS_ESINK,
  VV_HTTPS_EBODY
};

struct vv_https_cancel {
  bool (*is_cancelled)(void *arg);
  void *arg;
};

struct vv_https_peer_security {
  bool sni_set;
  bool peer_verified;
  uint32_t verify_flags;
};

/* All deadlines are absolute monotonic milliseconds. Transport methods must
 * return within deadline_ms and should check cancel while waiting. */
struct vv_https_transport_ops {
  int (*connect)(void *ctx, const char *host, uint16_t port,
                 uint64_t deadline_ms, const struct vv_https_cancel *cancel,
                 struct vv_https_peer_security *security);
  ssize_t (*write)(void *ctx, const void *data, size_t len,
                   uint64_t deadline_ms,
                   const struct vv_https_cancel *cancel);
  ssize_t (*read)(void *ctx, void *data, size_t len, uint64_t deadline_ms,
                  const struct vv_https_cancel *cancel);
  void (*close)(void *ctx);
};

struct vv_https_header {
  const char *name;
  const char *value;
  bool sensitive;
};

typedef ssize_t (*vv_https_body_read_fn)(void *arg, size_t offset, void *dst,
                                         size_t capacity);
typedef int (*vv_https_sink_fn)(void *arg, const void *data, size_t len);
typedef uint64_t (*vv_https_now_ms_fn)(void *arg);
typedef void (*vv_https_log_fn)(void *arg, const char *event, int value);

struct vv_https_request {
  const char *method;
  const char *host;
  uint16_t port; /* 0 means 443 */
  const char *path;
  const struct vv_https_header *headers;
  size_t header_count;
  size_t body_length;
  vv_https_body_read_fn body_read;
  void *body_arg;
};

struct vv_https_limits {
  size_t max_request_header_bytes;
  size_t max_response_header_bytes;
  size_t max_response_body_bytes;
  size_t max_chunk_bytes;
  uint32_t total_timeout_ms;
};

struct vv_https_client {
  const struct vv_https_transport_ops *transport;
  void *transport_ctx;
  vv_https_now_ms_fn now_ms;
  void *now_arg;
  vv_https_log_fn log; /* Receives event names and numeric metadata only. */
  void *log_arg;
  struct vv_https_limits limits;
};

#define VV_HTTPS_CONTENT_TYPE_CAPACITY 128

struct vv_https_response {
  int status_code;
  size_t body_bytes;
  bool chunked;
  /* Empty when absent; outer OWS removed, parameters/case preserved. Valid
   * only on a successful perform. This is metadata, not a MIME allowlist.
   * A sink must buffer untrusted audio until the caller validates the result. */
  char content_type[VV_HTTPS_CONTENT_TYPE_CAPACITY];
  /* Observation only: transport returned zero while framed HTTP still needed
   * bytes. False for normal close-delimited body EOF, syntax/sink errors and
   * negative transport returns. Never changes validation or the return code. */
  bool premature_eof;
};

int vv_https_perform(struct vv_https_client *client,
                     const struct vv_https_request *request,
                     vv_https_sink_fn sink, void *sink_arg,
                     const struct vv_https_cancel *cancel,
                     struct vv_https_response *response);

/* Explicit development-only cleartext entry; exact origin only, no TLS claims.
 * Disabled builds reject before connecting. Normal vv_https_perform stays TLS. */
int vv_http_development_perform(struct vv_https_client *,
 const struct vv_https_request *,vv_https_sink_fn,void *,
 const struct vv_https_cancel *,struct vv_https_response *);

void vv_https_secure_zero(void *data, size_t len);
const char *vv_https_strerror(int error);

#ifdef __cplusplus
}
#endif
#endif
