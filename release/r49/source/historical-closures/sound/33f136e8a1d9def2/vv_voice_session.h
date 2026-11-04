#ifndef VV_VOICE_SESSION_H
#define VV_VOICE_SESSION_H
#include <stdint.h>
#include <stdbool.h>
struct k7_voice_session;
struct vv_native_runtime;
/* Service-owner lifetime: start before providers, stop after every provider
 * call has returned. Borrowed audio stays alive until stop completes. */
int vv_voice_session_start(struct vv_native_runtime *);
int vv_voice_session_stop(void);
/* Serial service owner only, between completed provider calls. Health is zero
 * only while both the consumer and physical RX are available. Restart fully
 * joins and destroys the old owner before creating one replacement; a cleanup
 * failure retains the old state and never creates another owner. No mode,
 * pending upload, utterance or playback generation is replayed. */
int vv_voice_session_health(void);
int vv_voice_session_restart(struct vv_native_runtime *);
struct k7_voice_session *vv_voice_session_audio(void);
/* Fresh identifiers never wrap or reuse a prior window/playback generation.
 * Zero reports exhaustion or an inactive session. */
uint32_t vv_voice_session_next_asr_epoch(void);
uint32_t vv_voice_session_next_tx_generation(void);
/* Read only within the service's existing borrowed-session lifetime. Zero
 * means not current/ready. Epoch never reuses after restart in this process. */
uint64_t vv_voice_session_owner_epoch(const struct k7_voice_session *);

/* Default unregistered, synchronous worker/control-context event gate.
 * No model inference, network, service stop/start, or event API reentry in
 * callbacks. The manager pins its current audio and callback arg under a
 * short mutex for each call; unregister returns only after callbacks finish.
 * Register/unregister are serial platform-owner operations. Raw KWS is never
 * submitted here by the existing diagnostic consumer. */
struct vv_confirmed_wake_context;
struct vv_confirmed_wake_event;
struct vv_voice_event_state {
 uint64_t owner_epoch,microphone_samples,now_ms;
 uint32_t input_epoch,tx_generation;
 bool tx_active;
};
/* Concurrent value-only identity observation. Pins the current audio only
 * while copying under the manager lifetime mutex; never returns a borrowed
 * pointer and never invokes registered callbacks. Stop publishes stopping
 * under that same mutex before teardown. No acoustic qualification is implied.
 * Output is zero on failure. Caller must recheck identity at later decisions. */
int vv_voice_session_observe(struct vv_voice_event_state *);
struct vv_voice_event_ops {
 int (*snapshot)(void *,const struct vv_voice_event_state *,struct vv_confirmed_wake_context *);
 int (*submit)(void *,const struct vv_voice_event_state *,const struct vv_confirmed_wake_event *);
 void *arg;
};
int vv_voice_session_event_register(const struct vv_voice_event_ops *);
int vv_voice_session_event_unregister(const struct vv_voice_event_ops *);
/* Snapshot is an identity observation only, never proof of clean speech.
 * sample_next/now_ms use real manager microphone/monotonic counters. */
int vv_voice_session_event_snapshot(struct vv_confirmed_wake_context *,
 uint64_t *sample_next,uint64_t *now_ms);
/* Immutable value-copy; 1 accepted, 0 stale/consumed, negative rejected/error.
 * A clean confirmation still needs the registered independent verifier. */
int vv_voice_session_event_submit(const struct vv_confirmed_wake_event *);
/* Optional idle-only raw candidate staging. Register/unregister and select/claim
 * belong to the serial platform/service owner. epoch is a short atomic-only
 * getter; it must never call back into manager or access mutable PHOTO state.
 * Manager creates/owns the queue and its sole KWS consumer copies each real
 * frame before model evaluation. No candidate causes an action. */
struct vv_voice_idle_ops { unsigned (*mode_epoch)(void *); void *arg; };
struct vv_pf_wake_source;
int vv_voice_session_idle_register(const struct vv_voice_idle_ops *);
int vv_voice_session_idle_unregister(const struct vv_voice_idle_ops *);
int vv_voice_session_idle_select(const struct vv_voice_idle_ops *,bool eligible,
 unsigned mode_epoch);
/* 1: returns a claimed immutable source with a lifetime pin; 0: no candidate.
 * Caller must either arm it into PF (which finishes once) or finish(cancel).
 * Stop/unregister cannot destroy a live claimed source. */
int vv_voice_session_idle_claim(const struct vv_voice_idle_ops *,struct vv_pf_wake_source *);
#endif
