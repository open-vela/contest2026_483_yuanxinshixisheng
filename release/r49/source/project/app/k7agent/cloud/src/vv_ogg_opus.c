#include "vv_ogg_opus.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/* Container rules: RFC 3533 section 6 and RFC 7845 sections 3--5.
 * No third-party implementation source is embedded here. */
struct packet_ref {
  const uint8_t *data;
  size_t bytes;
  uint32_t duration;
};

static uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
  return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int fail(struct vv_ogg_opus *p, int rc)
{
  if (!p->error) p->error = rc < 0 ? rc : -EINVAL;
  return p->error;
}

static uint32_t page_crc(const uint8_t *p, size_t n)
{
  uint32_t crc = 0;
  size_t i;
  unsigned bit;
  for (i = 0; i < n; ++i) {
    uint8_t b = i >= 22 && i < 26 ? 0 : p[i];
    crc ^= (uint32_t)b << 24;
    for (bit = 0; bit < 8; ++bit)
      crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
  }
  return crc;
}

static int utf8(const uint8_t *p, size_t n)
{
  size_t i = 0;
  while (i < n) {
    uint32_t c = p[i++], min;
    unsigned more;
    if (c < 0x80) continue;
    if (c >= 0xc2 && c <= 0xdf) { more = 1; min = 0x80; c &= 31; }
    else if (c >= 0xe0 && c <= 0xef) { more = 2; min = 0x800; c &= 15; }
    else if (c >= 0xf0 && c <= 0xf4) { more = 3; min = 0x10000; c &= 7; }
    else return -EBADMSG;
    if (more > n - i) return -EBADMSG;
    while (more--) {
      if ((p[i] & 0xc0) != 0x80) return -EBADMSG;
      c = (c << 6) | (p[i++] & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return -EBADMSG;
  }
  return 0;
}

static int tags(const uint8_t *p, size_t n)
{
  uint32_t length, count, i;
  size_t at = 12;
  if (n < 16 || memcmp(p, "OpusTags", 8)) return -EBADMSG;
  length = le32(p + 8);
  if (length > n - at || utf8(p + at, length)) return -EBADMSG;
  at += length;
  if (n - at < 4) return -EBADMSG;
  count = le32(p + at); at += 4;
  if (count > (n - at) / 4) return -EBADMSG;
  for (i = 0; i < count; ++i) {
    if (n - at < 4) return -EBADMSG;
    length = le32(p + at); at += 4;
    if (length > n - at || utf8(p + at, length)) return -EBADMSG;
    at += length;
  }
  /* Remaining bounded binary padding is explicitly permitted by RFC 7845. */
  return 0;
}

static int frame_size(const uint8_t *p, size_t end, size_t *at, size_t *out)
{
  unsigned first;
  if (*at >= end) return -EBADMSG;
  first = p[(*at)++];
  if (first < 252) *out = first;
  else {
    if (*at >= end) return -EBADMSG;
    *out = first + 4u * p[(*at)++];
  }
  return *out <= 1275 ? 0 : -EBADMSG;
}

/* Only framing/TOC validation. Entropy-coded frame validity belongs to Opus. */
static int duration(const uint8_t *p, size_t n, uint32_t *out)
{
  unsigned frames, code, mode, i;
  uint32_t per;
  size_t at = 1, end = n, one, sum = 0, padding = 0;
  if (!n) return -EBADMSG;
  mode = (p[0] >> 3) & 3;
  if (p[0] & 0x80) per = 120u << mode;
  else if ((p[0] & 0x60) == 0x60) per = (p[0] & 8) ? 960 : 480;
  else per = mode == 3 ? 2880 : (480u << mode);
  code = p[0] & 3;
  frames = code == 0 ? 1 : 2;
  if (code == 0) {
    if (n - 1 > 1275) return -EBADMSG;
  } else if (code == 1) {
    if ((n - 1) % 2 || (n - 1) / 2 > 1275) return -EBADMSG;
  } else if (code == 2) {
    if (frame_size(p, n, &at, &one) || one > n - at ||
        n - at - one > 1275) return -EBADMSG;
  } else {
    unsigned control;
    if (n < 2) return -EBADMSG;
    control = p[1]; at = 2; frames = control & 63;
    if (!frames || frames > 48 || per * frames > 5760) return -EBADMSG;
    if (control & 0x40) {
      unsigned b;
      do {
        if (at >= n) return -EBADMSG;
        b = p[at++];
        padding += b == 255 ? 254u : b;
        if (padding > n - at) return -EBADMSG;
      } while (b == 255);
      end -= padding;
    }
    if (control & 0x80) {
      for (i = 1; i < frames; ++i) {
        if (frame_size(p, end, &at, &one)) return -EBADMSG;
        sum += one;
        if (sum > end - at) return -EBADMSG;
      }
      if (end - at - sum > 1275) return -EBADMSG;
    } else if ((end - at) % frames || (end - at) / frames > 1275)
      return -EBADMSG;
  }
  *out = per * frames;
  return *out <= 5760 ? 0 : -EBADMSG;
}

static int header(struct vv_ogg_opus *p, const uint8_t *b, size_t n)
{
  unsigned gain;
  if (n < 19 || memcmp(b, "OpusHead", 8) || b[8] == 0 || b[8] >= 16 ||
      b[9] != 1 || b[18] != 0 || (b[8] == 1 && n != 19))
    return -EBADMSG;
  p->head.version = b[8]; p->head.channels = b[9]; p->head.mapping = b[18];
  p->head.pre_skip_48k = (uint16_t)((unsigned)b[10] | ((unsigned)b[11] << 8));
  p->head.input_rate = le32(b + 12);
  gain = (unsigned)b[16] | ((unsigned)b[17] << 8);
  p->head.gain_q8 = (int16_t)(gain < 32768 ? (int)gain : (int)gain - 65536);
  return 0;
}

static int process_page(struct vv_ogg_opus *p)
{
  const uint8_t *b = p->page;
  struct packet_ref refs[255];
  unsigned flags = b[5], segments = b[26], count = 0, i;
  uint32_t serial = le32(b + 14), sequence = le32(b + 18);
  uint64_t granule = le64(b + 6), sum = 0, valid_end, origin = p->origin_48k;
  size_t at = 27 + segments, start = at, bytes = 0, carry = p->packet_have;
  unsigned mismatch = 0;
  int rc;
  if (memcmp(b, "OggS", 4) || b[4] || (flags & ~7u) ||
      page_crc(b, p->page_have) != le32(b + 22)) return -EBADMSG;
  if (p->stage == 0) {
    if (flags != 2 || sequence != 0 || granule != 0) return -EBADMSG;
  } else {
    if ((flags & 2) || serial != p->serial || sequence != p->next_sequence ||
        !!(flags & 1) != !!carry) return -EBADMSG;
  }
  /* Build views; only the first packet can need a cross-page copy. Its
   * buffer is not reused until all callbacks for this page have returned. */
  for (i = 0; i < segments; ++i) {
    size_t amount = b[27 + i];
    if (bytes + amount > VV_OGG_OPUS_PACKET_MAX - carry) return -E2BIG;
    bytes += amount; at += amount;
    if (amount == 255) continue;
    refs[count].bytes = carry + bytes;
    refs[count].duration = 0;
    if (carry) {
      memcpy(p->packet + carry, b + start, bytes);
      refs[count].data = p->packet;
    } else refs[count].data = b + start;
    ++count; carry = 0; bytes = 0; start = at;
  }
  if (p->stage == 0) {
    if (count != 1 || bytes || !segments ||
        (rc = header(p, refs[0].data, refs[0].bytes)) != 0)
      return -EBADMSG;
    p->serial = serial;
    rc = p->callbacks.on_head(p->arg, &p->head);
    if (rc || p->error) return p->error ? p->error : rc;
    p->stage = 1;
  } else if (p->stage == 1) {
    if ((flags & 4) || count > 1 || (count && (bytes || granule != 0)) ||
        (!count && granule != UINT64_MAX)) return -EBADMSG;
    if (count) {
      if ((rc = tags(refs[0].data, refs[0].bytes)) != 0) return rc;
      p->stage = 2;
    }
  } else {
    if (!count) {
      if (granule != UINT64_MAX || (flags & 4)) return -EBADMSG;
    } else {
      if ((p->profile == VV_OGG_OPUS_STRICT && granule > INT64_MAX) ||
          ((flags & 4) && bytes)) return -EBADMSG;
      for (i = 0; i < count; ++i) {
        if ((rc = duration(refs[i].data, refs[i].bytes, &refs[i].duration)) != 0)
          return rc;
        sum += refs[i].duration;
      }
      if (p->decoded_48k > (uint64_t)INT64_MAX - sum) return -EOVERFLOW;
      valid_end = p->decoded_48k + sum;
      if (p->profile == VV_OGG_OPUS_QWEN_LIVE) {
        /* Service-specific and explicit: never expand or trim PCM using GP. */
        origin = 0;
        mismatch = granule != valid_end;
      } else {
        if (!p->audio_pages) {
          if (!(flags & 4) && granule < sum) return -EBADMSG;
          origin = granule >= sum ? granule - sum : 0;
        }
        if (origin > (uint64_t)INT64_MAX - valid_end) return -EOVERFLOW;
        if (flags & 4) {
          if (granule < origin || granule - origin < p->decoded_48k ||
              granule - origin > valid_end ||
              granule - origin < p->head.pre_skip_48k) return -EBADMSG;
          valid_end = granule - origin;
        } else if (granule != origin + valid_end) return -EBADMSG;
      }
      /* Every frame/granule check above precedes the first page callback. */
      for (i = 0; i < count; ++i) {
        struct vv_ogg_opus_packet info;
        uint64_t start48 = p->decoded_48k, stop48 = start48 + refs[i].duration;
        uint64_t from = start48 < p->head.pre_skip_48k ? p->head.pre_skip_48k : start48;
        uint64_t to = stop48 < valid_end ? stop48 : valid_end;
        memset(&info, 0, sizeof(info));
        info.page_sequence = sequence; info.page_granule_48k = granule;
        info.page_eos = !!(flags & 4); info.index = i; info.count = count;
        info.profile = p->profile;
        info.granule_untrusted = p->profile == VV_OGG_OPUS_QWEN_LIVE;
        info.granule_mismatch = mismatch;
        info.duration_48k = refs[i].duration; info.decoded_start_48k = start48;
        info.skip_48k = (uint32_t)((from < stop48 ? from : stop48) - start48);
        info.keep_48k = from < to ? (uint32_t)(to - from) : 0;
        rc = p->callbacks.on_packet(p->arg, refs[i].data, refs[i].bytes, &info);
        if (rc || p->error) return p->error ? p->error : rc;
        p->decoded_48k = stop48;
      }
      p->origin_48k = origin; p->last_granule_48k = granule;
      p->granule_mismatches += mismatch;
      p->audio_pages++;
      if (flags & 4) p->eos = 1;
    }
  }
  if (bytes) {
    /* If no packet completed, carry still points to the old partial prefix. */
    memcpy(p->packet + carry, b + start, bytes);
    p->packet_have = carry + bytes;
  } else p->packet_have = carry;
  p->next_sequence = sequence + 1u;
  return 0;
}

int vv_ogg_opus_init_qwen_live(struct vv_ogg_opus *p,
                               const struct vv_ogg_opus_callbacks *callbacks, void *arg)
{
  int rc = vv_ogg_opus_init(p, callbacks, arg);
  if (!rc) p->profile = VV_OGG_OPUS_QWEN_LIVE;
  return rc;
}

int vv_ogg_opus_init(struct vv_ogg_opus *p,
                     const struct vv_ogg_opus_callbacks *callbacks, void *arg)
{
  struct vv_ogg_opus_callbacks copy;
  if (!p) return -EINVAL;
  memset(&copy, 0, sizeof(copy));
  if (callbacks) copy = *callbacks;
  memset(p, 0, sizeof(*p));
  p->page_need = 27;
  if (!copy.on_head || !copy.on_packet)
    return fail(p, -EINVAL);
  p->callbacks = copy; p->arg = arg;
  return 0;
}

int vv_ogg_opus_feed(struct vv_ogg_opus *p, const void *data, size_t n)
{
  const uint8_t *in = data;
  int rc = 0;
  if (!p) return -EINVAL;
  if (p->error) return p->error;
  if (p->busy) return fail(p, -EDEADLK);
  if (!p->callbacks.on_head || !p->callbacks.on_packet || (!in && n))
    return fail(p, -EINVAL);
  p->busy = 1;
  while (n) {
    size_t take;
    unsigned i;
    if (p->eos || p->sealed) { rc = -EBADMSG; break; }
    take = p->page_need - p->page_have;
    if (take > n) take = n;
    memcpy(p->page + p->page_have, in, take);
    p->page_have += take; in += take; n -= take;
    if (p->page_have != p->page_need) continue;
    if (p->page_need == 27) {
      if (memcmp(p->page, "OggS", 4) || p->page[4] || (p->page[5] & ~7u)) {
        rc = -EBADMSG; break;
      }
      p->page_need += p->page[26];
      if (p->page_have != p->page_need) continue;
    }
    if (p->page_need == (size_t)27 + p->page[26]) {
      for (i = 0; i < p->page[26]; ++i) p->page_need += p->page[27 + i];
      if (p->page_have != p->page_need) continue;
    }
    rc = process_page(p);
    if (rc) break;
    p->page_have = 0; p->page_need = 27;
  }
  p->busy = 0;
  return rc ? fail(p, rc) : p->error;
}

static int finish(struct vv_ogg_opus *p, int live_task)
{
  if (!p) return -EINVAL;
  if (p->error) return p->error;
  if (p->busy) return fail(p, -EDEADLK);
  if (!live_task && p->profile != VV_OGG_OPUS_STRICT) return fail(p, -EINVAL);
  if (p->stage != 2 || !p->audio_pages || (!live_task && !p->eos) ||
      p->page_have || p->packet_have ||
      (p->profile == VV_OGG_OPUS_QWEN_LIVE ? p->decoded_48k :
       p->last_granule_48k - p->origin_48k) < p->head.pre_skip_48k)
    return fail(p, -EBADMSG);
  p->sealed = 1;
  return 0;
}

int vv_ogg_opus_finish(struct vv_ogg_opus *p)
{
  return finish(p, 0);
}

int vv_ogg_opus_finish_live_task(struct vv_ogg_opus *p)
{
  return finish(p, 1);
}
