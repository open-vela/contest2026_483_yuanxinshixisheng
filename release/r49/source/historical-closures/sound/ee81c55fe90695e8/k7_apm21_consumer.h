#ifndef K7_APM21_CONSUMER_H
#define K7_APM21_CONSUMER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Private offline/link probe only. No FA backend or live clean-audio authority. */
#define K7_APM21_RATE_HZ 16000u
#define K7_APM21_FRAME_SAMPLES 160u
#define K7_APM21_CONTEXT_BYTES 2048u
#define K7_APM21_CONTEXT_ALIGNMENT 16u
typedef struct k7_apm21_context k7_apm21_context;
enum k7_apm21_result {
 K7_APM21_OK=0, K7_APM21_ARGUMENT=-19001, K7_APM21_ORDER=-19002,
 K7_APM21_UPSTREAM=-19003, K7_APM21_POISONED=-19004,
 K7_APM21_NO_MEMORY=-19005, K7_APM21_EXCEPTION=-19006
};
enum k7_apm21_stage {
 K7_APM21_PREFLIGHT=0, K7_APM21_RENDER=1, K7_APM21_DELAY=2,
 K7_APM21_CAPTURE=3, K7_APM21_COMPLETE=4
};
enum k7_apm21_delay_source {
 K7_APM21_DELAY_MEASURED=1, K7_APM21_DELAY_OFFLINE_HYPOTHESIS=2
};
struct k7_apm21_timing {
 int32_t valid; /* Must equal1. Never manufacture a default delay. */
 int32_t supplied_buffer_delay_ms; /* Explicit APM API delay, inclusive0..500. */
 int32_t source; /* One delay_source enum; hypothesis does not mean measured. */
};
struct k7_apm21_frame_report {
 uint64_t stream_epoch, frame_ordinal;
 int32_t stage, upstream_rc;
 uint32_t api_output_samples;
 int32_t supplied_buffer_delay_ms;
 int32_t delay_source;
 int32_t algorithmic_output_delay_samples; /* -1: unmeasured, not delay_ms. */
 uint32_t nominal_support_qualified; /* Always0 until a separate adapter proof. */
};

/* Storage is caller-owned, aligned16, >=2048B, previously unused or destroyed.
 * APM internals still allocate from the C++ heap; 2048B is NOT their heap bound.
 * All calls for one context, including destroy, require one serialized worker.
 * Construct/Initialize and destroy occur with audio stopped, never on DMA/ISR.
 */
size_t k7_apm21_internal_context_bytes(void);
int k7_apm21_construct(void *storage, size_t bytes, uint64_t stream_epoch,
                      k7_apm21_context **context, int32_t *upstream_rc);
void k7_apm21_destroy(k7_apm21_context *context);

/* Exactly160 signed native-endian int16 mono samples in each direction.
 * Reverse(render) -> supplied delay -> Process(capture), once, in that order.
 * Do not call Initialize per reply/cancel. Error after entering APM poisons this
 * instance; no retry, raw fallback or partially written output is published.
 * Caller must supply valid audio/metadata/storage with non-overlapping context
 * and metadata objects. Audio output may alias an audio input.
 * API output is numbered by call ordinal only; no physical/nominal sample axis
 * or startup-clean interval is asserted by this independent consumer.
 */
int k7_apm21_process_10ms(k7_apm21_context *context, uint64_t stream_epoch,
                        uint64_t frame_ordinal, const int16_t *render160,
                        const int16_t *microphone160,
                        const struct k7_apm21_timing *timing,
                        int16_t *output160, struct k7_apm21_frame_report *report);

/* Force this symbol during private final link; never called by product boot. */
int k7_apm21_consumer_link_probe(void *storage, size_t bytes,
                               const int16_t *render160,
                               const int16_t *microphone160,
                               const struct k7_apm21_timing *timing,
                               int16_t *output160);
#ifdef __cplusplus
}
#endif
#endif
