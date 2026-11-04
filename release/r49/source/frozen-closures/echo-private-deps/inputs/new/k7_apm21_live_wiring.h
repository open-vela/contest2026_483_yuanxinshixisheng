#ifndef K7_APM21_LIVE_WIRING_H
#define K7_APM21_LIVE_WIRING_H
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#if !defined(K7_APM21_PHYSICAL_STREAM) || !K7_APM21_PHYSICAL_STREAM
#error live observer requires the independently reviewed physical stream
#endif
#include "k7_apm21_physical_stream.h"
#include <stddef.h>
#include <stdint.h>

/* Private diagnostic observer only: no VAD/ASR/speaker callback or clean PCM
 * interface. A single PIO producer mirrors genuinely committed L/R pairs and
 * genuine MIC samples. A different single existing near worker consumes it.
 * Create before either thread; destroy only after both owners are joined.
 * No producer callback, lock, allocation, wait, logging or DSP. */
struct k7_apm21_pio_tap;
struct k7_voice_session;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#if !K7_APM21_DIAGNOSTIC_EVIDENCE || !K7_APM21_IDLE_EVENTS || !K7_APM21_CONSUMED_REFERENCE || !K7_APM21_PREPARED_LIFECYCLE
#error joint continuation requires all prepared diagnostic/source features
#endif
struct k7_apm21_cut_descriptor {
 uint64_t serial,owner,tap_epoch,reference_instance;
 uint64_t first,written,cut,published_upper,before_us,after_us;
 uint32_t reference_epoch,generation,input_epoch,clock_epoch,remaining_words;
 unsigned prefix_samples;int16_t prefix_pcm[160];uint8_t prefix_zero_bits[20];
};
/* Worker-authorized retirement, not a request to reset a failed stream.
 * The caller proves: source released, route tail released, reply/speaker
 * retired, real TX idle, and RX still live. PIO consumes this ticket once. */
#define K7_APM21_ROUND_RETIRED_CHECKS 31u
struct k7_apm21_round_ticket {
 uint64_t serial,owner,dsp_epoch,reply_lease;
 unsigned retired_checks;
 struct k7_apm21_cut_descriptor cut; /* unchanged physical reference axis */
};
/* PIO only: 1 ready, 0 wait for genuine RX to reach the existing 160 grid,
 * negative rejects. The pump adds a finite time/sample deadline. */
int k7_apm21_tap_start_gate(struct k7_apm21_pio_tap *,uint64_t mic);
int k7_apm21_tap_round_waiting(struct k7_apm21_pio_tap *);
void k7_apm21_tap_stop_pending(struct k7_apm21_pio_tap *,int turn_rc);
int k7_apm21_tap_pin_owner(struct k7_apm21_pio_tap *,uint64_t);
void k7_apm21_tap_reference_identity(struct k7_apm21_pio_tap *,uint64_t instance,uint32_t epoch,int status_rc);
int k7_apm21_tap_take_cut(struct k7_apm21_pio_tap *,uint64_t owner,struct k7_apm21_cut_descriptor *);
#endif

/* New private header only; no shared voice_session header/ABI edit. */
struct k7_apm21_pio_tap *k7_voice_session_apm21_tap(struct k7_voice_session *);
int k7_apm21_tap_create(struct k7_apm21_pio_tap **);
void k7_apm21_tap_destroy(struct k7_apm21_pio_tap *);
size_t k7_apm21_tap_storage_bytes(void);
/* PIO only. prepare is BEFORE the actual <=8-pair hardware prefill; on is
 * AFTER TXS enable and successful streams_started. RX has not advanced between
 * these calls. Failed prefill/enable/clock/half-write/stop calls gap instead.
 * These are software FIFO/RX observations, NOT a DAC timestamp/alignment. */
void k7_apm21_tap_prepare(struct k7_apm21_pio_tap *,uint64_t mic_first);
void k7_apm21_tap_on(struct k7_apm21_pio_tap *,uint64_t mic_first,
 uint64_t before_us,uint64_t after_us);
void k7_apm21_tap_pair(struct k7_apm21_pio_tap *,int16_t sample,unsigned explicit_zero,
 uint64_t before_us,uint64_t after_us);
void k7_apm21_tap_microphone(struct k7_apm21_pio_tap *,int16_t,uint64_t mic_index);
void k7_apm21_tap_gap(struct k7_apm21_pio_tap *,int reason);
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
void k7_apm21_tap_reference_begin(struct k7_apm21_pio_tap *,uint32_t generation,uint32_t input,uint32_t clock,uint64_t now);
void k7_apm21_tap_reference_observe(struct k7_apm21_pio_tap *,uint32_t generation,uint32_t input,uint32_t clock,unsigned remaining_words,int observed,uint64_t now,unsigned final_cut);
struct k7_apm21_reference_status {uint64_t published_next,final_cut;unsigned sealed;int fault;};
void k7_apm21_tap_reference_status(struct k7_apm21_pio_tap *,struct k7_apm21_reference_status *);
#endif


