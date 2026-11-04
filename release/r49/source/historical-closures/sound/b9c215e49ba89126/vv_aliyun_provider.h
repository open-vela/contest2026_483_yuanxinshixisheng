#ifndef VV_ALIYUN_PROVIDER_H
#define VV_ALIYUN_PROVIDER_H
#include "vv_https_mbedtls.h"
#include "vv_speech_provider.h"
#include "vv_prompt_cache.h"
/* Loader must fill a NUL-terminated ASCII key <=2048 bytes, return zero only
 * on success, never log it. Called only after trusted TLS creation succeeds. */
typedef int (*vv_aliyun_key_load_fn)(void *,char *,size_t);
struct vv_aliyun_provider_config {
 struct vv_https_mbedtls_config tls;
 struct vv_https_limits limits;
 vv_aliyun_key_load_fn load_key;void *key_arg;
 const char *asr_model,*tts_model,*voice;
 bool allow_proven_bj_compat;
 const struct vv_prompt_pcm_asset *prompt_assets;size_t prompt_asset_count;
 /* NULL keeps the historical endpoint. Validated/copied at create. */
 const char *speech_host;
};
struct vv_aliyun_provider_owner;
/* No DNS, HTTPS request or audio. TLS may seed approved entropy. CA and TLS
 * callback state remain borrowed; models and secret are copied into owner.
 * Existing China endpoint or explicit Beijing workspace endpoint only.
 * speech_host is copied; paths/models/format and download host are unchanged. */
int vv_aliyun_provider_create(const struct vv_aliyun_provider_config *,struct vv_aliyun_provider_owner **);
/* Descriptor borrowed until destroy. Client/platform must be retired first.
 * One owner thread: no concurrent new call/destroy. In-flight destroy refuses. */
const struct vv_speech_provider *vv_aliyun_provider_speech(struct vv_aliyun_provider_owner *);
/* CONFIG_EXAMPLES_K7CLOUD_ALIYUN_CHAT exposes optional Qwen Flash text chat on
 * the same trusted TLS/key/active owner. Disabled builds leave chat NULL. */
int vv_aliyun_provider_destroy(struct vv_aliyun_provider_owner *);
/* Board-side lazy prefetch: at most one fixed phrase per call, six per owner.
 * 0=one phrase attempted/skipped; 1=all six already attempted; negative=error.
 * Each started attempt is consumed even on failure; no automatic retries.
 * Caller schedules only in idle gaps, with a deadline/ASR-priority cancel token;
 * never loop all six before first listen. Busy/cancel before work consumes none.
 * No playback. Successful PCM enters the same cache used by speak(). */
int vv_aliyun_provider_warmup_next(struct vv_aliyun_provider_owner *,
 const struct vv_https_cancel *,unsigned *attempted);
#endif
