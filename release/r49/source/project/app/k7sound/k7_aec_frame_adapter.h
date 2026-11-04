#ifndef K7_AEC_FRAME_ADAPTER_H
#define K7_AEC_FRAME_ADAPTER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Private phase-1 contract only; no backend is implicitly selected or enabled.
 * One exclusive owner serializes init/begin/process/cancel/status. The adapter
 * allocates nothing. Backend reset/process/invalidate must obey that owner;
 * process160 must not allocate. Backend implementations are separately audited.
 */
#define K7_AEC_FA_INPUT_SAMPLES 320u
#define K7_AEC_FA_HALF_SAMPLES 160u
#define K7_AEC_FA_MAX_DELAY_SAMPLES 320u
#define K7_AEC_FA_HISTORY_HALVES 4u
#define K7_AEC_FA_BACKEND_ABI 1u

struct k7_aec_fa_identity {
 uint64_t owner_epoch, mapping_epoch, reference_instance, reference_epoch;
 uint32_t input_epoch, clock_epoch, render_generation;
};
struct k7_aec_fa_token {
 uint64_t serial; /* Process-wide non-reused; not a lifetime reference. */
 struct k7_aec_fa_identity identity;
};
struct k7_aec_fa_input {
 struct k7_aec_fa_identity before, after;
 const int16_t *mic, *reference;
 const uint8_t *reference_zero; /* Exactly320 supplied 0/1 provenance flags.
  * A1 requires a real zero reference sample; a0 may also contain natural zero.
  * These flags record caller provenance, not a new mute/acoustic assertion. */
 uint64_t mic_first, reference_first; /* Independent, contiguous sample axes. */
 unsigned samples; /* Exactly320; incomplete frames never enter the backend. */
 unsigned reference_valid; /* Exactly1: caller confirms complete source lineage,
  * not acoustic alignment, physical mute or near-speech qualification. */
};
struct k7_aec_fa_half {
 struct k7_aec_fa_identity identity;
 uint64_t backend_state_epoch;
 const int16_t *mic, *reference;
 const uint8_t *reference_zero;
 uint64_t mic_first, reference_first;
 unsigned samples; /* Always160. Pointers are valid only during the call. */
};
struct k7_aec_fa_support {
 uint64_t mic_first, mic_next, reference_first, reference_next;
 unsigned adaptive_history; /* Immediate support is not full adaptive history. */
};
struct k7_aec_fa_backend_output {
 struct k7_aec_fa_identity identity;
 uint64_t backend_state_epoch;
 uint64_t nominal_mic_first, nominal_reference_first;
 struct k7_aec_fa_support support;
 unsigned samples; /* OUTPUT requires all160 real output samples written. */
 int16_t pcm[160];
};
enum k7_aec_fa_backend_result {
 K7_AEC_FA_WARMUP=0, K7_AEC_FA_OUTPUT=1
};
struct k7_aec_fa_backend {
 unsigned abi;
 const char *name; /* Immutable for adapter lifetime; exposed, never fallback. */
 unsigned delay_samples; /* Fixed0/160/320 algorithmic OUTPUT block delay.
  * This does not bound/calibrate physical render->MIC propagation, device
  * buffering or the caller's upstream reference mapping. No shift is added. */
 unsigned max_warmup_halves; /* Bounded0..32 initial no-output half calls.
  * Longer-than-delay startup may discard a real prefix; it is reported.
  * WARMUP after the first output is a discontinuity, not concealed silence. */
 int (*reset)(void *,const struct k7_aec_fa_identity *,uint64_t state_epoch,
              uint64_t mic_first,uint64_t reference_first);
 int (*process160)(void *,const struct k7_aec_fa_half *,
                   struct k7_aec_fa_backend_output *);
 void (*invalidate)(void *);
};
struct k7_aec_fa_output {
 struct k7_aec_fa_identity identity;
 uint64_t backend_state_epoch; /* Explicit reset lineage, including adaptive state. */
 uint64_t nominal_mic_first, nominal_reference_first;
 /* Actual consumed-input watermarks when complete320 becomes publishable.
  * These are sample axes, NOT fabricated monotonic/physical emission times.
  * Function return is publication; wallclock timing needs an external probe. */
 uint64_t available_after_mic_next, available_after_reference_next;
 struct k7_aec_fa_support support;
 uint64_t startup_unavailable_samples;
 unsigned samples, backend_delay_samples, publication_delay_samples;
 unsigned qualified; /* Always0; bypass/backend output is diagnostic only. */
 int16_t pcm[320];
 uint8_t nominal_reference_zero[320]; /* Original supplied flags at nominal axis. */
};
struct k7_aec_fa_history {
 uint64_t mic_first, reference_first;
 unsigned valid;
 uint8_t zero[160];
};
struct k7_aec_frame_adapter {
 struct k7_aec_fa_backend backend;
 void *backend_context;
 struct k7_aec_fa_token token;
 uint64_t mic_origin,reference_origin,mic_next,reference_next;
 uint64_t halves,output_mic_next,output_reference_next,startup_unavailable;
 uint64_t published_frames,published_mic_next,published_reference_next;
 unsigned initialized,active,backend_live,warmup_halves,output_seen,pending;
 struct k7_aec_fa_backend_output pending_output;
 uint8_t pending_zero[160];
 struct k7_aec_fa_history history[K7_AEC_FA_HISTORY_HALVES];
};
struct k7_aec_fa_status {
 const char *backend_name;
 uint64_t mic_next,reference_next,processed_halves,startup_unavailable_samples;
 uint64_t published_frames,published_mic_next,published_reference_next;
 unsigned active,pending_samples,warmup_halves,backend_delay_samples;
};
/* Init on fresh caller-owned storage only; no implicit backend reset. */
int k7_aec_fa_init(struct k7_aec_frame_adapter *,
 const struct k7_aec_fa_backend *,void *);
/* Explicit begin revokes old pending output even if the new begin fails.
 * Backend reset must reset its own delay/history; a new token is issued only
 * after success. There is no automatic reset after a processing failure. */
int k7_aec_fa_begin(struct k7_aec_frame_adapter *,
 const struct k7_aec_fa_identity *,uint64_t,uint64_t,struct k7_aec_fa_token *);
/* Return1 publishes exactly320; return0 means no complete output yet. Every
 * other return leaves output untouched. Two process160 calls finish before
 * publication. Any current-stream error invalidates backend and pending PCM.
 * Stale token/foreign owner is rejected without damaging the current stream.
 * No flushing API invents missing MIC, reference or delayed tail samples. */
int k7_aec_fa_process(struct k7_aec_frame_adapter *,const struct k7_aec_fa_token *,
 const struct k7_aec_fa_input *,struct k7_aec_fa_output *);
int k7_aec_fa_cancel(struct k7_aec_frame_adapter *,const struct k7_aec_fa_token *);
int k7_aec_fa_status(const struct k7_aec_frame_adapter *,struct k7_aec_fa_status *);
#ifdef __cplusplus
}
#endif
#endif