/* Caller must pin this binding to reviewed complete TARGET D128 evidence AND
 * separately reviewed genuine FIFO/RX mapping + measured buffer delay/config.
 * This draft ships NO binding/verified receipt. Nonzero digests merely identify
 * evidence, they do not verify it. NULL/default binding never constructs APM.
 * Physical/acoustic delay is independent of the algorithmic D128 profile. */
struct k7_apm21_live_binding {
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 unsigned product_experiment; /* Exactly1, separately explicit one-shot arm. */
#endif
 struct k7_apm21_stream_profile profile;
 struct k7_apm21_timing timing;
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 unsigned diagnostic_hypothesis; /* Exactly1 is explicitly armed, not clean authority. */
#endif
#line 43
 unsigned char source_mapping_receipt_sha256[32];
};
#define K7_APM21_LIVE_PENDING 8u
struct k7_apm21_live_block {
 uint64_t first;
 int16_t pcm[160];
 uint8_t explicit_zero_bits[20];
};
struct k7_apm21_live_worker {
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 uint64_t mic_origin,reference_origin;
#endif
 struct k7_apm21_pio_tap *tap;
 struct k7_apm21_physical_stream stream;
 struct k7_apm21_live_binding binding;
 struct k7_apm21_live_block mic[K7_APM21_LIVE_PENDING],ref[K7_APM21_LIVE_PENDING];
 struct k7_apm21_stream_output last_output;
 uint64_t owner,epoch,reply_lease,reply_high_water,mic_next,ref_next;
 uint64_t accepted_frames,output_frames,warmup_frames,unverified_epochs,gaps;
 unsigned revision,mic_head,mic_count,ref_head,ref_count,binding_valid,initialized;
 int last_error;
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 int (*output_sink)(void *,const struct k7_apm21_stream_output *,const struct k7_apm21_live_block *,const struct k7_apm21_live_block *);
 void *output_sink_arg;
#endif
#line 61
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 uint64_t tap_epoch,pending_half_lease;
 struct k7_apm21_live_block pending_mic,pending_ref,output_mic,output_ref;
 unsigned prepared_lifecycle,origin_bound,paused,pending_pair_valid,output_pair_valid;
 /* Pair validity is only nominal PCM lineage, never NE/wake authorization. */
#endif
#line 61
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 uint64_t raw_reference_origin,reference_bias,round_serial,pending_reply_lease;
 unsigned round_pending;
 struct k7_apm21_cut_descriptor retired_cut;
#endif
};
/* All functions below ONLY on the one existing near worker, including close.
 * init does no DSP/heap work. drain uses at most budget records, no callback.
 * No retry, hidden restart or raw fallback. The separately gated product
 * poststop helper can provide explicitly hypothetical digital reference zeros
 * with genuine RX successors; it never pads microphone or API output tails.
 * Last output is private observer storage, always qualified0. It is invalid
 * after ANY gap/reply change/close; its coordinates remain nominal, not current
 * MIC. Adaptive APM state lineage is stream owner+dsp_epoch, not finite support. */
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
int k7_apm21_live_binding_valid(const struct k7_apm21_live_binding *);
int k7_apm21_live_worker_prepare(struct k7_apm21_live_worker *,uint64_t,
 const struct k7_apm21_live_binding *);
int k7_apm21_live_worker_attach(struct k7_apm21_live_worker *,struct k7_apm21_pio_tap *);
int k7_apm21_live_worker_pause(struct k7_apm21_live_worker *);
#endif
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
int k7_apm21_live_worker_set_output_sink(struct k7_apm21_live_worker *,
 int (*)(void *,const struct k7_apm21_stream_output *,const struct k7_apm21_live_block *,const struct k7_apm21_live_block *),void *);
#endif
int k7_apm21_live_worker_init(struct k7_apm21_live_worker *,struct k7_apm21_pio_tap *,
#line 76
#line 69
 uint64_t owner,const struct k7_apm21_live_binding *);
int k7_apm21_live_worker_reply(struct k7_apm21_live_worker *,uint64_t next_lease);
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
int k7_apm21_live_worker_round_retire(struct k7_apm21_live_worker *,const struct k7_apm21_round_ticket *);
int k7_apm21_live_worker_map_reference(const struct k7_apm21_live_worker *,uint64_t raw,uint64_t *mapped);
int k7_apm21_live_worker_map_cut(const struct k7_apm21_live_worker *,const struct k7_apm21_cut_descriptor *raw,struct k7_apm21_cut_descriptor *mapped);
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
/* Worker only after joined proof; genuine prefix plus explicitly hypothetical
 * stopped digital excitation. No microphone or APM API tail padding. */
int k7_apm21_live_worker_poststop(struct k7_apm21_live_worker *,const struct k7_apm21_cut_descriptor *);
#endif
int k7_apm21_live_worker_drain(struct k7_apm21_live_worker *,unsigned budget);
void k7_apm21_live_worker_close(struct k7_apm21_live_worker *);
#endif
#endif
