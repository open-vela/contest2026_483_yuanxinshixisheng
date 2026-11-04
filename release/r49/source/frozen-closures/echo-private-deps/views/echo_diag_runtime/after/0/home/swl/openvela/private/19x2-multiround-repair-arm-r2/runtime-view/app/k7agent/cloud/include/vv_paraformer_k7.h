#ifndef VV_PARAFORMER_K7_H
#define VV_PARAFORMER_K7_H
#include "vv_paraformer.h"
#include "vv_https_mbedtls.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_wake_input.h"
/* The serial service arms one input already claimed and pinned by manager.
 * Adapter copies this value, consumes the stage for the ENTIRE task and calls
 * finish exactly once after its last source access. It never splices to or
 * drains the ordinary ASR ring. Callbacks must not reenter adapter APIs.
 * arm failure leaves ownership with caller. disarm releases an unconsumed
 * source (including argument/thread-start failures); it is idempotent after
 * listen. Physical manager remains owned by service through provider return.
 * Cancellation must still release the source; finish returns actual consumed
 * bounds only for a healthy, complete keyword-containing staged stream. */
struct vv_pf_wake_source {
 void *arg;
 struct vv_wake_request request;
 int (*health)(void *,const struct vv_wake_request *);
 int (*take)(void *,const struct vv_wake_request *,struct k7_voice_frame *);
 int (*finish)(void *,const struct vv_wake_request *,bool cancel,uint64_t *first,uint64_t *next);
};
int vv_paraformer_k7_wake_arm(const struct vv_pf_wake_source *);
int vv_paraformer_k7_wake_disarm(void);
#endif
/* Explicit fixed-path endpoint. Host is copied into the joined network job;
 * only the old endpoint or a Beijing ws-* endpoint is accepted. */
int vv_paraformer_k7_listen_at_host(struct vv_https_mbedtls *,const char *host,const char *key,
 vv_https_now_ms_fn,void *,unsigned samples,const struct vv_https_cancel *,struct vv_asr_batch_result *);
int vv_paraformer_k7_listen(struct vv_https_mbedtls *,const char *key,
 vv_https_now_ms_fn,void *,unsigned samples,const struct vv_https_cancel *,struct vv_asr_batch_result *);
/* Release a retained no-command ADC window; idempotent, -EBUSY while listening.
 * With VOICE_SESSION enabled, close ends only ASR-window ownership and never
 * stops shared microphone/KWS. Manager must keep the session alive until this
 * provider has returned; each listen uses a fresh, non-reusable ASR epoch. */
int vv_paraformer_k7_close(void);
#endif
