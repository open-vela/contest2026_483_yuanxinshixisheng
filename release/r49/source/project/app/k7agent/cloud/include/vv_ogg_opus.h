#ifndef VV_OGG_OPUS_H
#define VV_OGG_OPUS_H

#include <stddef.h>
#include <stdint.h>

#define VV_OGG_OPUS_PAGE_MAX 65307u
#define VV_OGG_OPUS_PACKET_MAX 65536u

enum vv_ogg_opus_profile {
  VV_OGG_OPUS_STRICT = 0,
  VV_OGG_OPUS_QWEN_LIVE = 1
};

struct vv_ogg_opus_head {
  uint16_t pre_skip_48k;
  uint32_t input_rate; /* Metadata, not the decoder output rate. */
  int16_t gain_q8;
  uint8_t version;
  uint8_t channels;
  uint8_t mapping;
};

struct vv_ogg_opus_packet {
  uint32_t page_sequence;
  uint64_t page_granule_48k;
  unsigned page_eos;
  unsigned index; /* Zero-based completed audio packet index on this page. */
  unsigned count;
  enum vv_ogg_opus_profile profile;
  unsigned granule_untrusted; /* Always 1 in the explicit Qwen live profile. */
  unsigned granule_mismatch; /* Page GP differs from TOC cumulative clock. */
  uint32_t duration_48k;
  uint64_t decoded_start_48k; /* Untrimmed offset from the first audio packet. */
  uint32_t skip_48k; /* Decode all samples, discard this prefix. */
  uint32_t keep_48k; /* Publish only this many following samples. */
};

struct vv_ogg_opus_callbacks {
  int (*on_head)(void *, const struct vv_ogg_opus_head *);
  int (*on_packet)(void *, const uint8_t *, size_t,
                   const struct vv_ogg_opus_packet *);
};

/* A bounded, single-owner, from-BOS mono/mapping-0 demuxer. No allocation,
 * resynchronization, PLC, chaining or multiplexing. Keep this large object
 * out of small task stacks. init is the explicit reset between generations.
 * Callbacks must return 0 or a negative error; pointers last only until return.
 * No callback may reenter/reset this object. Previously delivered packets
 * cannot be recalled if a later page or finish fails.
 *
 * CRC, framing, sequence, all packet TOC shapes and the complete page granule
 * are checked BEFORE any audio callback for that page. This includes EOS
 * trimming across more than one packet. The actual Opus decoder must still
 * validate compressed frame contents and match duration_48k. All duration,
 * start, skip, keep and granule values use 48000-Hz samples, even for a 16k
 * decoder. Convert cumulative boundaries, never round each packet separately.
 * Valid zero-padding in Tags is accepted within the 65536-byte packet cap.
 * Streams ending before their global pre-skip is satisfied are rejected.
 */
struct vv_ogg_opus {
  struct vv_ogg_opus_callbacks callbacks;
  void *arg;
  struct vv_ogg_opus_head head;
  uint8_t page[VV_OGG_OPUS_PAGE_MAX];
  uint8_t packet[VV_OGG_OPUS_PACKET_MAX];
  size_t page_have, page_need, packet_have;
  uint32_t serial, next_sequence;
  uint64_t decoded_48k, origin_48k, last_granule_48k, audio_pages;
  uint64_t granule_mismatches;
  enum vv_ogg_opus_profile profile;
  unsigned stage, eos, sealed, busy;
  int error;
};

int vv_ogg_opus_init(struct vv_ogg_opus *,
                     const struct vv_ogg_opus_callbacks *, void *);
/* Explicit known-service profile, never a fallback after strict failure.
 * Qwen's observed stream has inconsistent GP and omits EOS. This profile
 * trusts only TOC durations for timestamps and global pre-skip; audio GP is
 * reported as untrusted, never used for silence insertion, PLC or end trim,
 * even if EOS is present. CRC, sequence, serial, headers, complete packets
 * and TOC validation remain enforced. finish_live_task is mandatory. */
int vv_ogg_opus_init_qwen_live(struct vv_ogg_opus *,
                               const struct vv_ogg_opus_callbacks *, void *);
int vv_ogg_opus_feed(struct vv_ogg_opus *, const void *, size_t);
/* Success requires both mandatory headers, audio, an EOS page, and no tail. */
int vv_ogg_opus_finish(struct vv_ogg_opus *);
/* Explicit live-service contract ONLY: the caller has already validated the
 * same task's successful task-finished event. Allows an audio page boundary
 * without Ogg EOS; it cannot infer encoder tail trimming absent EOS. This is
 * not a truncated-file recovery path, and never clears a prior strict-finish
 * error. Partial pages/packets or unsatisfied pre-skip still fail. */
int vv_ogg_opus_finish_live_task(struct vv_ogg_opus *);

#endif
