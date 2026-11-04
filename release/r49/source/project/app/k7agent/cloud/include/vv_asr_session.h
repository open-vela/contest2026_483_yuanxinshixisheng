#ifndef VV_ASR_SESSION_H
#define VV_ASR_SESSION_H
#include "vv_asr_stream.h"
/* Single owner event pump. send callback returns 0 for definite full send OR a complete copy into a
 * uniquely owned bounded transport queue. Queue acceptance is NOT peer delivery;
 * transport errors after acceptance MUST abort this session. Coordinator must
 * check queue capacity BEFORE calling audio/stop/cancel (preparation reserves seq).
 * Callback
 * honors absolute deadline_ms, and must not retain borrowed data. Any other
 * result aborts once: no retransmission. clock is monotonic ms supplied per call.
 * Complete WS messages are injected externally; this is NOT a socket poller.
 * EAGAIN from audio means input unconsumed; caller must retain it in bounded queue. */
struct vv_asr_session_io {
 int (*send)(void *,unsigned,const void *,size_t,uint64_t deadline_ms);
 void (*close)(void *); void *arg;
};
struct vv_asr_session {
 struct vv_asr_stream stream; struct vv_asr_session_io io;
 uint64_t started,last_now,phase_deadline,next_audio,ack_deadline[VV_ASR_WINDOW];
 int active,closed;
};
int vv_asr_session_start(struct vv_asr_session *,const struct vv_asr_session_io *,uint64_t now);
int vv_asr_session_tick(struct vv_asr_session *,uint64_t now);
int vv_asr_session_audio(struct vv_asr_session *,const void *,size_t,uint64_t now);
int vv_asr_session_stop(struct vv_asr_session *,uint64_t now);
int vv_asr_session_cancel(struct vv_asr_session *,uint64_t now);
int vv_asr_session_message(struct vv_asr_session *,unsigned opcode,const void *,size_t,uint64_t now,struct vv_asr_event *);
void vv_asr_session_abort(struct vv_asr_session *);
#endif
