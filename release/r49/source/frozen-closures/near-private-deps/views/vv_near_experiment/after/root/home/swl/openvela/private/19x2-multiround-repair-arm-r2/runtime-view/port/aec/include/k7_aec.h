#ifndef K7_AEC_H
#define K7_AEC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Optional worker-only SpeexDSP AEC. Never run in a PIO/ISR callback.
 * Each context has one owner; callers serialize process/reset/stats/destroy.
 * This API does not establish acoustic alignment or estimate a missing delay.
 */
#define K7_AEC_SAMPLE_RATE 16000u
#define K7_AEC_FRAME_SAMPLES 320u
#define K7_AEC_DEFAULT_ARENA_BYTES (256u * 1024u)
enum k7_aec_flags {
  K7_AEC_ALIGNMENT_VERIFIED = 1u,
  K7_AEC_REFERENCE_VALID = 2u,
  K7_AEC_EXPLICIT_MUTED = 4u,
  K7_AEC_CANCELLED_TAIL = 8u
};
enum k7_aec_discontinuity {
  K7_AEC_GAP = 1, K7_AEC_CLOCK_CHANGE, K7_AEC_GENERATION_CHANGE,
  K7_AEC_CANCEL_TAIL, K7_AEC_BAD_REFERENCE, K7_AEC_LIBRARY_FAULT
};
struct k7_aec_config {
  uint32_t tail_samples; /* 0 = 1600 (100 ms); 320..5120, multiple of 320. */
  size_t arena_bytes;    /* 0 = 256 KiB; fixed allocation at create only. */
};
struct k7_aec_frame {
  const int16_t *mic;
  const int16_t *render; /* NULL only for explicit, independently proven mute. */
  uint64_t mic_first_sample;
  uint64_t render_first_sample; /* Both mapped to the SAME calibrated clock. */
  uint32_t clock_epoch;
  uint32_t render_generation;  /* 0 only for explicit mute. */
  uint32_t flags;
  uint16_t samples;            /* Exactly 320; no zero-padding partial input. */
};
struct k7_aec_stats {
  uint64_t frames, rejected, resets, warnings;
  uint64_t clipped_mic, clipped_render;
  size_t arena_used, arena_bytes, allocations;
  uint32_t ready, last_discontinuity;
};
struct k7_aec;
int k7_aec_create(const struct k7_aec_config *, struct k7_aec **out);
/* Explicit caller assertion after calibrated alignment, never just FIFO index
 * equality. Reset loses filter adaptation. No output until this succeeds. */
int k7_aec_align(struct k7_aec *, uint32_t clock_epoch,
                 uint32_t render_generation, uint64_t first_sample,
                 uint32_t flags);
/* On any failure output is untouched; caller must NOT label it cleaned audio.
 * Discontinuities latch not-ready until an explicit, verified align call. */
int k7_aec_process(struct k7_aec *, const struct k7_aec_frame *, int16_t out[320]);
int k7_aec_invalidate(struct k7_aec *, enum k7_aec_discontinuity);
int k7_aec_get_stats(struct k7_aec *, struct k7_aec_stats *);
int k7_aec_destroy(struct k7_aec *);
/* Optional normalized-float ingress: all finite/in-range before ANY write. */
int k7_aec_float_to_s16(const float *, size_t count, int16_t *out);

/* Diagnostic-only worker path for an UNVERIFIED timing hypothesis. These
 * residual samples MUST NOT enter KWS/ASR/control as "clean" audio.
 * Reset/process always leave stats.ready == 0; diagnostic processing never
 * increments verified stats.frames. All normal allocation guards still apply.
 * Never mix this path and verified process on one live context. Successful
 * align resets the filter and closes the probe; explicit invalidate also
 * closes it. All outputs are untouched on error. No NULL/synthetic mute. */
int k7_aec_diagnostic_reset(struct k7_aec *);
int k7_aec_diagnostic_process(struct k7_aec *, const int16_t *mic,
  const int16_t *reference, size_t samples, int16_t out[320]);

#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY

/* Optional diagnostic continuity stream. One exclusive worker owns ctx and all
 * tokens; it must retire all calls before destroy. These value tokens do NOT
 * retain ctx. A successful begin resets adaptation. cancel_seal alone does NOT
 * reset or feed Speex: it marks a software control boundary, not AMP/DAC/mute
 * completion, acoustic calibration, or near-end qualification.
 *
 * Both sample axes are explicit and may have different origins. Caller must
 * provide the original continuous MIC and independently mapped reference; IDs
 * and index equality are accounting only, never evidence that mapping is true.
 * No NULL reference, implicit zero, padding, elapsed-time/mute shortcut, or
 * switch to raw-as-clean exists. Actual all-zero reference samples are accepted
 * only as supplied data; this API cannot establish their physical provenance.
 *
 * All results are diagnostic residuals: stats.ready and status.qualified stay
 * zero. Never send them to KWS/ASR/control under a clean-audio claim. Old APIs
 * revoke these tokens; they retain their original verified/reset behavior.
 */
struct k7_aec_continuity_identity {
  uint64_t owner_epoch, mapping_epoch, reference_instance, reference_epoch;
  uint32_t input_epoch, clock_epoch, render_generation;
};
enum k7_aec_continuity_phase {
  K7_AEC_CONTINUITY_NONE = 0,
  K7_AEC_CONTINUITY_BEFORE_CANCEL = 1,
  K7_AEC_CONTINUITY_AFTER_CANCEL = 2
};
struct k7_aec_continuity_token {
  uint64_t serial; /* Process-wide non-reused; exhaustion fails closed. */
  struct k7_aec_continuity_identity identity;
  uint64_t mic_first_sample, reference_first_sample;
  uint32_t phase;
};
struct k7_aec_continuity_frame {
  struct k7_aec_continuity_identity identity;
  const int16_t *mic, *reference; /* Both real complete supplied arrays. */
  uint64_t mic_first_sample, reference_first_sample;
  uint16_t samples; /* Exactly320, each axis contiguous; no flag-based mute. */
};
struct k7_aec_continuity_status {
  struct k7_aec_continuity_token token;
  uint64_t mic_next_sample, reference_next_sample, frames;
  uint32_t active, qualified; /* qualified is ALWAYS zero. */
};
int k7_aec_diagnostic_continuity_begin(struct k7_aec *,
  const struct k7_aec_continuity_identity *, uint64_t mic_first,
  uint64_t reference_first, struct k7_aec_continuity_token *);
/* Stale token/foreign owner: reject without touching the current stream.
 * Current token/owner with changed epoch or reference lineage, gap, missing
 * data or overflow: latch invalid until a
 * new explicit begin. Every error leaves out unchanged. No feed allocation. */
int k7_aec_diagnostic_continuity_process(struct k7_aec *,
  const struct k7_aec_continuity_token *, const struct k7_aec_continuity_frame *,
  int16_t out[320]);
/* Exact next indices are required and at least one full frame must have been
 * processed. Successful seal issues a new token, rejects the old token, and
 * preserves ALL Speex filter/adaptation state. Output token unchanged on error.
 * The recorded render_generation remains the original reference lineage even
 * after cancellation; generation0 or a later TX cannot silently replace it. */
int k7_aec_diagnostic_cancel_seal(struct k7_aec *,
  const struct k7_aec_continuity_token *, uint64_t mic_next,
  uint64_t reference_next, struct k7_aec_continuity_token *);
int k7_aec_diagnostic_continuity_status(struct k7_aec *,
  struct k7_aec_continuity_status *);


#endif
#ifdef __cplusplus
}
#endif
#endif
