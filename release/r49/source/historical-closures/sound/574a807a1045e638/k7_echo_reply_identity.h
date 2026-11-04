#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#ifndef K7_ECHO_REPLY_IDENTITY_H
#define K7_ECHO_REPLY_IDENTITY_H
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include <stdint.h>
#include <stdbool.h>
/* Diagnostic values only. No borrowed pointers, MIC owner, authority, actions,
 * or acoustic mapping. Call from workers/control, never PIO. One slot is NOT
 * an event log: every lifecycle transition advances a non-reused revision;
 * consumers reject missing transitions rather than reconstructing history.
 * The mailbox mutex protects values only, never calls into scope/session. */
struct k7_echo_reply_identity {
 uint64_t instance,revision,scope_incarnation,lease_serial,owner,reply;
 uint64_t cancel_observed_us;
 uint32_t asr,input,tx;
 bool live,bound,cancelled;
 bool retired_terminal; /* set only by successful explicit retirement */
};
int k7_echo_reply_begin(uint64_t dialogue,uint64_t scope,uint64_t lease,
 uint64_t owner,uint64_t reply,uint32_t asr);
int k7_echo_reply_bind(uint64_t scope,uint64_t lease,uint64_t owner,
 uint32_t input,uint32_t tx);
int k7_echo_reply_cancel(uint64_t dialogue,uint64_t reply);
int k7_echo_reply_retire(uint64_t scope,uint64_t lease);
int k7_echo_reply_snapshot(struct k7_echo_reply_identity *);
#endif
#endif

#else
#ifndef K7_ECHO_REPLY_IDENTITY_H
#define K7_ECHO_REPLY_IDENTITY_H
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include <stdint.h>
#include <stdbool.h>
/* Diagnostic values only. No borrowed pointers, MIC owner, authority, actions,
 * or acoustic mapping. Call from workers/control, never PIO. One slot is NOT
 * an event log: every lifecycle transition advances a non-reused revision;
 * consumers reject missing transitions rather than reconstructing history.
 * The mailbox mutex protects values only, never calls into scope/session. */
struct k7_echo_reply_identity {
 uint64_t instance,revision,scope_incarnation,lease_serial,owner,reply;
 uint64_t cancel_observed_us;
 uint32_t asr,input,tx;
 bool live,bound,cancelled;
};
int k7_echo_reply_begin(uint64_t dialogue,uint64_t scope,uint64_t lease,
 uint64_t owner,uint64_t reply,uint32_t asr);
int k7_echo_reply_bind(uint64_t scope,uint64_t lease,uint64_t owner,
 uint32_t input,uint32_t tx);
int k7_echo_reply_cancel(uint64_t dialogue,uint64_t reply);
int k7_echo_reply_retire(uint64_t scope,uint64_t lease);
int k7_echo_reply_snapshot(struct k7_echo_reply_identity *);
#endif
#endif

#endif
