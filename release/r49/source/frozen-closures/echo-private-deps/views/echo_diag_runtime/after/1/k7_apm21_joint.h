#ifndef K7_APM21_JOINT_H
#define K7_APM21_JOINT_H
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#include "k7_apm21_evidence_bridge.h"
#include "k7_apm21_idle_events.h"
#include "tail_reference_archive.h"
struct k7_apm21_joint {
 struct ne_identity identity;
 struct vv_confirmed_wake_context context;
 struct tail_reference_archive archive;
 struct k7_apm21_cut_descriptor cut;
 struct k7_apm21_idle_event off,disabled,quiet,source_end;
 uint64_t tap_epoch,last_idle_serial,idle_first,idle_next;
 unsigned begun,have_cut,have_off,have_disabled,have_quiet,have_end,idle_frames,transport_verified;
 unsigned hypothesis_enabled;
 int failure,tail_result;
 unsigned prompt_only; /* Physical control audio; no dialogue/context authority. */
};
int k7_apm21_joint_begin(struct k7_apm21_joint *,const struct ne_identity *,const struct vv_confirmed_wake_context *,uint64_t tap_epoch);
/* Only physical fields are populated; no synthetic dialogue/scope/reply ID. */
int k7_apm21_joint_prompt_begin(struct k7_apm21_joint *,uint64_t owner,
 const struct k7_fifo_ref_record *,uint32_t input,uint32_t clock,uint64_t tap_epoch);
int k7_apm21_joint_anchor(struct k7_apm21_joint *,const struct k7_voice_anchor *);
int k7_apm21_joint_record(struct k7_apm21_joint *,const struct k7_fifo_ref_record *);
int k7_apm21_joint_idle(struct k7_apm21_joint *,const struct k7_apm21_idle_event *);
int k7_apm21_joint_cut(struct k7_apm21_joint *,const struct k7_apm21_cut_descriptor *);
/* Returns1 only matching immutable transport sources. This is NOT a tail
 * authority, not acoustic zero, and not permission to create reference PCM. */
int k7_apm21_joint_reconcile(struct k7_apm21_joint *,const struct k7_apm21_live_worker *);
/* Original half/real32 state must remain complete. Negative rejects instead
 * of repairing an unresolved canceled boundary or filling its last half. */
int k7_apm21_joint_tail_ready(struct k7_apm21_joint *,const struct k7_apm21_live_worker *,const struct k7_apm21_evidence_bridge *);
int k7_apm21_joint_continue(struct k7_apm21_joint *,struct k7_apm21_live_worker *,struct k7_apm21_evidence_bridge *);
int k7_apm21_joint_live_ready(const struct k7_apm21_joint *,const struct k7_apm21_live_worker *,const struct k7_apm21_evidence_bridge *);
#endif
#endif
