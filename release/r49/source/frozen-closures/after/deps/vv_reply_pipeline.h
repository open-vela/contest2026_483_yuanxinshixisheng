#ifndef VV_REPLY_PIPELINE_H
#define VV_REPLY_PIPELINE_H
#include "vv_aliyun_chat.h"
#include "vv_aliyun_client.h"
#include "vv_stream_speaker.h"

#define VV_REPLY_PIPELINE_TEXT_BYTES 8192u
#define VV_REPLY_PIPELINE_PCM_BYTES 2880000u
#define VV_REPLY_PIPELINE_TIMEOUT_MS 90000u
#define VV_REPLY_PIPELINE_SEGMENT_BYTES 256u

/* Single consumer; nonblocking. READY=1 copies one complete UTF-8 fragment
 * (plus NUL), AGAIN=0 has no fragment yet, DONE=2 means all text was consumed
 * AND the LLM completed successfully. Negative errors are sticky. The caller
 * must keep receiving audio on AGAIN and may send finish only after DONE.
 * Concatenating READY bytes reproduces the original answer exactly, including
 * whitespace. Entirely blank answers are rejected without text publication.
 * Capacity must hold up to SEGMENT_BYTES+1; failure never drops a fragment. */
typedef int (*vv_reply_next_text_fn)(void *,char *,size_t,size_t *);
enum vv_reply_text_result { VV_REPLY_TEXT_AGAIN=0, VV_REPLY_TEXT_READY=1,
 VV_REPLY_TEXT_DONE=2 };
struct vv_reply_pipeline_ops {
 int (*chat)(void *,const char *,char *,size_t,const struct vv_https_cancel *,
             vv_aliyun_chat_delta_fn,void *);
 int (*tts)(void *,const char *,vv_stream_pcm_sink,void *,
            const struct vv_https_cancel *,struct vv_aliyun_stream_result *);
 uint64_t (*now_ms)(void *);
 void *arg;
 /* Preferred duplex route: exactly one call for the whole answer. Owns an
  * independent TLS connection from chat; no fallback to tts after failure.
  * Decoded PCM is forwarded immediately, never retried/replayed. Must poll
  * cancel while awaiting text/audio and honor its stop/deadline promptly. */
 int (*tts_stream)(void *,vv_reply_next_text_fn,void *,vv_stream_pcm_sink,void *,
                   const struct vv_https_cancel *,struct vv_aliyun_stream_result *);
};
struct vv_reply_pipeline_request {
 const struct vv_reply_pipeline_ops *ops;
 const char *question;
 char *answer;
 size_t answer_capacity;
};
/* Called once by the existing speaker receive worker. A separate joinable
 * 64 KiB chat thread only appends validated deltas to the bounded text store.
 * The consumer uses tts_stream when supplied, otherwise explicitly uses tts.
 * Neither route owns audio hardware. Both ops and the clock must support
 * concurrent independent chat/TTS calls; the external cancel must be the
 * speaker's thread-safe atomic token, not a business callback.
 * Only complete chat + complete exact TTS returns success/terminal=true.
 * Errors clear answer and retain the first cause; audible PCM is irrevocable.
 * The caller may commit dialogue history only after final success. */
int vv_reply_pipeline_fetch(void *,vv_stream_pcm_sink,void *,
 const struct vv_https_cancel *,struct vv_stream_fetch_result *);
#endif
