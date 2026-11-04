#ifndef VV_REAL_REPORT_SPEECH_H
#define VV_REAL_REPORT_SPEECH_H
#include <stddef.h>
struct vv_native_client;
struct vv_speech_provider;
struct vv_https_cancel;
/* Nonzero means PCM was offered to the speaker, including a failing sink.
 * Never retry or play fallback on failure with nonzero output. The caller
 * owns validated report identity, epoch, HOLD and STOP admission. */
int vv_native_client_speak_report(struct vv_native_client *,const char *,const struct vv_https_cancel *,size_t *submitted_or_attempted_pcm_bytes);
int vv_aliyun_provider_speak_report(const struct vv_speech_provider *,const char *,const struct vv_https_cancel *,size_t *submitted_or_attempted_pcm_bytes);
#endif
