#ifndef VV_NATIVE_RUNTIME_H
#define VV_NATIVE_RUNTIME_H
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include "vv_native_client.h"
#include "vv_native_mode.h"
#include "vv_native_assessment.h"
#include "vv_report_audio.h"
#include "device_photo_prompts.h"
#include <stdatomic.h>
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
#include "vv_dialogue.h"
#endif
struct vv_native_runtime_ops {
 bool (*ready)(void *);
 int (*capture)(void *,unsigned,unsigned char *,size_t,size_t *,const struct vv_https_cancel *);
 int (*photo_status)(void *,uint32_t *,unsigned *);
 int (*photo_copy)(void *,uint32_t,unsigned,unsigned char *,size_t,size_t *);
 int (*hold_revision)(void *,unsigned *);
 int (*resume)(void *,unsigned);
 void *arg;
 /* Optional synchronous command confirmation PCM playback; the cancel token
  * includes external cancellation, mode epoch and current platform readiness. */
 int (*play)(void *,const unsigned char *,size_t,const struct vv_https_cancel *);
};
struct vv_native_runtime_config {
 struct vv_native_client *client;struct vv_native_mode *mode;
 struct vv_native_assessment *assessment;struct vv_report_audio *report;
 struct vv_native_runtime_ops ops;
 unsigned char *pcm;size_t pcm_capacity; /* aligned serial TTS/report scratch */
 bool (*assessment_ready)(void *); /* optional separate business gate, ops.arg */
#ifdef CONFIG_EXAMPLES_K7CLOUD_DIALOGUE
 struct vv_dialogue *dialogue; /* borrowed; independent answer cancellation */
 /* Optional explicit route, always paired. admission runs before microphone
  * access; reply receives this call's completed result, never reconstructed
  * capture identity. The platform owns both callbacks and their lifetime. */
 int (*scoped_listen_admit)(void *);
 int (*scoped_reply)(void *,const struct vv_asr_batch_result *,const struct vv_https_cancel *);
 void *scoped_arg;
#endif
 /* Optional serial-owner gate after full ASR intent confirmation, before
  * mode dispatch. NULL preserves prior behavior. It may reject admission but
  * must not issue actions or swallow cancellation; borrowed for this lifetime. */
 int (*command_admit)(void *,enum k7_voice_intent,unsigned,const struct vv_https_cancel *);
 void *command_admit_arg;
};
struct vv_native_runtime {
 atomic_bool busy,cancelled;struct vv_native_runtime_config config;
 bool pending;uint32_t previous_photo;unsigned pending_epoch;
 bool listen_retryable; /* Serial-owner hint: failed ASR only, never replay actions. */
 struct k7_photo_prompt_state photo_prompt;
 struct k7_photo_prompt_state photo_saved; /* individually completed saved prompts */
 bool listen_window_failed; /* ASR failed before any command was dispatched. */
 bool listen_owner_failed; /* Resident capture provenance, not a generic errno. */
};
/* Explicit init, no login/capture/motion. Borrow everything for owner lifetime.
 * All operations except cancel are one serial owner, no legacy bridge parallel.
 * Configure assessment backend and report_audio_k7 explicitly before init.
 * ready must be bounded and nonblocking and reflect network/trust/auth readiness. */
int vv_native_runtime_init(struct vv_native_runtime *,const struct vv_native_runtime_config *);
/* Cross-thread token only. In-flight operations unwind to hold. If idle caller
 * must call stop to physically request hold. Never frees or deletes photos. */
void vv_native_runtime_cancel(struct vv_native_runtime *);
/* Thread-safe reply-only cancellation. No mode invalidation or motion command.
 * A real microphone/KWS owner must supply the trigger; software call alone is
 * not evidence of spoken barge-in. Available as a no-op in legacy builds. */
void vv_native_runtime_interrupt_reply(struct vv_native_runtime *);
int vv_native_runtime_stop(struct vv_native_runtime *);
/* Explicit new listening admission only after hold is applied; no motion. */
int vv_native_runtime_rearm(struct vv_native_runtime *);
/* Optional command TTS/play follows confirmed TRACK or STOP using shared PCM.
 * STOP requests hold before TTS and waits for applied hold. Errors return to
 * hold without resuming. Capture/TTS/play are serial: spoken STOP is recognized
 * next listening round; external cancel/epoch invalidation interrupts IO. */
int vv_native_runtime_listen(struct vv_native_runtime *,unsigned samples,enum k7_voice_intent *);
/* Serial owner only. Speaks solely from a fresh photo epoch/mask. EAGAIN means
 * capture reset/progress changed during synthesis; no stale playback or upload.
 * Other errors hold and cancel this assessment. */
int vv_native_runtime_photo_progress(struct vv_native_runtime *);
/* Caller supplies stable capture/consent/boundary/idempotency metadata. Images
 * are replaced with exact owned RAM copies. EAGAIN means photos/hold not ready.
 * Once admitted, no automatic replay; errors leave board RAM photos intact.
 * out records actual assessment/upload outcome; audio failure is return code.
 * Synchronous report phase cannot listen for spoken STOP concurrently. */
int vv_native_runtime_report(struct vv_native_runtime *,const struct vv_assessment_request *,struct vv_assessment_result *,struct vv_report_audio_result *);
/* Pure binding: no hardware call until returned callbacks are explicitly used. */
void vv_native_runtime_k7_ops(struct vv_native_runtime_ops *,bool (*ready)(void *),void *);
#endif
