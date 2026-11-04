#ifndef K7_STREAM_RING_H
#define K7_STREAM_RING_H
#include <stdint.h>
#define K7_STREAM_SAMPLES 320u
#define K7_STREAM_BYTES 640u
/* A live request records 150 frames (3 seconds). Keep the entire bounded
 * utterance if TCP stalls; the sender still drains frames as they arrive. */
#define K7_STREAM_SLOTS 150u
/* Single producer / single consumer; initialize only while both are stopped.
 * Publish a whole 20 ms frame with release/acquire. Overflow is fatal, never
 * overwrite unconsumed microphone audio or silently skip sequence numbers. */
struct k7_stream_ring {
  uint8_t slots[K7_STREAM_SLOTS][K7_STREAM_BYTES];
  uint8_t partial[K7_STREAM_BYTES];
  unsigned partial_samples;
  uint32_t produced, consumed;
  int cancelled;
};
void k7_stream_ring_init(struct k7_stream_ring *r);
void k7_stream_ring_cancel(struct k7_stream_ring *r);
int k7_stream_ring_sample(void *arg, uint32_t left, uint32_t right);
/* Returns 1 for a frame, 0 if empty, negative on cancellation/bad argument. */
int k7_stream_ring_take(struct k7_stream_ring *r, uint8_t out[K7_STREAM_BYTES],
                        uint32_t *seq);
#endif
