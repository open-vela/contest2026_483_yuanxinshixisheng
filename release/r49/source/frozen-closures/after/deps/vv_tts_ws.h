#ifndef VV_TTS_WS_H
#define VV_TTS_WS_H
#include "vv_reply_pipeline.h"
#include "vv_wss_incremental.h"
/* Verified TLS and RFC6455 Upgrade must already have succeeded. This call
 * owns that connection until close on every exit. Exactly one owner performs
 * all TLS reads/writes. Text is pulled without blocking, PCM is delivered in
 * order during generation, never retried after publication. */
struct vv_tts_ws_config {
 struct vv_wsi_ops io;
 vv_https_now_ms_fn now_ms;void *clock_arg;
 void (*sleep_ms)(void *,unsigned);void *sleep_arg;
 const char *model,*voice;
 bool opus; /* Explicit Ogg Opus32kbps; false keeps the original PCM route. */
 /* Default false. On explicit reply cancellation only, best-effort current
  * task cancel: <=200ms/256 nonblocking pumps and within the original budget.
  * Always closes; cancellation never becomes complete audio or connection reuse. */
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_WIRE_CANCEL
 bool wire_cancel;
#endif
};
int vv_tts_ws_run(const struct vv_tts_ws_config *,vv_reply_next_text_fn,void *,
 vv_stream_pcm_sink,void *,const struct vv_https_cancel *,struct vv_aliyun_stream_result *);
#endif
