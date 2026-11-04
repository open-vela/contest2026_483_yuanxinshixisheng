#ifndef K7_AEC_SPEEX10_H
#define K7_AEC_SPEEX10_H
#include "k7_aec_frame_adapter.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define K7_AEC_SPEEX10_ARENA_BYTES (256u*1024u)
struct k7_aec_speex10;
struct k7_aec_speex10_config {
 unsigned tail_samples; /* 0=1600; original320..5120 step320 grid. */
 size_t arena_bytes; /* 0=256KiB; fixed1024..256KiB allocation at create. */
};
struct k7_aec_speex10_stats {
 uint64_t resets,frames,outputs,warmup_frames,rejected,warnings,state_epoch;
 uint64_t mic_next,reference_next;
 size_t arena_bytes,arena_used,allocations;
 unsigned tail_samples,ready,same_live_echo_attached;
 int last_error;
};
/* Create allocates only context+fixed arena. Backend reset initializes/rebuilds
 * MDF+RES inside that arena, under the nonblocking private Speex guard. */
int k7_aec_speex10_create(const struct k7_aec_speex10_config *,
                         struct k7_aec_speex10 **);
/* Exclusive owner must retire processing first. No implicit tail flush. */
int k7_aec_speex10_destroy(struct k7_aec_speex10 **);
int k7_aec_speex10_get_stats(const struct k7_aec_speex10 *,
                            struct k7_aec_speex10_stats *);
/* Explicit Speex1.2.1 MDF160 -> same live RES160. AGC/VAD off, denoise on;
 * upstream suppression defaults-15/-40/-15. OLA160, initial one-half warmup.
 * Descriptor reset is an explicit new state lineage. Invalidate only revokes
 * processing; it never calls Speex reset. A software cancel seal that retains
 * the same continuous stream must NOT invoke adapter cancel/reset/invalidate.
 * Existing320 ABI and its private Speex symbols are entirely separate. */
const struct k7_aec_fa_backend *k7_aec_speex10_descriptor(void);
#ifdef __cplusplus
}
#endif
#endif
