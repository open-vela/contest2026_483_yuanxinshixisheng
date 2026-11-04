#ifndef K7_VAD_OBSERVER_H
#define K7_VAD_OBSERVER_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32) && defined(K7_VAD_BUILD_DLL)
#define K7VAD_API __declspec(dllexport)
#else
#define K7VAD_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define K7_VAD_WINDOW 576u
#define K7_VAD_ADVANCE 512u
#define K7_VAD_CONTEXT 64u
#define K7_VAD_MAX_FEED 320u
/* Numeric observation only. No threshold, speech/near-end qualification,
 * reply cancellation, network, device, threading or callback into audio owners.
 * One serialized worker owns all calls and keeps the borrowed backend alive.
 */
struct k7_vad;
struct k7_vad_backend {
  int (*reset)(void *);
  int (*run)(void *, const float samples[576], float *probability);
};
struct k7_vad_identity { uint64_t stream_id; uint32_t epoch, source_tag; };
struct k7_vad_frame {
  struct k7_vad_identity identity;
  uint64_t frame_id, first_sample;
  const int16_t *pcm;
  size_t samples; /* 1..320 genuine contiguous S16 samples. */
};
struct k7_vad_observation {
  struct k7_vad_identity identity;
  uint64_t window_id, completing_frame_id;
  uint64_t input_first_sample, input_end_sample;
  uint64_t advance_first_sample, advance_end_sample;
  float probability;
};
struct k7_vad_stats {
  uint64_t accepted_frames, accepted_samples, observations, resets, rejected;
  size_t context_bytes;
  uint32_t ready, buffered_samples;
};
K7VAD_API size_t k7_vad_context_bytes(void);
K7VAD_API int k7_vad_create(const struct k7_vad_backend *, void *backend,
                           struct k7_vad **);
/* Required initially and after EVERY invalidation/error. No automatic reset.
 * Sets the exact next frame/sample identity; accumulates576 real samples.
 */
K7VAD_API int k7_vad_reset(struct k7_vad *, struct k7_vad_identity,
                          uint64_t next_frame_id, uint64_t next_sample);
/* Return1 observation/0 need samples/negative error. On any error or0 output
 * is untouched. Frame/sample/identity gaps, malformed frames and backend
 * failure latch not-ready until explicit reset. Max320 input => max1 output.
 * S16 converts via /32768 only. Never pad, resample, suppress or rewrite PCM.
 */
K7VAD_API int k7_vad_feed(struct k7_vad *, const struct k7_vad_frame *,
                         struct k7_vad_observation *);
K7VAD_API void k7_vad_invalidate(struct k7_vad *);
K7VAD_API int k7_vad_stats(const struct k7_vad *, struct k7_vad_stats *);
K7VAD_API void k7_vad_destroy(struct k7_vad *);
#ifdef __cplusplus
}
#endif
#endif
