#ifndef VV_ASR_STREAM_H
#define VV_ASR_STREAM_H
#include <stddef.h>
#include <stdint.h>
#define VV_ASR_PCM_BYTES 640u
#define VV_ASR_PACKET_BYTES 644u
#define VV_ASR_MAX_FRAMES 3000u
#define VV_ASR_WINDOW 8u
#define VV_ASR_MAX_JSON 32768u
#define VV_ASR_MAX_TEXT 4096u
enum vv_asr_state { VV_ASR_NEW, VV_ASR_WAIT_READY, VV_ASR_STREAMING,
  VV_ASR_WAIT_COMPLETED, VV_ASR_CANCELLING, VV_ASR_DONE,
  VV_ASR_CANCELLED, VV_ASR_FAILED };
enum vv_asr_event_kind { VV_ASR_NO_EVENT, VV_ASR_READY, VV_ASR_ACK,
  VV_ASR_PARTIAL, VV_ASR_FINAL, VV_ASR_COMPLETED, VV_ASR_CANCEL_ACK,
  VV_ASR_REMOTE_ERROR };
struct vv_asr_stream { enum vv_asr_state state; uint32_t sent, acknowledged, revision; };
struct vv_asr_event { enum vv_asr_event_kind kind; uint32_t sentence_id, revision;
  char text[VV_ASR_MAX_TEXT+1]; char error_code[64]; };
/* Caller serializes ALL calls, owns output buffers and transport deadlines.
 * Successful TX preparation reserves the state/sequence BEFORE sending.
 * Send each output exactly once; any failed/uncertain send MUST call abort.
 * Never retry a prepared audio packet or silently discard microphone samples.
 * Suggested contract deadlines: ready15s, ACK/send10s, result20s, session80s.
 * No socket, audio, hardware, callbacks or transcript logging in this module. */
void vv_asr_stream_init(struct vv_asr_stream *s);
void vv_asr_stream_abort(struct vv_asr_stream *s);
int vv_asr_stream_start(struct vv_asr_stream *s,char *out,size_t capacity,size_t *bytes);
int vv_asr_stream_audio(struct vv_asr_stream *s,const void *pcm,size_t bytes,
                        unsigned char *out,size_t capacity);
int vv_asr_stream_stop(struct vv_asr_stream *s,char *out,size_t capacity,size_t *bytes);
int vv_asr_stream_cancel(struct vv_asr_stream *s,char *out,size_t capacity,size_t *bytes);
/* Complete reassembled WS TEXT message only (not fragment/binary).
 * Protocol errors latch FAILED. EAGAIN from audio/stop means backpressure.
 * FINAL is a sentence event, never session completion or a motion command. */
int vv_asr_stream_receive(struct vv_asr_stream *s,const char *json,size_t bytes,
                          struct vv_asr_event *event);
#endif
