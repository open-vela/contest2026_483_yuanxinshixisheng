#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#ifndef VV_BARGE_INPUT_H
#define VV_BARGE_INPUT_H
#include "vv_paraformer_k7.h"
#include "vv_reply_scope.h"
#ifdef __cplusplus
extern "C" {
#endif
struct vv_dialogue;
struct vv_barge_input;
#define VV_BARGE_HISTORY_FRAMES 32u
#define VV_BARGE_PRE_ROLL_SAMPLES 5120u
#define VV_BARGE_STAGING_FRAMES 256u
#define VV_BARGE_TICKET_MS 500u
#define VV_BARGE_RETIRE_MS 5000u
#define VV_BARGE_CAPTURE_MS 12000u
enum vv_barge_material {
  VV_BARGE_RAW_UNKNOWN = 0,
  VV_BARGE_QUALIFIED_CLEAN = 1,
  /* Same calibrated stream lineage through mute/tail. NOT raw MIC merely
   * because TX stopped. A new epoch or unknown tail must abort the ticket. */
  VV_BARGE_QUALIFIED_MUTED_CONTINUOUS = 2,
  VV_BARGE_EXPERIMENTAL_RESIDUAL = 3, VV_BARGE_EXPERIMENTAL_TAIL = 4
};
/* Explicit controlled experiment materials never imply calibrated clean. */
static inline bool vv_barge_material_clean(enum vv_barge_material m){
 return m==VV_BARGE_QUALIFIED_CLEAN
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 ||m==VV_BARGE_EXPERIMENTAL_RESIDUAL
#endif
 ;}
static inline bool vv_barge_material_tail(enum vv_barge_material m){
 return m==VV_BARGE_QUALIFIED_MUTED_CONTINUOUS
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 ||m==VV_BARGE_EXPERIMENTAL_TAIL
#endif
 ;}
enum vv_barge_check {
  VV_BARGE_CHECK_FRAME, VV_BARGE_CHECK_ONSET, VV_BARGE_CHECK_CANCEL,
  VV_BARGE_CHECK_CLAIM, VV_BARGE_CHECK_CAPTURE
};
/* Independent onset event: no full_wake, keyword or raw-energy authorization.
 * model_epoch identifies the qualified near-end VAD, not the short KWS model.
 * alignment_epoch is the independent calibration identity. All original reply
 * identities remain fixed after cancellation; live TX is separately observed. */
struct vv_barge_ticket {
  uint64_t id, onset_first, onset_next, created_ms;
  struct vv_confirmed_wake_context context;
};
struct vv_barge_observation {
  uint64_t owner_epoch, microphone_next, now_ms;
  uint32_t input_epoch, mode_epoch, live_tx_generation;
  bool rx_live, photo_busy, tx_active;
};
struct vv_barge_authority {
  void *arg;
  /* Default NULL is unavailable: create works, all admission fails ENOTSUP.
   * enter must independently validate exact clean/calibration/VAD lineage and
   * the requested range, then pin actual owner, mode/PHOTO, qualification and
   * live TX observations until leave. ONSET/CANCEL additionally require the
   * near-end onset range to be qualified (not echo-only/energy/KWS evidence).
   * CLAIM/CAPTURE require verified post-cancel tail continuity for this lineage.
   * Return 0 with a held pin, negative with NO pin. No nonzero-ID heuristic.
   * Calls run under this module's mutex, are bounded/non-reentrant, and may
   * not wait for audio/network. Future integration must keep lock order
   * queue -> authority -> scope; never tap while holding authority locks.
   * No production authority or calibrated model is supplied by this draft. */
  int (*enter)(void *, const struct vv_confirmed_wake_context *,
               enum vv_barge_check, enum vv_barge_material,
               uint64_t first, uint64_t next, struct vv_barge_observation *);
  void (*leave)(void *);
};
int vv_barge_input_create(const struct vv_barge_authority *,struct vv_barge_input **);
/* Caller stops/join all producers/observers first. Claimed PF source returns
 * EBUSY until finish, even after invalidation; no detached borrowed pointer. */
int vv_barge_input_destroy(struct vv_barge_input *);
void vv_barge_input_abort(struct vv_barge_input *);
/* New history only after the serial owner has disposed the previous ticket.
 * Claimed source cannot reset. No generation/sequence counter is reused. */
int vv_barge_input_reset(struct vv_barge_input *);
int vv_barge_input_frame(struct vv_barge_input *,
 const struct vv_confirmed_wake_context *,enum vv_barge_material,
 const struct k7_voice_frame *);
int vv_barge_input_onset(struct vv_barge_input *,uint64_t onset_first,
 uint64_t onset_next,struct vv_barge_ticket *);
/* Caller pins actual platform/dialogue/scope lifetimes. Only this exact
 * speaking reply can be cancelled. Event consumed once BEFORE the CAS;
 * completed/stale/failed cancellation cannot be replayed onto another reply.
 * No gimbal/PHOTO/mode action. Return 1 accepted, 0 already consumed/stale. */
int vv_barge_input_cancel(struct vv_barge_input *,const struct vv_barge_ticket *,
 struct vv_reply_scope *,struct vv_dialogue *);
/* Called by the serial service after old synchronous reply returns. Scope
 * must be CLOSED with exact TX retired + all workers joined + live RX/handoff.
 * EAGAIN means still retiring; unknown retirement is OWNER_UNSAFE. No timeout
 * releases uncertain resources. Successful source is the SOLE input for the
 * entire next PF task, never spliced to raw MIC/ordinary ASR fanout. Caller
 * pins this queue until PF finish/disarm, including arm-failure cleanup.
 * No scope pointer is retained. Return 1 on claim. */
/* Data-preservation barrier only, never a clean-audio proof. Serial worker
 * sets this for the exact pending/cancelled ticket BEFORE it cancels TX.
 * While deferred, claim cannot hand incomplete staging to ASR. Resume checks
 * every held sample was committed continuously; ordinary authority/scope
 * checks still apply. Caller pins the queue through its tail-buffer lifetime. */
int vv_barge_input_tail_defer(struct vv_barge_input *,const struct vv_barge_ticket *);
int vv_barge_input_tail_resume(struct vv_barge_input *,const struct vv_barge_ticket *,uint64_t committed_next);
int vv_barge_input_claim(struct vv_barge_input *,struct vv_reply_scope *,
 struct vv_pf_wake_source *);
#ifdef __cplusplus
}
#endif
#endif

