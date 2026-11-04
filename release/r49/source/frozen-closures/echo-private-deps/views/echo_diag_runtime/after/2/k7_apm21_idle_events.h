#ifndef K7_APM21_IDLE_EVENTS_H
#define K7_APM21_IDLE_EVENTS_H
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
#if !K7_APM21_LIVE_OBSERVER || !K7_APM21_PREPARED_LIFECYCLE
#error idle events require pinned prepared live session lifetime
#endif
#include "voice_session.h"
#include <stdatomic.h>
#define K7_APM21_IDLE_SLOTS 32u
enum { K7_APM21_IDLE_OFF=1,K7_APM21_IDLE_MIC,K7_APM21_IDLE_SOURCE_END,K7_APM21_IDLE_END,K7_APM21_IDLE_DIGITAL_QUIET };
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#define K7_APM21_IDLE_TX_DISABLED 6u
#endif
/* Accepted-after control observations, NEVER ADC/DAC timestamps or zero
 * reference authority. MIC PCM is real, not reference PCM. Consumer must pin
 * the original session/owner and retain OLD context by generation/epoch. */
struct k7_apm21_idle_event {
 uint64_t serial,first,next;
 uint32_t kind,input_epoch;
 struct k7_voice_anchor off,boundary;
 unsigned samples,quiet_checks;uint32_t xfer;
 int16_t pcm[320];
};
struct k7_apm21_idle_events {
 atomic_uint produced,consumed;
 atomic_int fault;
 struct k7_apm21_idle_event slots[K7_APM21_IDLE_SLOTS];
 /* Only original PIO producer below; consumer never reads this state. */
 struct k7_apm21_idle_event partial;
 struct k7_voice_anchor off;
 uint64_t serial,next,skip_until,last_us;
 uint32_t input_epoch;
 unsigned active,pending,ended,quiet_seen;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 unsigned disabled_seen;
#endif
};
void k7_apm21_idle_init(struct k7_apm21_idle_events *);
void k7_apm21_idle_fault(struct k7_apm21_idle_events *,int);
void k7_apm21_idle_anchor(struct k7_apm21_idle_events *,const struct k7_voice_anchor *,uint32_t input_epoch);
void k7_apm21_idle_microphone(struct k7_apm21_idle_events *,int16_t,uint64_t,uint32_t input_epoch,uint32_t anchor_epoch);
/* Bit checks: mute success, TX disable write, TX idle, clear complete/empty,
 * RX+clock still enabled. Does NOT assert acoustic silence or ADC alignment. */
#define K7_APM21_IDLE_QUIET_CHECKS 31u
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
void k7_apm21_idle_disabled(struct k7_apm21_idle_events *,const struct k7_voice_anchor *,uint32_t input,int read_rc,uint32_t xfer);
void k7_voice_session_apm21_idle_disabled(struct k7_voice_session *,uint32_t generation,uint64_t rx_next,uint64_t before,uint64_t after,int read_rc,uint32_t xfer);
#endif
void k7_apm21_idle_quiet(struct k7_apm21_idle_events *,const struct k7_voice_anchor *,uint32_t input_epoch,unsigned checks,uint32_t xfer);
int k7_apm21_idle_take(struct k7_apm21_idle_events *,struct k7_apm21_idle_event *);
/* Sole worker consumer only; session must outlive PIO and this drain. */
struct k7_apm21_idle_events *k7_voice_session_apm21_idle_events(struct k7_voice_session *);
void k7_voice_session_apm21_idle_quiet(struct k7_voice_session *,uint32_t generation,uint64_t rx_next,uint64_t before,uint64_t after,unsigned checks,uint32_t xfer);
#endif
#endif
