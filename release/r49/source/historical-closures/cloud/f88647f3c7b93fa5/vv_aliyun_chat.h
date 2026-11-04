#ifndef VV_ALIYUN_CHAT_H
#define VV_ALIYUN_CHAT_H
#include "vv_https_client.h"
#include "vv_chat_stream.h"
#define VV_ALIYUN_CHAT_EVENT_MAX 16384u
#define VV_ALIYUN_CHAT_WIRE_MAX 262144u
/* Fixed text-only Qwen Flash route. No tools, redirects, retries or device
 * dispatch. text contains the existing bounded dialogue history/current turn.
 * Caller serializes the borrowed TLS client/key against ASR and TTS. */
int vv_aliyun_chat_request(const char *text, char *json, size_t capacity, size_t *bytes);
/* Optional incremental delivery. A nonempty, validated UTF-8 content delta is
 * borrowed only until this synchronous call returns; copy it into a bounded
 * queue. Return 0 for all bytes accepted, a negative error to abort (preserved),
 * or a positive value to abort as VV_CHAT_FAILED. No partial acceptance/retry.
 * The callback must be bounded and must not reenter this parser or the borrowed
 * TLS client, call TTS, or wait for playback. Previously delivered deltas cannot
 * be revoked if later HTTP/SSE validation fails. They are not device commands. */
typedef int (*vv_aliyun_chat_delta_fn)(void *arg,const char *utf8,size_t bytes);
struct vv_aliyun_chat_stream {
 char line[VV_ALIYUN_CHAT_EVENT_MAX+1],event[VV_ALIYUN_CHAT_EVENT_MAX+1];
 char answer[VV_CHAT_ANSWER_MAX+1],id[257];
 size_t line_bytes,event_bytes,answer_bytes,wire_bytes;
 unsigned events;
 bool cr,data_seen,stopped,done,usage_seen;
 int error;
 const struct vv_https_cancel *cancel;
 vv_aliyun_chat_delta_fn on_delta;void *delta_arg;
 bool emitting_delta;
};
/* Feed dechunked SSE; only finish after successful complete HTTP framing.
 * finish requires both finish_reason=stop and [DONE]. All failures erase text;
 * delta, reasoning, function/tool content can never become a device command. */
void vv_aliyun_chat_stream_init(struct vv_aliyun_chat_stream *,const struct vv_https_cancel *);
void vv_aliyun_chat_stream_init_delta(struct vv_aliyun_chat_stream *,const struct vv_https_cancel *,
 vv_aliyun_chat_delta_fn,void *delta_arg);
int vv_aliyun_chat_stream_feed(struct vv_aliyun_chat_stream *,const void *,size_t);
int vv_aliyun_chat_stream_finish(struct vv_aliyun_chat_stream *,char *,size_t);
int vv_aliyun_chat_perform(const struct vv_https_client *,const char *key,
 const char *text,char *answer,size_t capacity,const struct vv_https_cancel *);
/* Same complete-answer/error contract as perform; callbacks can run before
 * HTTP/SSE completion. Only a final zero return authorizes history commit.
 * On failure answer is erased, but the caller must retire already emitted
 * deltas/queued work itself. TLS remains single-owner for the entire call.
 * A NULL callback retains the original behavior. */
int vv_aliyun_chat_perform_stream(const struct vv_https_client *,const char *key,
 const char *text,char *answer,size_t capacity,const struct vv_https_cancel *,
 vv_aliyun_chat_delta_fn,void *delta_arg);
/* Explicit host variants retain the exact model, path, body and SSE contract.
 * Only old endpoint or Beijing ws-* endpoint; copied for the synchronous call. */
int vv_aliyun_chat_perform_at_host(const struct vv_https_client *,const char *host,const char *key,
 const char *text,char *answer,size_t capacity,const struct vv_https_cancel *);
int vv_aliyun_chat_perform_stream_at_host(const struct vv_https_client *,const char *host,const char *key,
 const char *text,char *answer,size_t capacity,const struct vv_https_cancel *,
 vv_aliyun_chat_delta_fn,void *delta_arg);
#endif
