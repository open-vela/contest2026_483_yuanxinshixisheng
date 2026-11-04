#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller supplies verified read-only assets and an output buffer. This entry
 * does not play audio or log text. Cancellation is cooperative between chunks. */
struct k7_tts_request {
  const char *model;
  const char *tokens;
  const char *lexicon;
  const char *text;
  int (*cancel_requested)(void *);
  void *cancel_ctx;
};
int k7_tts_synthesize(const struct k7_tts_request *, int16_t *pcm,
                      size_t capacity, size_t *frames, unsigned *sample_rate);
#ifdef __cplusplus
}
#endif
