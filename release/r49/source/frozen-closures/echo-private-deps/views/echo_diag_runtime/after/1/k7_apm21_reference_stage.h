#ifndef K7_APM21_REFERENCE_STAGE_H
#define K7_APM21_REFERENCE_STAGE_H
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
#include <stdint.h>
#include <stdatomic.h>
#define K7_APM21_REFERENCE_CAPACITY 320u
#define K7_APM21_REFERENCE_LATE_US 40000u
struct k7_apm21_reference_block {uint64_t first,before_us,after_us;int16_t pcm[160];uint8_t zero_bits[20];};
struct k7_apm21_reference_stage {
 /* Original PIO is the only writer/reader of staging data. */
 uint64_t first,written,confirmed,last_us,epoch;
 uint32_t generation,input,clock;
 unsigned head,count,active;
 int16_t pcm[320];uint8_t zero[40];uint64_t time[2],after[2];
 /* Independent atomics are observations, not a composite authority token. */
 atomic_int fault;atomic_uint sealed;
 atomic_uint_fast64_t published_next,final_cut;
};
void k7_apm21_reference_init(struct k7_apm21_reference_stage *);
int k7_apm21_reference_begin(struct k7_apm21_reference_stage *,uint64_t epoch,uint32_t generation,uint32_t input,uint32_t clock,uint64_t first,uint64_t now);
int k7_apm21_reference_pair(struct k7_apm21_reference_stage *,int16_t,unsigned zero,uint64_t before,uint64_t after);
int k7_apm21_reference_observe(struct k7_apm21_reference_stage *,uint64_t epoch,uint32_t generation,uint32_t input,uint32_t clock,unsigned remaining_words,int observation_rc,uint64_t now);
int k7_apm21_reference_take(struct k7_apm21_reference_stage *,struct k7_apm21_reference_block *,uint64_t now);
/* Stop cut uses exact TX_IDLE FIFO count, before CLEAR. Odd remaining words
 * reject tail continuity; even a full pair cut is FIFO transport, not DAC. */
int k7_apm21_reference_cut(struct k7_apm21_reference_stage *,uint64_t epoch,uint32_t generation,uint32_t input,uint32_t clock,unsigned remaining_words,int observation_rc,uint64_t now);
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
/* PIO-only sealed snapshot of genuine consumed samples, never discarded FIFO. */
int k7_apm21_reference_prefix(const struct k7_apm21_reference_stage *,struct k7_apm21_reference_block *,unsigned *samples);
#endif
int k7_apm21_reference_fault(struct k7_apm21_reference_stage *,int);
#endif
#endif
