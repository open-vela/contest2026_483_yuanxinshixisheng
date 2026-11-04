/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "voice_frame_ring.h"
#include <errno.h>
#include <string.h>
static int valid(const struct k7_voice_frame *f)
{
  return f && f->samples && f->samples <= K7_VOICE_FRAME_SAMPLES &&
    !(f->flags & ~(K7_VOICE_FRAME_RENDER | K7_VOICE_FRAME_PARTIAL)) &&
    ((f->samples < K7_VOICE_FRAME_SAMPLES) == !!(f->flags & K7_VOICE_FRAME_PARTIAL)) &&
    f->first_sample <= UINT64_MAX - f->samples;
}
void k7_voice_ring_init(struct k7_voice_ring *r)
{ if (r) memset(r, 0, sizeof(*r)); }
int k7_voice_ring_put(struct k7_voice_ring *r, const struct k7_voice_frame *f)
{
  if (!r || !valid(f)) return -EINVAL;
  uint32_t p = __atomic_load_n(&r->produced, __ATOMIC_RELAXED);
  uint32_t c = __atomic_load_n(&r->consumed, __ATOMIC_ACQUIRE);
  if (p - c >= K7_VOICE_RING_SLOTS) {
    uint32_t n = __atomic_load_n(&r->overflows, __ATOMIC_RELAXED);
    if (n != UINT32_MAX) __atomic_store_n(&r->overflows, n + 1, __ATOMIC_RELAXED);
    return -ENOSPC;
  }
  r->frames[p % K7_VOICE_RING_SLOTS] = *f;
  __atomic_store_n(&r->produced, p + 1, __ATOMIC_RELEASE);
  return 0;
}
int k7_voice_ring_take(struct k7_voice_ring *r, struct k7_voice_frame *f)
{
  if (!r || !f) return -EINVAL;
  uint32_t c = __atomic_load_n(&r->consumed, __ATOMIC_RELAXED);
  uint32_t p = __atomic_load_n(&r->produced, __ATOMIC_ACQUIRE);
  if (p == c) return 0;
  if (p - c > K7_VOICE_RING_SLOTS) return -EIO;
  *f = r->frames[c % K7_VOICE_RING_SLOTS];
  __atomic_store_n(&r->consumed, c + 1, __ATOMIC_RELEASE);
  return 1;
}
unsigned k7_voice_ring_overflows(const struct k7_voice_ring *r)
{ return r ? __atomic_load_n(&r->overflows, __ATOMIC_RELAXED) : 0; }
int k7_voice_packer_init(struct k7_voice_packer *p, struct k7_voice_ring *r,
                         uint32_t epoch, unsigned flags, uint64_t first)
{
  if (!p || !r || flags & ~K7_VOICE_FRAME_RENDER) return -EINVAL;
  memset(p, 0, sizeof(*p)); p->ring = r; p->pending.epoch = epoch;
  p->pending.flags = (uint16_t)flags; p->next_sample = first; return 0;
}
int k7_voice_packer_flush(struct k7_voice_packer *p)
{
  if (!p || !p->ring) return -EINVAL;
  if (p->error) return p->error;
  if (!p->pending.samples) return 0;
  if (p->pending.samples < K7_VOICE_FRAME_SAMPLES)
    p->pending.flags |= K7_VOICE_FRAME_PARTIAL;
  int rc = k7_voice_ring_put(p->ring, &p->pending);
  if (rc) return p->error = rc;
  p->pending.samples = 0; p->pending.flags &= ~K7_VOICE_FRAME_PARTIAL;
  return 0;
}
int k7_voice_packer_sample(struct k7_voice_packer *p, int16_t sample, uint64_t index)
{
  if (!p || !p->ring) return -EINVAL;
  if (p->error) return p->error;
  if (index != p->next_sample || index == UINT64_MAX) return p->error = -EILSEQ;
  if (!p->pending.samples) p->pending.first_sample = index;
  p->pending.pcm[p->pending.samples++] = sample; p->next_sample++;
  return p->pending.samples == K7_VOICE_FRAME_SAMPLES ? k7_voice_packer_flush(p) : 0;
}
void k7_voice_preroll_init(struct k7_voice_preroll *p)
{ if (p) memset(p, 0, sizeof(*p)); }
int k7_voice_preroll_add(struct k7_voice_preroll *p, const struct k7_voice_frame *f)
{
  if (!p || !valid(f) || f->flags || f->samples != K7_VOICE_FRAME_SAMPLES)
    return -EINVAL;
  if (p->count && (p->epoch != f->epoch || p->next_sample != f->first_sample))
    k7_voice_preroll_init(p);
  p->frames[p->next] = *f; p->next = (p->next + 1) % K7_VOICE_PREROLL_FRAMES;
  if (p->count < K7_VOICE_PREROLL_FRAMES) p->count++;
  p->epoch = f->epoch; p->next_sample = f->first_sample + f->samples;
  return 0;
}
int k7_voice_preroll_copy(const struct k7_voice_preroll *p,
                          struct k7_voice_frame *out, unsigned capacity)
{
  if (!p || (!out && p->count)) return -EINVAL;
  if (capacity < p->count) return -ENOSPC;
  unsigned begin = (p->next + K7_VOICE_PREROLL_FRAMES - p->count) % K7_VOICE_PREROLL_FRAMES;
  for (unsigned i = 0; i < p->count; ++i) out[i] = p->frames[(begin + i) % K7_VOICE_PREROLL_FRAMES];
  return (int)p->count;
}
