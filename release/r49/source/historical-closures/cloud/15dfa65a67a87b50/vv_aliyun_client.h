#ifndef VV_ALIYUN_CLIENT_H
#define VV_ALIYUN_CLIENT_H
#include "vv_speech_provider.h"
#include <stdatomic.h>
struct vv_aliyun_config {
 struct vv_https_client *https; /* verified TLS, single owner, provider CA */
 const char *host,*asr_path,*tts_path,*api_key,*asr_model,*tts_model,*voice;
 const char *const *download_hosts;size_t download_host_count; /* zero permits ASR only; TTS refused before POST */
 unsigned total_timeout_ms; /* 1..120000 including TTS POST+download */
 bool allow_proven_bj_compat; /* default false: exact BJ host HTTPS upgrade + observed WAV sentinel */
};
struct vv_aliyun_client {struct vv_aliyun_config config;atomic_bool busy;};
/* Borrow immutable config/secret storage until caller retires client. Init makes no
 * request. Never share HTTPS concurrently with another client. Redirects are
 * BUSY/invalid admission preserves caller output; admitted failures clear it.
 * Redirects are rejected; download GET has no provider Authorization or business Bearer. */
int vv_aliyun_client_init(struct vv_aliyun_client *,const struct vv_aliyun_config *);
struct vv_speech_provider vv_aliyun_client_provider(struct vv_aliyun_client *);
int vv_aliyun_client_stream(struct vv_aliyun_client *,const char *,
 int (*)(void *,const unsigned char *,size_t),void *,const struct vv_https_cancel *);
/* Per-request audio integrity, independent of HTTP framing success. Always
 * cleared on entry. A true terminal flag requires successful SSE finish and
 * cancellation checks, never merely an observed finish_reason field. The
 * function still returns the original HTTP error; this is not success/history
 * admission. Valid only until the caller discards this request's result. */
struct vv_aliyun_stream_result {
 int http_result;
 bool terminal_audio_complete;
 /* Number of even PCM bytes successfully accepted by this request's sink. */
 size_t pcm_bytes;
 /* Framed transport EOF only, valid HTTP200/SSE and all received events fed
  * without error. Only already accepted PCM is valid; an unfinished SSE tail
  * or odd trailing byte is excluded. Still an error/partial, never success. */
 bool validated_prefix;
};
int vv_aliyun_client_stream_ex(struct vv_aliyun_client *,const char *,
 int (*)(void *,const unsigned char *,size_t),void *,const struct vv_https_cancel *,
 struct vv_aliyun_stream_result *);
#endif
