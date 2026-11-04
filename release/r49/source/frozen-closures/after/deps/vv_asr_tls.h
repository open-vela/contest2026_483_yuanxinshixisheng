#ifndef VV_ASR_TLS_H
#define VV_ASR_TLS_H
#include "vv_asr_wss.h"
#include "vv_https_mbedtls.h"
/* Zero initialize. All calls on one owning thread, no reentrant event calls.
 * TLS is borrowed: must outlive this object and may not be used elsewhere until
 * close. Caller creates it through verified trust gates; no TLS bypass exists.
 * Open performs synchronous TLS/Upgrade BEFORE recording. Subsequent I/O never
 * waits; caller polls fd/events and ticks timers. No recording is started here. */
struct vv_asr_tls {
 struct vv_asr_wss asr;struct vv_https_mbedtls *tls;
 short wanted_events;bool transport_live;
};
int vv_asr_tls_open(struct vv_asr_tls *,struct vv_https_mbedtls *,const char *host,uint16_t port,const char *path,const char *authorization,vv_https_now_ms_fn,void *clock_arg,uint64_t handshake_deadline,const struct vv_https_cancel *,void *rx_storage,size_t rx_capacity,void (*event)(void *,const struct vv_asr_event *),void *event_arg);
int vv_asr_tls_tick(struct vv_asr_tls *,uint64_t now,bool cancel,unsigned budget);
int vv_asr_tls_audio(struct vv_asr_tls *,const void *,size_t,uint64_t);
int vv_asr_tls_stop(struct vv_asr_tls *,uint64_t);
int vv_asr_tls_cancel(struct vv_asr_tls *,uint64_t);
int vv_asr_tls_fd(const struct vv_asr_tls *);
short vv_asr_tls_wanted(const struct vv_asr_tls *);
void vv_asr_tls_close(struct vv_asr_tls *);
#endif
