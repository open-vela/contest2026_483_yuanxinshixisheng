#ifndef CANCEL_CONTINUATION_H
#define CANCEL_CONTINUATION_H
#include "tail_reference_archive.h"
#define CC_CAPACITY 8192u
struct cc_pair {
 uint64_t microphone_first,reference_first;
 uint32_t input_epoch;
 int16_t microphone[320],reference[320];
 /* These are post-mute digital-input assumptions, never acoustic zero/clean. */
 unsigned post_mute_samples;
 bool hypothesis_only;
};
struct cancel_continuation;
int cc_create(uint64_t owner,uint32_t input,uint32_t clock_epoch,
 uint64_t microphone_first,uint32_t floor,struct cancel_continuation **);
void cc_destroy(struct cancel_continuation *);
/* Sole existing worker; archive is the sole FIFO drain for session lifetime.
 * Does not change or reset AEC. Feed each popped pair through the SAME AEC.
 * No separate model/MIC owner. Gaps/overflows require explicit restart. */
int cc_record(struct cancel_continuation *,const struct k7_fifo_ref_record *);
int cc_anchor(struct cancel_continuation *,const struct k7_voice_anchor *);
int cc_microphone(struct cancel_continuation *,uint64_t owner,uint32_t input,
 uint64_t first,const int16_t *,unsigned count);
/* 1 paired frame, 0 waiting. Retains 320 actual reference samples before stop
 * so FIFO discarded suffix never reaches AEC. Digital-silence continuation
 * waits for real clear/end AND successful amp-off. Physical delay/mute ramp
 * remain measured error terms; this API intentionally grants no clean token. */
int cc_pop(struct cancel_continuation *,struct cc_pair *);
#endif
