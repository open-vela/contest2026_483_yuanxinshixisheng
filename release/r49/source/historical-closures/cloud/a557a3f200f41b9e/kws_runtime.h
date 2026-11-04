#ifndef VELAVISION_KWS_RUNTIME_H
#define VELAVISION_KWS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional KWS module. No automatic startup and no hardware side effects.
 * create/feed/reset/destroy belong to ONE consumer worker, never the audio PIO
 * producer. Only request_cancel may run concurrently (before destroy begins).
 * Caller retains immutable asset buffers until destroy returns. */
typedef struct k7_kws_runtime k7_kws_runtime;
struct k7_kws_blob {
  const uint8_t *data;
  size_t bytes;
  uint8_t sha256[32];
};
struct k7_kws_assets {
  struct k7_kws_blob encoder, decoder, joiner, tokens, keywords;
};
typedef int (*k7_kws_sha256_fn)(const void *, size_t, uint8_t out[32], void *);
struct k7_kws_config {
  struct k7_kws_assets assets;
  k7_kws_sha256_fn sha256;
  void *sha256_context;
  /* Probability (0,1]; score must be finite, positive. */
  float threshold, score;
  uint32_t duplicate_guard_ms; /* <= 10000; 0 disables duplicate suppression. */
};
struct k7_kws_detection {
  char keyword[96]; /* Exact configured keyword label; no action is executed. */
  uint64_t end_sample;
  uint32_t epoch;
};

int k7_kws_create(const struct k7_kws_config *, k7_kws_runtime **);
/* PCM16 mono 16 kHz; 1..320 real samples, including explicit partial frames.
 * first_sample and epoch come from capture. Gaps reset acoustic state; no
 * padding or stitching across dropped frames. Returns 1 keyword, 0 none, <0
 * errno. The result is cleared on every call. Cancellation stays latched until
 * reset; a single backend decode is not preemptible, so measure its latency. */
int k7_kws_feed(k7_kws_runtime *, const int16_t *pcm, size_t samples,
                uint64_t first_sample, uint32_t epoch,
                struct k7_kws_detection *);
void k7_kws_request_cancel(k7_kws_runtime *);
int k7_kws_reset(k7_kws_runtime *);
void k7_kws_destroy(k7_kws_runtime *);

#ifdef __cplusplus
}
#endif
#endif
