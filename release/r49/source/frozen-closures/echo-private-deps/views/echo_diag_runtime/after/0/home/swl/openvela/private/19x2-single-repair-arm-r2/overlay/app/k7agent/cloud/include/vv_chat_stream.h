#ifndef VV_CHAT_STREAM_H
#define VV_CHAT_STREAM_H
#include <stdbool.h>
#include <stddef.h>

#define VV_CHAT_QUESTION_SCALARS 2000u
#define VV_CHAT_QUESTION_UTF16_MAX 2000u /* Java @Size counts UTF-16 code units. */
#define VV_CHAT_QUESTION_BYTES 8000u
#define VV_CHAT_REQUEST_MAX 16384u
#define VV_CHAT_ANSWER_MAX 16384u
#define VV_CHAT_EVENT_MAX 65536u
#define VV_CHAT_WIRE_MAX 2097152u

enum vv_chat_error {
  VV_CHAT_INVALID=-6000, VV_CHAT_LIMIT, VV_CHAT_PROTOCOL, VV_CHAT_CANCELLED,
  VV_CHAT_DEPENDENCY_TIMEOUT, VV_CHAT_DEPENDENCY_UNAVAILABLE, VV_CHAT_FAILED,
  VV_CHAT_MEMORY, VV_CHAT_MOCK
};
/* Existing gimbal-ai contract: one stateless question, no history or tool calls.
 * Heap-allocate this bounded parser. Only completed.answerText is authoritative;
 * delta text is validated/counted but never exposed as an answer or played.
 * Feed dechunked HTTP bytes. Call finish only after complete HTTP200/event-stream
 * framing; it is the only operation that copies an answer to caller storage. */
struct vv_chat_stream {
  char line[VV_CHAT_EVENT_MAX+1],json[VV_CHAT_EVENT_MAX+1];
  char answer[VV_CHAT_ANSWER_MAX+1],event[32];
  size_t line_used,json_used,answer_bytes,wire_bytes,delta_scalars;
  unsigned events;
  bool cr,event_seen,data_seen,terminal;
  int error;
  bool (*cancelled)(void *);void *cancel_arg;
};
int vv_chat_question_json(const char *,char *,size_t,size_t *);
/* Shared bounded UTF-8 validation/counting for endpoint and local context.
 * Does not normalize, trim, split a scalar or accept embedded NUL/control text.
 * Empty/whitespace text is valid with content=false; caller decides admission. */
int vv_chat_text_measure(const char *,size_t,size_t *,size_t *,bool *);
int vv_chat_text_utf16(const char *,size_t,size_t *,size_t *,bool *);
int vv_chat_stream_init(struct vv_chat_stream *,bool (*)(void *),void *);
int vv_chat_stream_feed(struct vv_chat_stream *,const void *,size_t);
int vv_chat_stream_finish(struct vv_chat_stream *,char *,size_t);
#endif
