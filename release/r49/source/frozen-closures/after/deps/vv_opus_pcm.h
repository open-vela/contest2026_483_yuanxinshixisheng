#ifndef VV_OPUS_PCM_H
#define VV_OPUS_PCM_H
#include <stdint.h>
#include "vv_stream_speaker.h"
/* One ordered mono Ogg Opus task -> 16kHz S16LE PCM. Decoder state belongs
 * to this task and is never shared with the audio-owner thread. The native
 * 16k route supports trim boundaries exactly divisible by three; unsupported
 * boundaries fail explicitly rather than rounding away audio samples. */
struct vv_opus_pcm;
struct vv_opus_pcm_stats {
 size_t wire_bytes,pcm_bytes;
 unsigned packets,pre_skip_48k,eos;
 uint64_t granule_mismatches;
};
int vv_opus_pcm_create(vv_stream_pcm_sink,void *,const struct vv_https_cancel *,
                       struct vv_opus_pcm **);
/* Explicit service profile: Qwen's streaming Ogg granules have been observed
 * inconsistent with valid Opus packet duration. Derive time from packets,
 * never add gap samples or infer tail trimming from those granules. */
int vv_opus_pcm_create_qwen_live(vv_stream_pcm_sink,void *,const struct vv_https_cancel *,
                                 struct vv_opus_pcm **);
int vv_opus_pcm_feed(struct vv_opus_pcm *,const void *,size_t);
/* live_task=true requires the caller to have validated the matching successful
 * cloud task-finished event. Only then may complete pages without Ogg EOS end
 * the stream. It cannot recover a partial page or infer encoder tail padding. */
int vv_opus_pcm_finish(struct vv_opus_pcm *,bool live_task);
void vv_opus_pcm_get_stats(const struct vv_opus_pcm *,struct vv_opus_pcm_stats *);
void vv_opus_pcm_destroy(struct vv_opus_pcm *);
#endif
