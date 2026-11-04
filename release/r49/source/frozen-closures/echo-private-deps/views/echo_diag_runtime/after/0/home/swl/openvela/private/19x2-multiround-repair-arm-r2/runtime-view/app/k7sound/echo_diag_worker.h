/* Private worker-only diagnostic candidate. Never a verified AEC/KWS source. */
#ifndef K7_ECHO_DIAG_WORKER_H
#define K7_ECHO_DIAG_WORKER_H
#include "fifo_reference.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define K7_ECHO_DIAG_FRAME 320u
#define K7_ECHO_DIAG_CAPACITY 8192u
struct k7_echo_diag;
struct k7_echo_diag_mic_id { uint64_t owner; uint32_t epoch; };
struct k7_echo_diag_pair {
  uint64_t reference_instance, fifo_first, mic_first, mic_owner;
  uint32_t reference_epoch, generation, mic_epoch;
  uint8_t explicit_zero_bits[40];
  int16_t microphone[320], reference[320];
  bool hypothesis_only; /* Always true. Never feed a control/action consumer. */
};
/* One worker owns this object, serialized; never call in PIO/ISR or concurrently.
 * Producer rings belong to their own SPSC owners. This object does not retain
 * a session or owner pointer. Caller must pin/join the actual parent separately.
 * At most 320 samples per push and 320 per pop; no waits or callbacks.
 * 8192 samples per channel (~512 ms) is diagnostic backpressure only. Overflow
 * revokes this hypothesis; it MUST NOT cancel playback or block its PIO owner. */
int k7_echo_diag_create(struct k7_echo_diag **);
void k7_echo_diag_destroy(struct k7_echo_diag *);
size_t k7_echo_diag_storage_bytes(void);
/* Explicitly supplied same-turn HYPOTHESIS:
 *   mic_index = mic_anchor + (FIFO_index - fifo_anchor).
 * Anchors are not inferred from equal numeric indices or enqueue timestamps.
 * BEGIN is from the actual FIFO queue; MIC identity is its actual capture ID.
 * Older/same reference instance+epoch cannot be reactivated after a gap.
 * This interface can validate digital continuity only, not acoustic alignment. */
int k7_echo_diag_begin(struct k7_echo_diag *,
  const struct k7_fifo_ref_record *begin,
  const struct k7_fifo_ref_status *current,
  struct k7_echo_diag_mic_id mic, uint64_t fifo_anchor, uint64_t mic_anchor);
int k7_echo_diag_reference(struct k7_echo_diag *,
  const struct k7_fifo_ref_record *, const struct k7_fifo_ref_status *current);
int k7_echo_diag_microphone(struct k7_echo_diag *, struct k7_echo_diag_mic_id,
  uint64_t first, const int16_t *, size_t samples);
/* 1 complete pair, 0 wait for actual data, negative revoked. Recheck current
 * FIFO status and actual MIC owner/epoch immediately before every pop. END,
 * clear, revocation, discontinuity or exhaustion invalidate all queued output.
 * A copied pair still is only a diagnostic hypothesis, even with fresh status. */
int k7_echo_diag_pop(struct k7_echo_diag *,
  const struct k7_fifo_ref_status *current, struct k7_echo_diag_mic_id,
  struct k7_echo_diag_pair *);
void k7_echo_diag_invalidate(struct k7_echo_diag *);
bool k7_echo_diag_active(const struct k7_echo_diag *);
#ifdef __cplusplus
}
#endif
#endif
