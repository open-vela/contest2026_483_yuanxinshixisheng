#ifndef VV_NATIVE_AUDIO_H
#define VV_NATIVE_AUDIO_H
#include "vv_asr_tls.h"
/* Borrow open ASR session; caller already verified TLS/Upgrade, no capture yet.
 * All runtime network callbacks on caller thread; producer owns codec lock.
 * stop_join returns 0 only once producer no longer references its storage. */
struct vv_native_capture {
 int (*start)(void *); int (*take)(void *,uint8_t pcm[640],uint32_t *seq);
 int (*done)(void *,int *capture_result); int (*stop_join)(void *); void *arg;
};
struct vv_native_audio_clock {
 uint64_t (*now)(void *);int (*wait)(void *,int fd,short events,unsigned ms);
 bool (*cancelled)(void *);void *arg;
};
int vv_native_audio_run(struct vv_asr_tls *,const struct vv_native_capture *,const struct vv_native_audio_clock *,unsigned frames);
/* Explicit callable K7 adapter, never automatically invoked. samples320..48000
 * in multiples320; network session must already be open. Captures with cue. */
uint64_t vv_native_audio_posix_now(void *unused);
int vv_native_audio_posix_wait(void *unused,int fd,short events,unsigned ms);
int vv_native_audio_k7_run(struct vv_asr_tls *,unsigned samples,const struct vv_native_audio_clock *);
#endif
