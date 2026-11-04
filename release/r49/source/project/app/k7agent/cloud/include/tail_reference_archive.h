#ifndef TAIL_REFERENCE_ARCHIVE_H
#define TAIL_REFERENCE_ARCHIVE_H
#include "voice_session.h"
#include <stdatomic.h>
#define TRA_SAMPLES 5120u
enum { TRA_IDLE,TRA_ARMED,TRA_RECORDING,TRA_DONE,TRA_FAILED };
struct tail_reference_archive {
 atomic_uint state;
 uint32_t floor,generation,epoch;uint64_t owner,instance,first,next,source_next;
 uint64_t last_before_us,last_after_us,kept_first,digital_complete_pair_next;
 unsigned kept,clear_seen,uncertain_partial_pair;int reason;
 struct k7_voice_anchor clear;
 int16_t pcm[TRA_SAMPLES];uint8_t zeros[TRA_SAMPLES];
};
/* Worker-only producer; CLI may read only after acquire DONE/FAILED and while
 * command reset is excluded. No DAC-rendered/clean/zero-tail claim exists. */
void tra_arm(struct tail_reference_archive *,uint64_t owner,uint32_t floor);
void tra_fault(struct tail_reference_archive *,int);
void tra_anchor(struct tail_reference_archive *,const struct k7_voice_anchor *);
void tra_record(struct tail_reference_archive *,const struct k7_fifo_ref_record *);
#endif
