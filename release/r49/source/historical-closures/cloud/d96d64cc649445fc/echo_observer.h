#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#ifndef K7_ECHO_OBSERVER_H
#define K7_ECHO_OBSERVER_H
#include "near_evidence.h"
#include "voice_session.h"
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "k7_apm21_nominal_identity.h"
#endif
/* Borrowed for callback duration only; real paired worker data, NOT clean proof. */
struct k7_echo_packet {
 struct ne_identity identity;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 struct k7_apm21_nominal_identity apm_nominal;
 unsigned apm_post_stop_hypothesis_samples; /* FIFO hypothesis, never acoustic zero. */
#endif
 struct k7_voice_frame raw,residual;
 const int16_t *reference;
 const struct ne_report *report;
 uint64_t reference_first;
 unsigned applied_lag_samples; /* explicit experimental transform parameter */
 bool cancel_observed;
 unsigned source_stop_zero_samples; /* modeled stopped excitation, NOT acoustic zeros */
};
typedef void (*k7_echo_observer_fn)(void *,const struct k7_echo_packet *,int);
int k7_echo_observer_subscribe(k7_echo_observer_fn,void *);
/* Never from callback. Success guarantees no callback borrows arg. */
int k7_echo_observer_unsubscribe(k7_echo_observer_fn,void *);
/* Existing single serial echo worker only, never PIO. reason != 0 revokes. */
void k7_echo_observer_emit(const struct k7_echo_packet *,int reason);
#endif

#else
#ifndef K7_ECHO_OBSERVER_H
#define K7_ECHO_OBSERVER_H
#include "near_evidence.h"
#include "voice_session.h"
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "k7_apm21_nominal_identity.h"
#endif
/* Borrowed for callback duration only; real paired worker data, NOT clean proof. */
struct k7_echo_packet {
 struct ne_identity identity;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 struct k7_apm21_nominal_identity apm_nominal;
 unsigned apm_post_stop_hypothesis_samples; /* FIFO hypothesis, never acoustic zero. */
#endif
 struct k7_voice_frame raw,residual;
 const int16_t *reference;
 const struct ne_report *report;
 uint64_t reference_first;
 bool cancel_observed;
 unsigned source_stop_zero_samples; /* modeled stopped excitation, NOT acoustic zeros */
};
typedef void (*k7_echo_observer_fn)(void *,const struct k7_echo_packet *,int);
int k7_echo_observer_subscribe(k7_echo_observer_fn,void *);
/* Never from callback. Success guarantees no callback borrows arg. */
int k7_echo_observer_unsubscribe(k7_echo_observer_fn,void *);
/* Existing single serial echo worker only, never PIO. reason != 0 revokes. */
void k7_echo_observer_emit(const struct k7_echo_packet *,int reason);
#endif

#endif
