#ifndef VV_REPORT_AUDIO_H
#define VV_REPORT_AUDIO_H
#include "vv_https_client.h"
#include <stdatomic.h>
struct vv_report_audio_ops {
  int (*tts)(void *,const char *,unsigned char *,size_t,size_t *,const struct vv_https_cancel *);
  /* Zero ONLY when both requested identities still match the current task. */
  int (*fresh)(void *,const char *,const char *,const struct vv_https_cancel *);
  /* Synchronous completion; MUST stop and return on cancellation, no retained
   * PCM pointer or deferred playback after returning. */
  int (*play)(void *,const unsigned char *,size_t,const struct vv_https_cancel *);
  /* Optional synchronous streamed synthesis/playback; cancellation must stop
   * playback and join workers before return. Freshness is checked first. */
  int (*speak)(void *,const char *,const struct vv_https_cancel *);
};
struct vv_report_audio {atomic_bool busy;const struct vv_report_audio_ops *ops;void *arg;};
struct vv_report_audio_result {size_t segments_synthesized,segments_played;int callback_error;};
enum vv_report_audio_error {VV_REPORT_AUDIO_OK=0,VV_REPORT_AUDIO_INVALID=-4000,
  VV_REPORT_AUDIO_BUSY,VV_REPORT_AUDIO_CANCELLED,VV_REPORT_AUDIO_TEXT,
  VV_REPORT_AUDIO_TTS,VV_REPORT_AUDIO_PCM,VV_REPORT_AUDIO_STALE,VV_REPORT_AUDIO_PLAY};
void vv_report_audio_init(struct vv_report_audio *);
/* Explicit synchronous runner over already validated brief text. No networking
 * or hardware implementation here. Caller supplies one private PCM scratch
 * buffer (<=960000B), kept exclusive until return. Text is limited to16384B.
 * No auto retries or motion. Callback network/play timeouts and a total
 * deadline cancellation predicate must be supplied by the caller.
 * Single owner; INVALID/BUSY do not touch output. Admitted runs always clear PCM
 * before returning, including success. Caller keeps source/IDs immutable.
 * Playback callback must enforce cancellation itself; this runner cannot stop
 * an asynchronous device or preempt a non-cooperative callback. */
int vv_report_audio_run(struct vv_report_audio *,const char *text,size_t text_bytes,
  const char *task_id,const char *report_id,unsigned char *pcm,size_t pcm_capacity,
  const struct vv_https_cancel *,struct vv_report_audio_result *);
#endif
