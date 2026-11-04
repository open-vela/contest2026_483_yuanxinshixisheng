#ifndef VV_BARGE_ROUTE_H
#define VV_BARGE_ROUTE_H
#include "vv_cancel_tail.h"
#ifdef __cplusplus
extern "C" {
#endif
struct vv_barge_route;
struct vv_barge_onset_support {
 uint64_t evidence_id,input_first,input_next,advance_first,advance_next;
 uint64_t completing_frame_first,completing_frame_next;
};
/* The acoustic worker owns its independently qualified clean/model/alignment
 * registry. No provider is supplied here. attach borrows the route; detach
 * MUST stop and join every observer before returning zero. Failed attach must
 * retain no observer/borrow; the caller will not destroy unknown live workers.
 * A failed detach
 * retains the platform and every dependency. Never call from PIO/KWS realtime
 * callbacks: this is a serialized worker interface, not a second RX consumer. */
struct vv_barge_driver {
 struct vv_barge_authority authority;
 struct vv_cancel_tail_transform tail;
 /* Under authority pin, validate exact model output/near-end support. IDs and
  * ranges alone are not proof. No builtin implementation/default success. */
 int (*onset_support)(void *,const struct vv_confirmed_wake_context *,
   const struct vv_barge_onset_support *);
 int (*attach)(void *,struct vv_barge_route *);
 int (*detach)(void *,struct vv_barge_route *);
 void *arg;
};
struct vv_barge_route_config {
 struct vv_barge_authority authority;
 struct vv_cancel_tail_transform tail;
 /* Under authority pin, validate exact model output/near-end support. IDs and
  * ranges alone are not proof. No builtin implementation/default success. */
 int (*onset_support)(void *,const struct vv_confirmed_wake_context *,
   const struct vv_barge_onset_support *);
 /* Actual owner/RX/input/TX/mode observation, not acoustic qualification.
  * Called under route -> input -> authority locks; no reverse entry. */
 int (*observe)(void *,struct vv_barge_observation *);
 void *arg;
};
int vv_barge_route_create(const struct vv_barge_route_config *,struct vv_barge_route **);
/* Serial service opens only a scoped natural-answer window, never PHOTO/cue. */
int vv_barge_route_begin(struct vv_barge_route *,struct vv_reply_scope *,struct vv_dialogue *);
void vv_barge_route_end(struct vv_barge_route *);
/* Identity only: clean/model/alignment remain ZERO for the real provider to
 * bind independently. NOT a qualification token. */
int vv_barge_route_context(struct vv_barge_route *,struct vv_confirmed_wake_context *);
int vv_barge_route_clean(struct vv_barge_route *,const struct vv_confirmed_wake_context *,
 enum vv_barge_material,const struct k7_voice_frame *);
/* onset is the frame-aligned retained PCM range. The authority must separately
 * retain the exact (possibly non-frame-aligned) VAD support and reject invented
 * coverage. This route does not round a 512-sample model window into proof. */
int vv_barge_route_onset(struct vv_barge_route *,uint64_t first,uint64_t next,
 const struct vv_barge_onset_support *);
/* After cancel every physical RAW frame is held, transformed with independent
 * continuity proof, and accepted by the SAME queue. No raw fallback. */
int vv_barge_route_tail(struct vv_barge_route *,const struct vv_confirmed_wake_context *,
 const struct k7_voice_frame *,uint64_t now_ms);
int vv_barge_route_tail_pump(struct vv_barge_route *,uint64_t now_ms);
/* Serial listen_admit: 2=no interrupted utterance; 0=waiting bounded retirement
 * or tail; 1=exact source armed into PF; negative=abort, no raw fallback.
 * Unknown retirement keeps OWNER_UNSAFE. Does not itself open network IO. */
int vv_barge_route_admit(struct vv_barge_route *);
int vv_barge_route_release(struct vv_barge_route *);
/* Closing rejects future calls; caller then joins driver, only then destroy.
 * An armed/live PF source prevents destruction. No hidden join/detach. */
void vv_barge_route_close(struct vv_barge_route *);
int vv_barge_route_destroy(struct vv_barge_route *);
#ifdef __cplusplus
}
#endif
#endif

#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
int vv_barge_route_rearm_ready(struct vv_barge_route *);
#endif
