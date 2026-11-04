#ifndef K7_APM21_PHYSICAL_STREAM_H
#define K7_APM21_PHYSICAL_STREAM_H
#if defined(K7_APM21_PHYSICAL_STREAM) && K7_APM21_PHYSICAL_STREAM
#include "k7_apm21_consumer.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Private/default-OFF. One caller audio worker serializes ALL calls, including
 * construct/destroy. No thread, ADC owner, affinity, allocator or timer is added.
 * Fresh storage MUST be zero-initialized once; never memset a live/closed stream
 * to reuse an epoch. The context's APM heap is owned by the frozen consumer.
 */
enum k7_apm21_stream_state {
 K7_APM21_STREAM_EMPTY=0, K7_APM21_STREAM_ACTIVE=1,
 K7_APM21_STREAM_GAP=2, K7_APM21_STREAM_CLOSED=3
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 ,K7_APM21_STREAM_PREPARED=4
#endif
#line 18
};
enum k7_apm21_stream_result {
 K7_APM21_STREAM_WARMUP=0, K7_APM21_STREAM_OUTPUT=1,
 K7_APM21_STREAM_ARGUMENT=-19101, K7_APM21_STREAM_UNVERIFIED=-19102,
 K7_APM21_STREAM_BUSY=-19103, K7_APM21_STREAM_STALE=-19104,
 K7_APM21_STREAM_DISCONTINUITY=-19105, K7_APM21_STREAM_REPORT=-19106,
 K7_APM21_STREAM_OVERFLOW=-19107
};
enum k7_apm21_stream_profile_kind {
 K7_APM21_STREAM_PROFILE_UNKNOWN=0,
 K7_APM21_STREAM_PROFILE_TARGET_D128=1
};
struct k7_apm21_stream_profile {
 unsigned kind, measured_algorithmic_delay_samples;
 /* Caller must bind this to reviewed complete TARGET evidence. A nonzero hash
  * alone is NOT authorization, measurement, or acoustic qualification. This
  * module ships NO verified profile; tests use an explicitly fake receipt. */
 unsigned char target_receipt_sha256[32];
};
struct k7_apm21_stream_input {
 uint64_t owner, dsp_epoch, ordinal;
 uint64_t mic_first, reference_first;
 const int16_t *mic, *reference;
 unsigned samples; /* Exactly160 genuine samples in each buffer. */
 unsigned reference_complete; /* Exactly1 with caller-proved lineage. No implicit
  * zero, idle/AMP_OFF inference, gap filler, or reference synthesis is allowed. */
 const struct k7_apm21_timing *timing; /* Forward explicit source/delay unchanged. */
};
struct k7_apm21_stream_output {
 uint64_t owner,dsp_epoch,reply_lease,accepted_ordinal;
 uint64_t nominal_mic_first,nominal_reference_first;
 uint64_t available_after_mic_next,available_after_reference_next;
 unsigned samples,algorithmic_delay_samples,extra_fifo_samples,qualified;
 /* Nominal axes describe the verified profile's input-pair mapping, not DAC
  * time, physical alignment, finite adaptive support, or a clean-ASR claim. */
 int16_t pcm[160];
};
struct k7_apm21_physical_stream {
#ifdef __cplusplus
 alignas(K7_APM21_CONTEXT_ALIGNMENT) unsigned char consumer_storage[K7_APM21_CONTEXT_BYTES];
#else
 _Alignas(K7_APM21_CONTEXT_ALIGNMENT) unsigned char consumer_storage[K7_APM21_CONTEXT_BYTES];
#endif
 k7_apm21_context *consumer;
 uint64_t owner,dsp_epoch,last_epoch,ordinal,mic_next,reference_next;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 uint64_t attempted_reference_next,accepted_reference_next;
 unsigned ingestion_fault;
#endif
 uint64_t reply_lease,lease_high_water;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 uint64_t pending_reply_lease; /* Original half identity, never current relabel. */
#endif
#line 64
 struct k7_apm21_stream_profile profile;
 unsigned state,have_tail;
 int last_error;
 int16_t api_output[160],real_tail[32];
};

/* Unknown/missing/unverified profile rejects before APM construction. Start is
 * allowed only on fresh/CLOSED storage and epoch>last_epoch. Failed actual
 * construction consumes its epoch. No implicit destroy/restart is performed. */
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
int k7_apm21_stream_prepare(struct k7_apm21_physical_stream *,uint64_t owner,
 uint64_t dsp_epoch,const struct k7_apm21_stream_profile *,int32_t *upstream_rc);
int k7_apm21_stream_bind_origin(struct k7_apm21_physical_stream *,uint64_t owner,
 uint64_t dsp_epoch,uint64_t mic_first,uint64_t reference_first);
#endif
int k7_apm21_stream_start(struct k7_apm21_physical_stream *,uint64_t owner,
#line 74
 uint64_t dsp_epoch,uint64_t mic_first,uint64_t reference_first,
 const struct k7_apm21_stream_profile *,int32_t *upstream_rc);
/* Reply authorization is separate from the DSP stream. next=0 revokes; a new
 * nonzero lease must exceed the prior high-water. Rebind/cancel NEVER call APM,
 * reset ordinal, discard the real32 tail, flush output, or grant qualification.
 * READY processing remains possible with reply_lease=0. */
int k7_apm21_stream_rebind_reply(struct k7_apm21_physical_stream *,uint64_t owner,
 uint64_t dsp_epoch,uint64_t expected_lease,uint64_t next_lease);
/* Return0 is initial warmup; return1 writes all160 real output samples. Every
 * other return, including WARMUP, leaves caller output untouched. The first API
 * call stores only its actual final32 samples. Later output is previous32 plus
 * current first128; current last32 is retained. No zero seed or tail flush.
 * Input/timing pointers are borrowed only for this call; caller input/output
 * storage must not overlap the stream object or be modified during the call.
 * Any same-stream gap/order/report/backend failure requires stop + new epoch.
 * Foreign owner/epoch is rejected without damaging the current stream. */
int k7_apm21_stream_process(struct k7_apm21_physical_stream *,
 const struct k7_apm21_stream_input *,struct k7_apm21_stream_output *);
int k7_apm21_stream_gap(struct k7_apm21_physical_stream *,uint64_t owner,
 uint64_t dsp_epoch,int reason);
/* Caller worker destroys APM once before its own exit. Idempotent after CLOSED
 * for the matching identity. Pending actual32 tail is discarded, never flushed. */
int k7_apm21_stream_stop(struct k7_apm21_physical_stream *,uint64_t owner,uint64_t dsp_epoch);
#ifdef __cplusplus
}
#endif
#endif
#endif
