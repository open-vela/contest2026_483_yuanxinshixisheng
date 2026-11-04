#ifndef K7_APM21_PREPARED_LIFECYCLE_H
#define K7_APM21_PREPARED_LIFECYCLE_H
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
#if !defined(K7_APM21_LIVE_OBSERVER) || !K7_APM21_LIVE_OBSERVER || !defined(K7_APM21_PHYSICAL_STREAM) || !K7_APM21_PHYSICAL_STREAM
#error prepared lifecycle requires live observer and physical stream
#endif
#include "k7_apm21_live_wiring.h"
#include <stdint.h>
struct k7_echo_runtime;struct k7_voice_session;struct k7_raw_vad_loan;struct k7_vad;
/* NULL/UNKNOWN declines BEFORE taking the sole model loan or allocating.
 * No verified live profile is provided in this draft. Caller's receipt binding
 * and missing idle-reference coverage remain activation blockers. */
int k7_raw_vad_owner_preopen_residual(uint64_t,const struct k7_apm21_live_binding *,
 struct k7_raw_vad_loan *,struct k7_echo_runtime **);
int k7_echo_runtime_preopen_residual(uint64_t,struct k7_vad *,
 const struct k7_apm21_live_binding *,struct k7_echo_runtime **);
int k7_echo_runtime_attach_prepared(struct k7_echo_runtime *,struct k7_voice_session *,uint64_t);
int k7_echo_runtime_pause_prepared(struct k7_echo_runtime *,uint64_t);
/* For manager-owned dispatch only: 1 iff this is a preopened context. */
int k7_echo_runtime_is_prepared(struct k7_echo_runtime *);
int k7_voice_session_apm21_retirement_check(struct k7_voice_session *);
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
int k7_raw_vad_owner_take_apm21_diagnostic(uint64_t,struct k7_apm21_live_binding *);
#endif
#line 25
#endif
#endif
