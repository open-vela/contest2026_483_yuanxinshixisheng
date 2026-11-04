#ifndef VV_ASR_WSS_H
#define VV_ASR_WSS_H
#include "vv_asr_session.h"
#include "vv_wss_incremental.h"
/* Initialize zero storage. Single owner; TLS and Upgrade already verified.
 * Application event callback may only copy events, never reenter this object.
 * Tick at least every 20ms, using monotonic clock; audio EAGAIN is unconsumed.
 * Audio capture and poll/ready scheduling remain external. */
struct vv_asr_wss {
 struct vv_asr_session session;struct vv_wsi ws;struct vv_wsi_ops transport;
 void (*event)(void *,const struct vv_asr_event *);void *event_arg;
 uint64_t now,tx_deadline;bool entered;
};
int vv_asr_wss_start(struct vv_asr_wss *,const struct vv_wsi_ops *,void *,size_t,uint64_t,void (*)(void *,const struct vv_asr_event *),void *);
int vv_asr_wss_tick(struct vv_asr_wss *,uint64_t,bool,unsigned);
int vv_asr_wss_audio(struct vv_asr_wss *,const void *,size_t,uint64_t);
int vv_asr_wss_stop(struct vv_asr_wss *,uint64_t);
int vv_asr_wss_cancel(struct vv_asr_wss *,uint64_t);
void vv_asr_wss_abort(struct vv_asr_wss *);
#endif
