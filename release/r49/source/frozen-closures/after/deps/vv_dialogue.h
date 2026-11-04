#ifndef VV_DIALOGUE_H
#define VV_DIALOGUE_H
#include "vv_https_client.h"
#include "vv_reply_scope.h"
#include <stdatomic.h>
#include <stdint.h>

#define VV_DIALOGUE_ANSWER_CAP 8193u
#define VV_DIALOGUE_HISTORY_TURNS 2u
#define VV_DIALOGUE_HISTORY_TURN_BYTES 4096u
struct vv_dialogue_turn {
  /* NUL-terminated UTF-8 user then assistant; answer_bytes==0 marks an
   * admitted user question whose reply-only cancellation saved no answer. */
  char text[VV_DIALOGUE_HISTORY_TURN_BYTES+2];
  size_t user_bytes,answer_bytes,utf16_units;
};
struct vv_dialogue_ops {
  int (*ask)(void *, const char *, char *, size_t, const struct vv_https_cancel *);
  int (*speak)(void *, const char *, const struct vv_https_cancel *);
  uint64_t (*now_ms)(void *);
  void *arg;
  /* Optional concurrent incremental answer+audio. Same cancellation/cleanup
   * contract as ask/speak; returns an authoritative answer only on success. */
  int (*ask_speak)(void *,const char *,char *,size_t,const struct vv_https_cancel *);
  /* Optional combined-stream override, called with the raw current question.
   * 0=unmatched, no side effect; 1=complete authoritative answer/audio;
   * negative=recognized failure, never fall through to LLM. NULL is legacy.
   * Scope/cancel/retirement/history contracts are identical to ask_speak.
   * Only used by the combined streaming path; never changes device mode. */
  int (*local_reply)(void *,const char *,char *,size_t,const struct vv_https_cancel *,
                     const struct vv_reply_scope_token *);
  /* Default false preserves legacy idle sleep. For an explicitly resident
   * listener only, ordinary monotonic idle expiry drops stale context but
   * accepts the current question while already awake. This never wakes an
   * asleep/stopped conversation and never accepts an invalid/backward clock. */
  bool resident_idle;
};
enum vv_dialogue_reply_phase {
  VV_DIALOGUE_REPLY_IDLE = 0,
  VV_DIALOGUE_REPLY_THINKING = 1,
  VV_DIALOGUE_REPLY_SPEAKING = 2,
  VV_DIALOGUE_REPLY_CANCEL_REQUESTED = 3
};
struct vv_dialogue_reply_status {
  uint64_t id; /* Zero only when idle; never reused within this object lifetime. */
  enum vv_dialogue_reply_phase phase;
};
/* Control-command dispatch belongs to the caller and always runs first.
 * This serial-owner object only handles ordinary utterances after wake.
 * Cancel is thread-safe and retires a reply, never changes gimbal mode.
 * No microphone, KWS, AEC or duplex capability is implied by this module. */
struct vv_dialogue {
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  uint64_t diagnostic_instance; /* Non-reused object lifetime, diagnostic only. */
#endif

  struct vv_dialogue_ops ops;
  atomic_uint generation;
  /* Packed ID/phase gives observers and targeted cancellation one atomic
   * decision point. reply_sequence belongs only to the serial reply owner. */
  atomic_uint_fast64_t reply_state;
  uint64_t reply_sequence;
  atomic_bool busy,sleep_requested;
  bool awake;
  uint64_t last_activity;
  unsigned idle_ms;
  unsigned replies, interruptions;
  int last_error;
  struct vv_dialogue_turn history[VV_DIALOGUE_HISTORY_TURNS];
  unsigned history_count;
};
int vv_dialogue_init(struct vv_dialogue *, const struct vv_dialogue_ops *, unsigned idle_ms);
void vv_dialogue_interrupt(struct vv_dialogue *);
/* Read-only atomic snapshot, not proof of microphone/KWS/AEC acceptance.
 * SPEAKING covers the synchronous speak call, including network and tail drain;
 * it does not assert that the speaker is currently producing audible sound.
 * Legacy/outer cancellation may be pending independently of this phase. */
int vv_dialogue_reply_snapshot(const struct vv_dialogue *, struct vv_dialogue_reply_status *);
/* 1: cancellation accepted for exactly this active reply; 0: stale, already
 * cancelled or completed; -EINVAL: invalid arguments. Never affects a newer
 * ID, wake/sleep, history ownership, gimbal or PHOTO state. Completion and
 * cancellation race atomically: if completion wins, the reply may be committed;
 * if cancellation wins, it cannot enter successful history. No action source
 * is connected here: callers must independently validate speech/echo/epoch. */
int vv_dialogue_cancel_reply_id(struct vv_dialogue *, uint64_t expected_id);
int vv_dialogue_wake(struct vv_dialogue *);
int vv_dialogue_sleep(struct vv_dialogue *);
/* Returns 0 if asleep/empty or completed; negative errors never dispatch motion.
 * Legacy ask/speak speaks the authoritative completed answer in bounded
 * sentences. Optional ask_speak may start audio before answer completion;
 * failed/partial answers are never remembered. An admitted same-ID reply-only
 * cancellation retains its user question with an explicit interrupted marker;
 * STOP/outer/generation cancellation does not. Backend stays stateless;
 * up to two successful or interrupted user turns are supplied as bounded local
 * context inside its existing text field, without inventing assistant text.
 * Oldest complete turns are discarded to fit the Java DTO's 2000 UTF-16 units
 * (therefore also <=2000 Unicode scalars); an oversized current question is
 * rejected with -E2BIG and never sent. The
 * current user utterance is never truncated. A turn above the fixed history
 * byte cap clears older context and is not stored. Sleep/expiry clears history.
 * Sleep may be called during a reply: it requests cancellation immediately and
 * the owner clears session/history on unwind. Ask/speak are synchronous and must
 * honor cancellation, stop playback and join their workers before returning. */
int vv_dialogue_reply(struct vv_dialogue *, const char *, const struct vv_https_cancel *);
/* Explicit diagnostic scope route; legacy reply() is unchanged. The caller
 * pins scope + dialogue + audio owner throughout this synchronous operation.
 * asr_epoch must come from the completed input, never allocated for an old
 * queued event to make it appear current. No KWS source is connected here. */
typedef int (*vv_dialogue_scoped_speak)(void *,const char *,char *,size_t,
  const struct vv_https_cancel *,const struct vv_reply_scope_token *);
int vv_dialogue_reply_scoped(struct vv_dialogue *,const char *,
  const struct vv_https_cancel *,struct vv_reply_scope *,uint64_t owner_epoch,
  uint32_t asr_epoch,vv_dialogue_scoped_speak,void *);
#endif
