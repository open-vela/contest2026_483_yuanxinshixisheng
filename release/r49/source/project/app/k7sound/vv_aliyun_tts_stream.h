#ifndef VV_ALIYUN_TTS_STREAM_H
#define VV_ALIYUN_TTS_STREAM_H
#include <stddef.h>
#include <stdbool.h>
/* Qwen-Audio-TTS HTTP SSE, requested format=pcm/sample_rate=16000.
 * Feed dechunked HTTP body only. Caller verifies HTTP200/event-stream and calls
 * finish only after complete successful HTTP framing. No URL is downloaded.
 * Heap-allocate this bounded state; do not place it on a small task stack.
 * Sink receives borrowed, even-length mono S16LE chunks; copy before returning.
 * Single owner; destroy/wipe state only after feed/sink have returned. */
#define VV_ALIYUN_TTS_EVENT_MAX 65536u
int vv_aliyun_tts_stream_json(const char *model,const char *voice,const char *text,
 char *out,size_t capacity,size_t *written);
typedef int (*vv_aliyun_tts_pcm_fn)(void *,const unsigned char *,size_t);
struct vv_aliyun_tts_stream {
 char line[VV_ALIYUN_TTS_EVENT_MAX+1],json[VV_ALIYUN_TTS_EVENT_MAX+1];
 unsigned char pcm[VV_ALIYUN_TTS_EVENT_MAX*3/4+2];
 size_t line_used,json_used,total,max_bytes;
 unsigned events,audio_events;
 bool stop,pending,cr,error_event;
 unsigned char odd;
 int error;
 vv_aliyun_tts_pcm_fn sink;void *sink_arg;
 bool (*cancelled)(void *);void *cancel_arg;
};
int vv_aliyun_tts_stream_init(struct vv_aliyun_tts_stream *,size_t,
 vv_aliyun_tts_pcm_fn,void *,bool (*)(void *),void *);
int vv_aliyun_tts_stream_feed(struct vv_aliyun_tts_stream *,const unsigned char *,size_t);
int vv_aliyun_tts_stream_finish(struct vv_aliyun_tts_stream *);
#endif
