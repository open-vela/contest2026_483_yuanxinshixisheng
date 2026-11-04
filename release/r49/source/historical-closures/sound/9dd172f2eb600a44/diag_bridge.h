#ifndef DIAG_CONTINUITY_BRIDGE_H
#define DIAG_CONTINUITY_BRIDGE_H
#include "k7_aec.h"
#include "voice_frame_ring.h"
#include "vv_confirmed_wake.h"
struct vv_pf_wake_source;
#include "vv_cancel_tail.h"
/* Private diagnostic-only adapter. Single serialized worker; callers join it
 * before destroy. No acoustic driver, authority registration, ASR source or
 * automatic action exists. Supplied reference remains a mapping hypothesis. */
struct diag_bridge;
struct diag_preroll { struct k7_voice_frame frames[16]; unsigned count,qualified; };
int diag_bridge_create(struct diag_bridge **);
int diag_bridge_begin(struct diag_bridge *,const struct vv_confirmed_wake_context *,
 const struct k7_aec_continuity_identity *,uint64_t,uint64_t,uint64_t *);
int diag_bridge_process(struct diag_bridge *,uint64_t,
 const struct k7_aec_continuity_identity *,const struct k7_voice_frame *,
 const int16_t reference[320],uint64_t,struct k7_voice_frame *);
int diag_bridge_seal(struct diag_bridge *,uint64_t,const struct vv_barge_ticket *,uint64_t *);
int diag_bridge_preroll(struct diag_bridge *,uint64_t,struct diag_preroll *);
/* Conforms to actual tail callback signature but intentionally NEVER grants
 * clean output. ENOTSUP makes the real tail/route fail closed, not RAW retry. */
int diag_bridge_tail_unqualified(void *,const struct vv_barge_ticket *,
 const struct k7_voice_frame *,struct k7_voice_frame *,enum vv_barge_material *);
/* Pure software diagnostic observation; no forged qualified barge ticket. */
int diag_bridge_boundary(struct diag_bridge *,uint64_t,
 const struct vv_confirmed_wake_context *,uint64_t mic_next,uint64_t ref_next,uint64_t *);
int diag_bridge_invalidate(struct diag_bridge *,int reason);
int diag_bridge_status(struct diag_bridge *,struct k7_aec_continuity_status *,struct k7_aec_stats *);
void diag_bridge_destroy(struct diag_bridge *);

#if defined(K7_ECHO_AEC10_ADAPTER) && K7_ECHO_AEC10_ADAPTER
#include "k7_aec_frame_adapter.h"
/* All samples/zero flags are copied original nominal data. Return1 publishes
 * complete320; return0 is genuine startup without a complete output. */
struct diag_bridge_paired_output {
 struct k7_voice_frame raw,residual;
 int16_t reference[320];uint8_t zero[320];
 struct k7_aec_fa_identity identity;
 struct k7_aec_fa_support support;
 uint64_t reference_first,backend_state_epoch;
 uint64_t accepted_mic_next,accepted_ref_next,published_mic_next,published_ref_next;
 uint64_t startup_unavailable_samples;
 unsigned backend_delay_samples,publication_delay_samples,source_stop_zero_samples;
 unsigned qualified; /* Always0: wiring is not acoustic trust. */
};
int diag_bridge_process_paired(struct diag_bridge *,uint64_t,
 const struct k7_aec_continuity_identity *before,const struct k7_aec_continuity_identity *after,
 const struct k7_voice_frame *,const int16_t reference[320],uint64_t,
 const uint8_t reference_zero[320],unsigned reference_valid,struct diag_bridge_paired_output *);
int diag_bridge_aec10_status(struct diag_bridge *,uint64_t,struct k7_aec_fa_status *);
#endif
#endif
