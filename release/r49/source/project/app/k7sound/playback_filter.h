#ifndef K7_PLAYBACK_FILTER_H
#define K7_PLAYBACK_FILTER_H
#include <stdint.h>
#include <stddef.h>
enum af_mode { AF_MA4=1, AF_MA4_HP80=2 };
#define AF_MAX_FRAMES 48000u
#define AF_HP_Q30 INT64_C(1040533595)
/* Nominal16kHz stereo raw signed32 encoded in uint32. Zero state per call.
 * All3 memory ranges must be aligned, valid and mutually disjoint.
 * Validation failure leaves destination and saturation count untouched.
 * Caller exclusively owns buffers for full call; no concurrent mutation. */
int af_filter(const uint32_t *src,size_t src_words,uint32_t *dst,
 size_t dst_words,unsigned frames,enum af_mode mode,uint32_t *saturations);
#endif
