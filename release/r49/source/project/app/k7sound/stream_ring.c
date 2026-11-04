#include "stream_ring.h"
#include <errno.h>
#include <string.h>

void k7_stream_ring_init(struct k7_stream_ring *r)
{
  if (r) memset(r, 0, sizeof(*r));
}

void k7_stream_ring_cancel(struct k7_stream_ring *r)
{
  if (r) __atomic_store_n(&r->cancelled, 1, __ATOMIC_RELEASE);
}

int k7_stream_ring_sample(void *arg, uint32_t left, uint32_t right)
{
  struct k7_stream_ring *r = arg;
  uint32_t produced, consumed;
  unsigned at;
  (void)right;
  if (!r) return -EINVAL;
  if (__atomic_load_n(&r->cancelled, __ATOMIC_ACQUIRE)) return -ECANCELED;
  at = r->partial_samples * 2;
  /* Existing S32 capture is left-aligned. Preserve zero and sign bits exactly;
   * little-endian S16 encoding does not depend on host signed-shift behavior. */
  r->partial[at] = (uint8_t)(left >> 16);
  r->partial[at + 1] = (uint8_t)(left >> 24);
  if (++r->partial_samples != K7_STREAM_SAMPLES) return 0;
  produced = __atomic_load_n(&r->produced, __ATOMIC_RELAXED);
  consumed = __atomic_load_n(&r->consumed, __ATOMIC_ACQUIRE);
  if (produced - consumed >= K7_STREAM_SLOTS || produced == UINT32_MAX)
    {
      k7_stream_ring_cancel(r);
      return -ENOSPC;
    }
  memcpy(r->slots[produced % K7_STREAM_SLOTS], r->partial, K7_STREAM_BYTES);
  r->partial_samples = 0;
  __atomic_store_n(&r->produced, produced + 1, __ATOMIC_RELEASE);
  return 0;
}

int k7_stream_ring_take(struct k7_stream_ring *r, uint8_t out[K7_STREAM_BYTES],
                        uint32_t *seq)
{
  uint32_t consumed, produced;
  if (!r || !out || !seq) return -EINVAL;
  if (__atomic_load_n(&r->cancelled, __ATOMIC_ACQUIRE)) return -ECANCELED;
  consumed = __atomic_load_n(&r->consumed, __ATOMIC_RELAXED);
  produced = __atomic_load_n(&r->produced, __ATOMIC_ACQUIRE);
  if (consumed == produced) return 0;
  memcpy(out, r->slots[consumed % K7_STREAM_SLOTS], K7_STREAM_BYTES);
  *seq = consumed;
  __atomic_store_n(&r->consumed, consumed + 1, __ATOMIC_RELEASE);
  return 1;
}
