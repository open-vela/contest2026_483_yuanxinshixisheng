#ifndef VV_CANCEL_TAIL_H
#define VV_CANCEL_TAIL_H
#include "vv_barge_input.h"
#ifdef __cplusplus
extern "C" {
#endif
#define VV_CANCEL_TAIL_FRAMES 64u
#define VV_CANCEL_TAIL_WAIT_MS 1500u
struct vv_cancel_tail;
struct vv_cancel_tail_transform {
  void *arg;
  /* Actual worker monotonic clock, checked before and after transformation.
   * UINT64_MAX/rollback/expired budget abort; this grants no audio authority. */
  uint64_t (*now_ms)(void *);
  /* Independent, qualified worker transform, absent by default. Input is RAW
   * and may contain unknown amplifier/codec/acoustic tail. This callback must
   * NEVER return raw input as clean merely because TX retired or time passed.
   * Return 0 only for the same exact physical range with independently valid
   * calibration/near-end preservation; -EAGAIN while unknown, other <0 abort.
   * Existing barge authority revalidates this range before queue acceptance.
   * Bounded and non-reentrant: may not destroy/reset this buffer or queue;
   * its qualification dependencies must stay pinned through the callback.
   * No production transform/qualified registry is provided by this draft. */
  int (*frame)(void *,const struct vv_barge_ticket *,const struct k7_voice_frame *,
               struct k7_voice_frame *,enum vv_barge_material *);
};
/* Single serialized worker only (not PIO/KWS callback). The manager's single
 * RX consumer must copy frames into a nonblocking upstream queue; it must not
 * wait for this transform. Caller retains q and joins workers before destroy.
 * 64 complete raw frames are bounded staging, never an ASR input API. */
int vv_cancel_tail_create(struct vv_barge_input *q,const struct vv_barge_ticket *,
                          uint64_t now_ms,struct vv_cancel_tail **);
int vv_cancel_tail_append(struct vv_cancel_tail *,const struct vv_confirmed_wake_context *,
                          const struct k7_voice_frame *,uint64_t now_ms);
/* At most one frame per call. 1 forwarded, 0 no pending frame, -EAGAIN waiting
 * for independent qualification. Unknown data is retained and never padded. */
int vv_cancel_tail_forward(struct vv_cancel_tail *,const struct vv_cancel_tail_transform *,uint64_t now_ms);
/* Serial worker hands over at an empty boundary only. Future frames must use
 * the SAME qualified lineage and exact next physical index through existing
 * barge frame ingress. This lifts a staging barrier, not acoustic authority. */
int vv_cancel_tail_finish(struct vv_cancel_tail *,uint64_t now_ms,uint64_t *first,uint64_t *next);
void vv_cancel_tail_destroy(struct vv_cancel_tail *);
size_t vv_cancel_tail_storage_bytes(void);
#ifdef __cplusplus
}
#endif
#endif
