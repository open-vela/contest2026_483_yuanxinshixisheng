#ifndef VV_NATIVE_BATCH_AUDIO_H
#define VV_NATIVE_BATCH_AUDIO_H
#include "vv_asr_batch.h"
/* Explicit blocking single round; never auto-started. Caller serializes this
 * adapter with other audio/ASR/TTS sessions and owns borrowed client/request,
 * readiness and cancellation contexts for the entire call. Existing codec lock
 * remains authoritative for hardware; module guard rejects its own reentry.
 * ready must freshly verify network AND TLS trust prerequisites, not only WiFi.
 * No transcript is interpreted as a command. PCM is wiped before release. */
struct vv_native_batch_audio_request {
 struct vv_asr_batch_request endpoint; /* pcm/pcm_bytes are ignored */
 unsigned samples; /* 320..48000, multiple320;16kHz mono16,<=3seconds */
 bool (*ready)(void *);void *ready_arg;
};
int vv_native_batch_audio_perform(struct vv_https_client *,
 const struct vv_native_batch_audio_request *,const struct vv_https_cancel *,
 struct vv_asr_batch_result *);
#endif
