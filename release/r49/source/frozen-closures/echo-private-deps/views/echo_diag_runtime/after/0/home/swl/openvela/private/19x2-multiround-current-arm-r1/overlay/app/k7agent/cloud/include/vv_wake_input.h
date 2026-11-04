#ifndef VV_WAKE_INPUT_H
#define VV_WAKE_INPUT_H
#include "vv_confirmed_wake.h"
#include "voice_frame_ring.h"
#define VV_WAKE_STALE (-21101)
#define VV_WAKE_HISTORY_FRAMES 80u
#define VV_WAKE_STAGING_FRAMES 256u
struct vv_wake_input;
struct vv_wake_request {
  uint64_t id, sample_first, keyword_end, created_ms;
  struct vv_confirmed_wake_context context;
};
int vv_wake_input_create(struct vv_wake_input **);
void vv_wake_input_destroy(struct vv_wake_input *);
/* KWS's existing single consumer copies real frames here BEFORE decoding.
 * Never read the ASR fanout as a second consumer. Only idle/no-TX frames are
 * eligible; PHOTO/TRACK/reply preserve their original paths. */
int vv_wake_input_frame(struct vv_wake_input *,
  const struct vv_confirmed_wake_context *,enum vv_wake_activity,
  const struct k7_voice_frame *,uint64_t now_ms);
/* Any configured wake/skin/stop label (short or full) is merely a candidate.
 * Called after the very same KWS frame was staged. No motion/cancel action. */
int vv_wake_input_candidate(struct vv_wake_input *,
  const struct vv_confirmed_wake_context *,uint64_t keyword_end,uint64_t now_ms);
/* Serial service claims once before cloud connect. take is the SOLE source
 * for this entire ASR task, including pre-roll and subsequent frames; do not
 * splice to the live ASR ring. No padding/replay if continuity is lost.
 * Producer continues while TLS opens. Full staging -> abort this task.
 * All copies are bounded under a mutex on workers, never PIO. */
int vv_wake_input_claim(struct vv_wake_input *,
  const struct vv_confirmed_wake_context *current,enum vv_wake_activity,
  uint64_t now_ms,struct vv_wake_request *);
int vv_wake_input_health(struct vv_wake_input *,const struct vv_wake_request *,
  uint64_t now_ms);
int vv_wake_input_take(struct vv_wake_input *,const struct vv_wake_request *,
  uint64_t now_ms,struct k7_voice_frame *);
/* finish closes this ticket, discards its queued tail, and re-arms fresh
 * history. Caller may issue a gate event only if transport/protocol, actual
 * ASR final, normal capture close, and this finish all succeeded. */
int vv_wake_input_finish(struct vv_wake_input *,const struct vv_wake_request *,
  uint64_t now_ms,uint64_t *first,uint64_t *next);
void vv_wake_input_reset(struct vv_wake_input *);
#endif
