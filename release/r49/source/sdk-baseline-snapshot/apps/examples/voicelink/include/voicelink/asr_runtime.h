#ifndef VELAVISION_VOICELINK_ASR_RUNTIME_H
#define VELAVISION_VOICELINK_ASR_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct k7_asr_audio_request {
  const float *samples;
  unsigned frames;
  unsigned sample_rate_hz;
  uint64_t deadline_ms;
  int (*cancel_requested)(void *ctx);
  void *cancel_ctx;
};

int k7_asr_audio_text(const struct k7_asr_audio_request *request,
                      char *utf8_text, size_t capacity);
int k7_asr_model_session_probe(void);
int k7_asr_microphone_probe(void);
void k7_asr_runtime_reset(void);

/* Recognize the last completed microphone capture into caller-owned storage.
 * Returns 0 on success or a negative errno value. The destination is always
 * NUL terminated when capacity is nonzero and is cleared on every failure. */
int k7_asr_microphone_text(char *utf8_text, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
