#ifndef K7_APM21_EVIDENCE_BRIDGE_H
#define K7_APM21_EVIDENCE_BRIDGE_H
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
#if !defined(K7_APM21_PREPARED_LIFECYCLE) || !K7_APM21_PREPARED_LIFECYCLE
#error diagnostic evidence requires prepared lifecycle
#endif
#include "k7_apm21_live_wiring.h"
#include "near_evidence.h"
#include "echo_observer.h" /* Packet VALUE layout; no emit call in this module. */
#include "vv_confirmed_wake.h"
#define K7_APM21_EVIDENCE_HALVES 32u
#define K7_APM21_EVIDENCE_LEDGER 16u
struct k7_apm21_evidence_ledger {
 struct ne_identity identity;
 struct vv_confirmed_wake_context context;
 uint64_t mic_first,reference_first;
 int16_t mic[320],reference[320];
 unsigned cancel_observed;
};
struct k7_apm21_evidence_half {
 struct k7_apm21_stream_output y;
 struct k7_apm21_live_block mic,reference;
};
struct k7_apm21_evidence_bridge {
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 uint64_t poststop_cut;unsigned poststop_enabled; /* 1 cancel proof; 2 successful EOF proof. */
#endif
 struct ne_worker *near; /* EXCLUSIVE SAME worker/model; never a second model. */
 uint64_t owner,dsp_epoch,frames,windows,candidates,rejected,unsupported;
 uint64_t last_mic_next,last_ref_next;
 uint64_t source_mic_next,source_ref_next;
 unsigned initialized,ledger_head,ledger_count,half_head,half_count;
 unsigned have_first,have_identity,have_watermark,packet_valid;
 unsigned source_registered,cancel_noted;
 int failure;
 struct k7_apm21_evidence_ledger ledger[K7_APM21_EVIDENCE_LEDGER];
 struct k7_apm21_evidence_half halves[K7_APM21_EVIDENCE_HALVES];
 struct k7_apm21_evidence_half first;
 struct ne_identity identity;
 struct vv_confirmed_wake_context context,packet_context;
 struct k7_voice_frame raw,residual;
 int16_t reference[320];uint8_t zero[320];
 struct ne_report report;
 struct k7_echo_packet packet;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 int (*packet_sink)(void *,const struct k7_echo_packet *,const struct vv_confirmed_wake_context *);
 void *packet_sink_arg;
 /* Appended: all existing member offsets stay unchanged. A deferred sink
  * retains this exact computed packet and head half/ledger until delivery. */
 unsigned packet_pending;
#endif
};
/* All operations serialized on the SAME near worker. Fixed buffers only.
 * Default diagnostic never calls observer/route/cancel/ASR. Explicit product
 * installs a separately gated packet sink; neither mode grants clean
 * or physical calibration. The borrowed near/model must not also receive
 * Speex frames. Caller owns lifetime; init does not create/reset APM. */
int k7_apm21_evidence_init(struct k7_apm21_evidence_bridge *,uint64_t owner,
 uint64_t dsp_epoch,struct ne_worker *exclusive_near);
/* The existing cc_pop path supplies this BEFORE any provisional lag39 or
 * Speex transform, under its real continuity/context checks. No pointer borrow.
 * modeled_stop_samples!=0 requires explicit joined poststop hypothesis; it
 * is never relabeled actual FIFO-written or acoustically silent samples. */
int k7_apm21_evidence_register(struct k7_apm21_evidence_bridge *,
 const struct ne_identity *,const struct vv_confirmed_wake_context *,
 uint64_t mic_first,uint64_t reference_first,const int16_t mic[320],
 const int16_t reference[320],unsigned cancel_observed,unsigned modeled_stop_samples);
/* Worker output sink: enqueue every nominal160 with ORIGINAL cached raw/ref/
 * explicit-zero bits. y.reply_lease is only an advisory reply ID from the old
 * observer; historical authorization comes exclusively from the ledger. */
int k7_apm21_evidence_accept(void *,const struct k7_apm21_stream_output *,
 const struct k7_apm21_live_block *,const struct k7_apm21_live_block *);
/* Match axes AND full raw/ref samples to immutable ledger. Two adjacent160
 * must share ALL identity/context values. Real ne_begin/ne_feed use the SAME
 * original policy/model; retain actual576/512 supports. Return fed320 count.
 * Packet/report remain diagnostic values, qualified0/actions0. */
int k7_apm21_evidence_pump(struct k7_apm21_evidence_bridge *);
/* Numeric diagnostic boundary only: same immutable OLD source/context and
 * genuine accepted-source watermark required. Does not mint a bridge lease,
 * clean token, model reset, tail proof or cancellation action. */
int k7_apm21_evidence_note_cancel(struct k7_apm21_evidence_bridge *,
 const struct ne_identity *,const struct vv_confirmed_wake_context *,uint64_t,uint64_t);
/* Only original strict post-source-finish/rearm point may call this. It clears
 * model aggregation and stale model queues, not physical DSP/APM state. */
int k7_apm21_evidence_rearm(struct k7_apm21_evidence_bridge *,uint64_t owner);
void k7_apm21_evidence_invalidate(struct k7_apm21_evidence_bridge *,int reason);
#endif
#endif
