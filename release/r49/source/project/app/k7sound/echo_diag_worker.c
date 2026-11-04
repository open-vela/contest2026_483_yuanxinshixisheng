#include "echo_diag_worker.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
struct k7_echo_diag {
  uint64_t instance, fifo_anchor, mic_anchor, next_fifo, next_source, next_mic;
  uint64_t ref_p, mic_p, read, last_us, mic_owner;
  uint32_t epoch, generation, mic_epoch;
  bool active, mic_seen, time_seen;
  int16_t ref[K7_ECHO_DIAG_CAPACITY], mic[K7_ECHO_DIAG_CAPACITY];
  uint8_t zeros[K7_ECHO_DIAG_CAPACITY / 8];
};
static int bad(struct k7_echo_diag *s)
{ if (s) s->active = false; return -EPIPE; }
static bool matches(const struct k7_echo_diag *s,
                    const struct k7_fifo_ref_status *v)
{
  return s && s->active && v && v->eligible && !v->exhausted && !v->faults &&
    s->instance == v->instance && s->epoch == v->epoch &&
    s->generation == v->generation;
}
static bool mic_matches(const struct k7_echo_diag *s, struct k7_echo_diag_mic_id m)
{ return s && s->active && m.owner == s->mic_owner && m.epoch == s->mic_epoch; }
static bool plus(uint64_t a, uint64_t b) { return a <= UINT64_MAX - b; }
size_t k7_echo_diag_storage_bytes(void) { return sizeof(struct k7_echo_diag); }
int k7_echo_diag_create(struct k7_echo_diag **out)
{
  if (!out) return -EINVAL;
  *out = calloc(1, sizeof(**out));
  return *out ? 0 : -ENOMEM;
}
void k7_echo_diag_destroy(struct k7_echo_diag *s)
{ if (s) { memset(s, 0, sizeof(*s)); free(s); } }
void k7_echo_diag_invalidate(struct k7_echo_diag *s) { (void)bad(s); }
bool k7_echo_diag_active(const struct k7_echo_diag *s) { return s && s->active; }
int k7_echo_diag_begin(struct k7_echo_diag *s,
  const struct k7_fifo_ref_record *r, const struct k7_fifo_ref_status *v,
  struct k7_echo_diag_mic_id m, uint64_t fa, uint64_t ma)
{
  if (!s || !r || !v) return -EINVAL;
  /* Re-begin is explicit but cannot revive a failed or replaced round. */
  if (s->active || !v->eligible || v->exhausted || v->faults ||
      r->kind != K7_FIFO_REF_BEGIN || !r->instance || !r->epoch ||
      !r->generation || r->samples || r->source_samples || r->faults ||
      r->first_source || r->instance != v->instance || r->epoch != v->epoch ||
      r->generation != v->generation || !m.owner || !m.epoch ||
      fa < r->first_enqueue || !plus(fa, K7_ECHO_DIAG_FRAME) ||
      !plus(ma, K7_ECHO_DIAG_FRAME) ||
      r->instance < s->instance ||
      (r->instance == s->instance && r->epoch <= s->epoch) ||
      m.owner < s->mic_owner ||
      (m.owner == s->mic_owner && m.epoch < s->mic_epoch))
    return bad(s);
  memset(s, 0, sizeof(*s));
  s->instance = r->instance; s->epoch = r->epoch; s->generation = r->generation;
  s->fifo_anchor = fa; s->mic_anchor = ma; s->next_fifo = r->first_enqueue;
  s->mic_owner = m.owner; s->mic_epoch = m.epoch; s->active = true;
  return 0;
}
int k7_echo_diag_reference(struct k7_echo_diag *s,
  const struct k7_fifo_ref_record *r, const struct k7_fifo_ref_status *v)
{
  uint64_t selected;
  size_t skip, n, sources = 0;
  if (!s || !r) return -EINVAL;
  if (!matches(s, v) || r->instance != s->instance || r->epoch != s->epoch ||
      r->generation != s->generation || r->kind != K7_FIFO_REF_PCM || r->faults ||
      !r->samples || r->samples > K7_ECHO_DIAG_FRAME ||
      r->first_enqueue != s->next_fifo || r->first_source != s->next_source ||
      !plus(s->next_fifo, r->samples) ||
      r->before_us == UINT64_MAX || r->after_us == UINT64_MAX ||
      r->after_us < r->before_us ||
      (s->time_seen && r->before_us < s->last_us))
    return bad(s);
  for (size_t i = 0; i < K7_ECHO_DIAG_FRAME; ++i)
    {
      bool zero = (r->zero_bits[i / 8] >> (i % 8)) & 1u;
      if (i >= r->samples) { if (zero) return bad(s); continue; }
      if (zero && r->pcm[i]) return bad(s);
      if (!zero) ++sources;
    }
  if (sources != r->source_samples || !plus(s->next_source, sources))
    return bad(s);
  skip = s->next_fifo < s->fifo_anchor ?
    (size_t)((s->fifo_anchor - s->next_fifo) < r->samples ?
             s->fifo_anchor - s->next_fifo : r->samples) : 0;
  n = r->samples - skip;
  selected = s->next_fifo + skip;
  if (n && (selected < s->fifo_anchor || selected - s->fifo_anchor != s->ref_p ||
            !plus(s->ref_p, n) || s->ref_p < s->read ||
            s->ref_p - s->read + n > K7_ECHO_DIAG_CAPACITY))
    return bad(s);
  for (size_t i = 0; i < n; ++i)
    {
      size_t slot = (size_t)((s->ref_p + i) % K7_ECHO_DIAG_CAPACITY);
      uint8_t bit = (uint8_t)(1u << (slot % 8));
      bool zero = (r->zero_bits[(i+skip)/8] >> ((i+skip)%8)) & 1u;
      s->ref[slot] = r->pcm[i+skip];
      s->zeros[slot/8] = (uint8_t)((s->zeros[slot/8] & ~bit) | (zero ? bit : 0));
    }
  s->ref_p += n; s->next_fifo += r->samples; s->next_source += sources;
  s->last_us = r->after_us; s->time_seen = true;
  return 0;
}
int k7_echo_diag_microphone(struct k7_echo_diag *s, struct k7_echo_diag_mic_id m,
  uint64_t first, const int16_t *pcm, size_t count)
{
  size_t skip, n;
  uint64_t selected;
  if (!s || !pcm) return -EINVAL;
  if (!mic_matches(s, m) || !count || count > K7_ECHO_DIAG_FRAME ||
      !plus(first, count) || (s->mic_seen && first != s->next_mic))
    return bad(s);
  skip = first < s->mic_anchor ?
    (size_t)((s->mic_anchor - first) < count ? s->mic_anchor - first : count) : 0;
  n = count - skip;
  selected = first + skip;
  if (n && (selected < s->mic_anchor || selected - s->mic_anchor != s->mic_p ||
            !plus(s->mic_p, n) || s->mic_p < s->read ||
            s->mic_p - s->read + n > K7_ECHO_DIAG_CAPACITY))
    return bad(s);
  for (size_t i = 0; i < n; ++i)
    s->mic[(s->mic_p + i) % K7_ECHO_DIAG_CAPACITY] = pcm[i+skip];
  s->mic_p += n; s->next_mic = first + count; s->mic_seen = true;
  return 0;
}
int k7_echo_diag_pop(struct k7_echo_diag *s, const struct k7_fifo_ref_status *v,
  struct k7_echo_diag_mic_id m, struct k7_echo_diag_pair *out)
{
  if (!s || !out) return -EINVAL;
  if (!matches(s, v) || !mic_matches(s, m)) return bad(s);
  if (s->ref_p - s->read < K7_ECHO_DIAG_FRAME ||
      s->mic_p - s->read < K7_ECHO_DIAG_FRAME) return 0;
  if (!plus(s->read, K7_ECHO_DIAG_FRAME) ||
      !plus(s->fifo_anchor, s->read) || !plus(s->mic_anchor, s->read))
    return bad(s);
  memset(out, 0, sizeof(*out));
  out->reference_instance = s->instance; out->reference_epoch = s->epoch;
  out->generation = s->generation; out->mic_owner = s->mic_owner;
  out->mic_epoch = s->mic_epoch; out->fifo_first = s->fifo_anchor + s->read;
  out->mic_first = s->mic_anchor + s->read; out->hypothesis_only = true;
  for (size_t i = 0; i < K7_ECHO_DIAG_FRAME; ++i)
    {
      size_t slot = (size_t)((s->read + i) % K7_ECHO_DIAG_CAPACITY);
      out->reference[i] = s->ref[slot]; out->microphone[i] = s->mic[slot];
      if ((s->zeros[slot/8] >> (slot%8)) & 1u)
        out->explicit_zero_bits[i/8] |= (uint8_t)(1u << (i%8));
    }
  s->read += K7_ECHO_DIAG_FRAME;
  return 1;
}
