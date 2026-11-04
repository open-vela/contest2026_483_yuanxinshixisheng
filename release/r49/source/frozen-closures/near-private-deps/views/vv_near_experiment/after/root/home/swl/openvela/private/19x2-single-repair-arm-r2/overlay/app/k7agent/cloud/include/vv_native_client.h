#ifndef VV_NATIVE_CLIENT_H
#define VV_NATIVE_CLIENT_H
#include "vv_https_mbedtls.h"
#include "vv_backend_client.h"
#include "vv_asr_batch.h"
#include "vv_speech_provider.h"

struct vv_native_client_config {
  struct vv_https_mbedtls_config tls;
  struct vv_https_limits limits;
  const char *host, *base_path, *asr_path, *tts_path;
  uint16_t port;
  bool tts_device_bearer;
  bool (*utc_now)(void *, uint64_t *);
  void *clock_arg;
  const struct vv_speech_provider *speech; /* optional explicit provider; no fallback on failure */
 bool development_lan_http; /* Explicit fixed-origin HTTP; requires separate speech provider. */
};
struct vv_native_client;
/* Explicit assembly, no DNS/network/login/audio at creation. All configuration
 * strings, CA and callback state are borrowed until destroy. Single owner:
 * no concurrent calls or destroy, including use of the borrowed backend.
 * TLS time must be the same trusted clock reported by utc_now. */
struct vv_native_client *vv_native_client_create(const struct vv_native_client_config *);
bool vv_native_client_streaming(const struct vv_native_client *);
bool vv_native_client_streaming_tts(const struct vv_native_client *);
int vv_native_client_speak(struct vv_native_client *,const char *,const struct vv_https_cancel *);
int vv_native_client_listen(struct vv_native_client *,unsigned,const struct vv_https_cancel *,struct vv_asr_batch_result *);
void vv_native_client_destroy(struct vv_native_client *);
struct vv_backend_client *vv_native_client_backend(struct vv_native_client *);
int vv_native_client_asr(struct vv_native_client *, const unsigned char *, size_t,
  const struct vv_https_cancel *, struct vv_asr_batch_result *);
int vv_native_client_tts(struct vv_native_client *, const char *, unsigned char *,
  size_t, size_t *, const struct vv_https_cancel *);
#endif
