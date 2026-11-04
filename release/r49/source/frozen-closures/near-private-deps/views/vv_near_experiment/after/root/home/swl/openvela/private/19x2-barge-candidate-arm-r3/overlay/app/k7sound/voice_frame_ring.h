/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_VOICE_FRAME_RING_H
#define K7_VOICE_FRAME_RING_H
#include <stdint.h>
#define K7_VOICE_FRAME_SAMPLES 320u
#define K7_VOICE_RING_SLOTS 64u
#define K7_VOICE_PREROLL_FRAMES 16u
enum { K7_VOICE_FRAME_RENDER = 1u, K7_VOICE_FRAME_PARTIAL = 2u };
struct k7_voice_frame {
  uint64_t first_sample;
  uint32_t epoch;
  uint16_t samples, flags;
  int16_t pcm[K7_VOICE_FRAME_SAMPLES];
};
/* Single producer, single consumer. Initialize only while both are stopped.
 * No overwrite or artificial samples on overflow. Caller must invalidate the
 * affected utterance/AEC history. Render samples describe FIFO enqueue order,
 * not DAC/acoustic timestamps; a cancelled TX can discard an enqueued tail. */
struct k7_voice_ring {
  struct k7_voice_frame frames[K7_VOICE_RING_SLOTS];
  uint32_t produced, consumed, overflows;
};
void k7_voice_ring_init(struct k7_voice_ring *);
int k7_voice_ring_put(struct k7_voice_ring *, const struct k7_voice_frame *);
int k7_voice_ring_take(struct k7_voice_ring *, struct k7_voice_frame *);
unsigned k7_voice_ring_overflows(const struct k7_voice_ring *);
/* Producer-owned packer: no allocation, blocking, model or network calls.
 * Flush publishes the real short tail with PARTIAL, never zero-pads it. */
struct k7_voice_packer {
  struct k7_voice_ring *ring;
  struct k7_voice_frame pending;
  uint64_t next_sample;
  int error;
};
int k7_voice_packer_init(struct k7_voice_packer *, struct k7_voice_ring *,
                         uint32_t epoch, unsigned flags, uint64_t first_sample);
int k7_voice_packer_sample(struct k7_voice_packer *, int16_t sample,
                           uint64_t sample_index);
int k7_voice_packer_flush(struct k7_voice_packer *);
/* Consumer-thread-only pre-roll, separate from the SPSC transport. A new epoch
 * or sample gap resets history. Partial tails are not accepted by KWS. */
struct k7_voice_preroll {
  struct k7_voice_frame frames[K7_VOICE_PREROLL_FRAMES];
  unsigned count, next;
  uint64_t next_sample;
  uint32_t epoch;
};
void k7_voice_preroll_init(struct k7_voice_preroll *);
int k7_voice_preroll_add(struct k7_voice_preroll *, const struct k7_voice_frame *);
int k7_voice_preroll_copy(const struct k7_voice_preroll *,
                          struct k7_voice_frame *, unsigned capacity);
#endif
