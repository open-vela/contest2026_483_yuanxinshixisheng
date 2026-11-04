#ifndef K7_ECHO_DIAG_RUNTIME_H
#define K7_ECHO_DIAG_RUNTIME_H
#include "voice_session.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional diagnostic only: no cleaned-audio or action consumer exists.
 * Serial manager creates while holding its session lifetime, taps from its
 * SOLE KWS consumer, joins that producer, then closes before session destroy.
 * A failed close retains both object and borrowed session; never detach/free.
 * Enable/disable latch the NEXT session configuration; no hot enable/restart.
 * requested versus attached status distinguishes those facts. An attached
 * diagnostic owner rejects either change; a detached frozen capture must be
 * explicitly cleared before a fresh diagnostic owner can attach.
 * Disabled means no thread/allocation. */
struct k7_echo_runtime;
bool k7_echo_runtime_requested(void);
int k7_echo_runtime_open(struct k7_voice_session *, uint64_t owner,
                         struct k7_echo_runtime **);
/* Bounded SPSC copy only, no lock/wait/DSP/logging. Failure affects diagnostics. */
void k7_echo_runtime_tap(struct k7_echo_runtime *, const struct k7_voice_frame *);
int k7_echo_runtime_close(struct k7_echo_runtime **);
/* enable/disable before native-run; arm next TX only while TX IDLE;
 * status; abort; clear; dump mic|reference|residual|zero OFFSET COUNT;
 * anchors OFFSET COUNT. Dump only frozen; counts <=1024 samples/32 anchors.
 * Joined capture survives native-stop until explicit clear; contains only
 * complete paired prefix frames, NOT a complete DAC/acoustic turn. */
/* Independent diagnostic RAW tail snapshot (same worker/tap, no AEC input):
 * echo-diag-tail-arm while TX idle; next valid AMP_OFF locks >=5120 prior
 * samples plus >=16000 subsequent samples, rounded to whole320 frames.
 * echo-diag-tail-status; echo-diag-tail-dump OFFSET COUNT (<=1024), frozen only;
 * echo-diag-tail-clear clears a completed/failed snapshot. echo-diag-abort
 * aborts both captures. Unknown/short/gapped data freezes incomplete; stop
 * never pads. Paired capture state is independent. Control mute is not DAC
 * silence or a clean-audio qualification. Parent owner must stay pinned. */
#ifdef K7_ECHO_RAW_VAD_OBSERVER
#include "raw_vad_observer.h"
/* Optional diagnostic fork only; the old open path lends no model and performs
 * no inference/allocation for VAD. The caller pre-creates and EXCLUSIVELY lends
 * a model until successful close. No callback may wait, take manager/platform
 * locks, or issue actions. Failure leaves ordinary voice owner unchanged. */
typedef void (*k7_raw_vad_sink)(void *,const struct k7_raw_vad_report *);
int k7_echo_runtime_open_vad(struct k7_voice_session *,uint64_t owner,
 struct k7_vad *,k7_raw_vad_sink,void *,struct k7_echo_runtime **);
/* Snapshot via worker mailbox while attached; immutable cached stats after
 * join. Cached armed/started describe the LAST worker snapshot; destroy then
 * invalidates the borrowed model. Holds control only in command thread, never in audio/tap/inference.
 * Timeout leaves pending command; no partial/racing struct is returned. */
int k7_echo_runtime_vad_stats(struct k7_echo_runtime *,struct k7_raw_vad_stats *);
#endif
int k7_echo_runtime_command(int argc,char **argv);
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
int k7_echo_runtime_open_residual_vad(struct k7_voice_session *,uint64_t,struct k7_vad *,struct k7_echo_runtime **);
#endif
#ifdef __cplusplus
}
#endif
#endif

#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
int k7_echo_runtime_rearm_owner(uint64_t owner);
#endif
