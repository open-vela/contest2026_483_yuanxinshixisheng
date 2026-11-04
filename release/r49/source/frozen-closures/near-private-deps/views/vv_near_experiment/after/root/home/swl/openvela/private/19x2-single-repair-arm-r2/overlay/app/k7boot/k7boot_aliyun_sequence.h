#ifndef K7BOOT_ALIYUN_SEQUENCE_H
#define K7BOOT_ALIYUN_SEQUENCE_H
#include "../k7agent/cloud/include/vv_k7_platform_factory.h"
#include "../k7agent/cloud/include/vv_aliyun_provider.h"
struct k7boot_aliyun_config {
 struct vv_k7_platform_boot_config platform;
 vv_aliyun_key_load_fn load_key;void *key_arg;
 const char *asr_model,*tts_model,*voice;
 bool allow_proven_bj_compat;
 const struct vv_prompt_pcm_asset *prompt_assets;size_t prompt_asset_count;
 bool announce_network_ready;
 /* Local immutable PCM callback, usable before TLS/time initialization. */
 int (*play_network_prompt)(void *,const struct vv_https_cancel *);
 void *network_prompt_arg;
 /* EAGAIN means wait in preview. A successful take consumes/seals one
  * explicitly approved time-source sample; never call after factory starts. */
 int (*take_trusted_time)(void *,const struct vv_https_cancel *,struct vv_trusted_time_sample *);
 void *trusted_time_arg;
 /* Optional explicit business login, performed before service binding. */
 vv_k7_secure_load_fn load_business;void *business_arg;
 int (*prepare_business)(struct vv_native_platform_config *,void *);
 void *prepare_business_arg;
 /* Optional validated endpoint; provider copies this borrowed input. */
 const char *speech_host;
 /* Default false. Explicit independent-speech startup only, with DIALOGUE
  * business maintenance compiled. HTTP/AUTH business-login failures may defer
  * login to the existing serial loader; PHOTO remains blocked until auth.
  * All other errors stay fatal. No config, trust, TLS or cancellation bypass. */
 bool allow_business_login_deferred;
};
/* Explicit, one-shot process-lifetime candidate entry. Caller supplies real
 * authority/CA, exclusion callbacks and secure key loader. No embedded secret,
 * default authority or old TCP speech bridge. Business login requires an
 * explicit loader and completes before binding unless the narrow deferred
 * policy is explicitly enabled. All config and
 * callback state remains alive through successful cleanup. Current preview is
 * bounded to 3 hours. Automatic startup registration is deliberately separate.
 * run waits for the asynchronous service worker to actually terminate before
 * stop/hold and cleanup. Cancellation requests do not release borrowed state.
 * Factory is one-shot; no retry after failure. */
int k7boot_aliyun_run(const struct k7boot_aliyun_config *);
void k7boot_aliyun_cancel(void);
/* Retained numeric stage/result, including failures before service binding. */
void k7boot_aliyun_status_print(void);
#ifdef CONFIG_EXAMPLES_K7SOUND_VAD_PROBE
/* Diagnostic-profile only. Probe owns admission until resources retire.
 * Boot prepare waits at most30s, then returns ETIMEDOUT; cancellation and
 * clock errors remain ECANCELED/EIO. Once prepare begins no probe can enter. */
int k7boot_aliyun_vad_probe_acquire(void);
int k7boot_aliyun_vad_probe_release(void);
#endif
/* Call serially only after run returned. If platform retirement fails, retains
 * provider and all borrowed state, preventing use-after-free. */
int k7boot_aliyun_cleanup(void);
#endif