#else
#ifndef VV_BARGE_INPUT_H
#define VV_BARGE_INPUT_H
#include "vv_paraformer_k7.h"
#include "vv_reply_scope.h"
#ifdef __cplusplus
extern "C" {
#endif
struct vv_dialogue;
struct vv_barge_input;
#define VV_BARGE_HISTORY_FRAMES 32u
#define VV_BARGE_PRE_ROLL_SAMPLES 5120u
#define VV_BARGE_STAGING_FRAMES 256u
#define VV_BARGE_TICKET_MS 500u
#define VV_BARGE_RETIRE_MS 5000u
#define VV_BARGE_CAPTURE_MS 12000u
enum vv_barge_material {
  VV_BARGE_RAW_UNKNOWN = 0,
  VV_BARGE_QUALIFIED_CLEAN = 1,
  /* Same calibrated stream lineage through mute/tail. NOT raw MIC merely
   * because TX stopped. A new epoch or unknown tail must abort the ticket. */
  VV_BARGE_QUALIFIED_MUTED_CONTINUOUS = 2
};
enum vv_barge_check {
  VV_BARGE_CHECK_FRAME, VV_BARGE_CHECK_ONSET, VV_BARGE_CHECK_CANCEL,
  VV_BARGE_CHECK_CLAIM, VV_BARGE_CHECK_CAPTURE
};
/* Independent onset event: no full_wake, keyword or raw-energy authorization.
 * model_epoch identifies the qualified near-end VAD, not the short KWS model.
 * alignment_epoch is the independent calibration identity. All original reply
 * identities remain fixed after cancellation; live TX is separately observed. */
struct vv_barge_ticket {
  uint64_t id, onset_first, onset_next, created_ms;
  struct vv_confirmed_wake_context context;
};
struct vv_barge_observation {
  uint64_t owner_epoch, microphone_next, now_ms;
  uint32_t input_epoch, mode_epoch, live_tx_generation;
  bool rx_live, photo_busy, tx_active;
};
struct vv_barge_authority {
  void *arg;
  /* Default NULL is unavailable: create works, all admission fails ENOTSUP.
   * enter must independently validate exact clean/calibration/VAD lineage and
   * the requested range, then pin actual owner, mode/PHOTO, qualification and
   * live TX observations until leave. ONSET/CANCEL additionally require the
   * near-end onset range to be qualified (not echo-only/energy/KWS evidence).
   * CLAIM/CAPTURE require verified post-cancel tail continuity for this lineage.
   * Return 0 with a held pin, negative with NO pin. No nonzero-ID heuristic.
   * Calls run under this module's mutex, are bounded/non-reentrant, and may
   * not wait for audio/network. Future integration must keep lock order
   * queue -> authority -> scope; never tap while holding authority locks.
   * No production authority or calibrated model is supplied by this draft. */
  int (*enter)(void *, const struct vv_confirmed_wake_context *,
               enum vv_barge_check, enum vv_barge_material,
               uint64_t first, uint64_t next, struct vv_barge_observation *);
  void (*leave)(void *);
};
int vv_barge_input_create(const struct vv_barge_authority *,struct vv_barge_input **);
/* Caller stops/join all producers/observers first. Claimed PF source returns
 * EBUSY until finish, even after invalidation; no detached borrowed pointer. */
int vv_barge_input_destroy(struct vv_barge_input *);
void vv_barge_input_abort(struct vv_barge_input *);
/* New history only after the serial owner has disposed the previous ticket.
 * Claimed source cannot reset. No generation/sequence counter is reused. */
int vv_barge_input_reset(struct vv_barge_input *);
int vv_barge_input_frame(struct vv_barge_input *,
 const struct vv_confirmed_wake_context *,enum vv_barge_material,
 const struct k7_voice_frame *);
int vv_barge_input_onset(struct vv_barge_input *,uint64_t onset_first,
 uint64_t onset_next,struct vv_barge_ticket *);
/* Caller pins actual platform/dialogue/scope lifetimes. Only this exact
 * speaking reply can be cancelled. Event consumed once BEFORE the CAS;
 * completed/stale/failed cancellation cannot be replayed onto another reply.
 * No gimbal/PHOTO/mode action. Return 1 accepted, 0 already consumed/stale. */
int vv_barge_input_cancel(struct vv_barge_input *,const struct vv_barge_ticket *,
 struct vv_reply_scope *,struct vv_dialogue *);
/* Called by the serial service after old synchronous reply returns. Scope
 * must be CLOSED with exact TX retired + all workers joined + live RX/handoff.
 * EAGAIN means still retiring; unknown retirement is OWNER_UNSAFE. No timeout
 * releases uncertain resources. Successful source is the SOLE input for the
 * entire next PF task, never spliced to raw MIC/ordinary ASR fanout. Caller
 * pins this queue until PF finish/disarm, including arm-failure cleanup.
 * No scope pointer is retained. Return 1 on claim. */
/* Data-preservation barrier only, never a clean-audio proof. Serial worker
 * sets this for the exact pending/cancelled ticket BEFORE it cancels TX.
 * While deferred, claim cannot hand incomplete staging to ASR. Resume checks
 * every held sample was committed continuously; ordinary authority/scope
 * checks still apply. Caller pins the queue through its tail-buffer lifetime. */
int vv_barge_input_tail_defer(struct vv_barge_input *,const struct vv_barge_ticket *);
int vv_barge_input_tail_resume(struct vv_barge_input *,const struct vv_barge_ticket *,uint64_t committed_next);
int vv_barge_input_claim(struct vv_barge_input *,struct vv_reply_scope *,
 struct vv_pf_wake_source *);
#ifdef __cplusplus
}
#endif
#endif

#endif
