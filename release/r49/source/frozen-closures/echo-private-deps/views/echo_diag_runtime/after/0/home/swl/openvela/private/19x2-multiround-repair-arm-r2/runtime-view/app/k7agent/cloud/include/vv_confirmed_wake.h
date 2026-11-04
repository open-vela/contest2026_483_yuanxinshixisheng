#ifndef VV_CONFIRMED_WAKE_H
#define VV_CONFIRMED_WAKE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Pure values, never a borrowed audio, platform or scope pointer. */
struct vv_confirmed_wake_context {
  uint64_t owner_epoch, model_epoch, clean_epoch, alignment_epoch;
  uint64_t scope_incarnation, lease_serial, reply_id;
  uint32_t input_epoch, mode_epoch, asr_epoch, tx_generation;
};
enum vv_confirmed_wake_source {
  VV_WAKE_IDLE_ASR_FINAL = 1, VV_WAKE_CLEAN_FULL_KWS = 2
};
struct vv_confirmed_wake_event {
  uint64_t event_id, sample_first, sample_next, confirmed_ms;
  struct vv_confirmed_wake_context context;
  enum vv_confirmed_wake_source source;
  bool full_wake, negated;
};
enum vv_wake_activity { VV_WAKE_IDLE, VV_WAKE_TRACK, VV_WAKE_PHOTO, VV_WAKE_REPLY };
struct vv_confirmed_wake_gate;
int vv_confirmed_wake_create(struct vv_confirmed_wake_gate **);
void vv_confirmed_wake_destroy(struct vv_confirmed_wake_gate *);
/* Called by the pinned owner outside PIO. verified_clean is an imported
 * calibration/clean-stream fact, not inferred from nonzero epoch fields.
 * Current raw-KWS path must pass false. Changing any identity invalidates
 * pending events. No callback, inference, playback, or mode action here. */
int vv_confirmed_wake_bind(struct vv_confirmed_wake_gate *,
  const struct vv_confirmed_wake_context *, enum vv_wake_activity, bool verified_clean);
void vv_confirmed_wake_invalidate(struct vv_confirmed_wake_gate *);
/* Full successful protocol/utterance completion only, never a KWS fragment or
 * partial ASR. IDLE preserves existing fuzzy final "你好" semantics.
 * CLEAN_FULL_KWS additionally requires exact whole wake phrase and the
 * independently bound clean/alignment proof. Existing KWS label alone cannot
 * supply utterance_complete, negation coverage or calibrated clean proof.
 * Returns 1 newly issued event, 0 rejected semantic/context, negative errno. */
int vv_confirmed_wake_final(struct vv_confirmed_wake_gate *,
  const struct vv_confirmed_wake_context *, enum vv_confirmed_wake_source,
  uint64_t sample_first, uint64_t sample_next, uint64_t now_ms,
  const char *text, size_t bytes, bool protocol_verified, bool utterance_complete,
  struct vv_confirmed_wake_event *);
/* Short non-reentrant callback compatible with manager/platform pins.
 * Exact private ticket, current identities and age checked before one-shot
 * consume. Returns 1 once, 0 stale/forged/expired, negative errno.
 * Consumption authorizes no mode action by itself; caller rechecks PHOTO
 * and target scope and performs its own atomic reply-ID cancellation. */
int vv_confirmed_wake_consume(void *gate,
  const struct vv_confirmed_wake_event *,
  const struct vv_confirmed_wake_context *current, uint64_t now_ms);
#endif
