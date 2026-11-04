/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_RAW_VAD_OBSERVER_H
#define K7_RAW_VAD_OBSERVER_H
#include "voice_session.h"
#include "k7_vad.h"

#define K7_RAW_VAD_MAX_AGE 8000u
enum k7_raw_vad_failure {
  K7_RAW_VAD_OK, K7_RAW_VAD_FRAME, K7_RAW_VAD_CONTINUITY,
  K7_RAW_VAD_OWNER, K7_RAW_VAD_STALE, K7_RAW_VAD_CLOCK,
  K7_RAW_VAD_MODEL, K7_RAW_VAD_EXHAUSTED, K7_RAW_VAD_TAP_LOSS
};
/* Numeric RAW observations only. No near-end or clean authority, no speech
 * threshold, no reply cancellation. Current TX fields are context snapshots;
 * they do NOT bind every sample of the model window to a reply generation.
 * sample endpoints are exclusive. Model576/512 and completed-frame320 ranges
 * deliberately remain distinct. frame ordinal is local, not a hardware ID.
 * monotonic_*_us use CLOCK_MONOTONIC. They are NOT PIO anchor timestamps or
 * a proven mapping to the PIO counter; neither is acoustic/DAC time. owner
 * is the caller's label for its borrowed pinned session, not authentication. */
struct k7_raw_vad_report {
  struct k7_vad_observation model;
  uint64_t owner, segment, local_frame_ordinal;
  uint64_t completion_frame_first, completion_frame_end;
  uint64_t microphone_before, microphone_after;
  uint64_t monotonic_before_us, monotonic_after_us;
  uint32_t input_epoch, tx_generation_before, tx_generation_after;
  unsigned tx_state_before, tx_state_after;
  unsigned raw, clean, qualified, reply_binding_verified;
};
struct k7_raw_vad_stats {
  uint64_t frames, observations, rejected, segments;
  uint64_t last_window_end, last_completion_end;
  enum k7_raw_vad_failure last_failure;
  bool armed, started;
};
struct k7_raw_vad;
/* The caller lends an EXCLUSIVELY owned, pre-created model and pins the actual
 * session until destroy. Creating/loading ORT is deliberately outside this API
 * and outside audio/KWS consumers. All methods below are single-worker only. */
int k7_raw_vad_create(struct k7_voice_session *, uint64_t owner,
                     struct k7_vad *borrowed_model, struct k7_raw_vad **);
/* Explicit recovery starts a new model segment on the NEXT complete real
 * frame. No fabricated padding/preroll, no identity reuse after a gap. */
int k7_raw_vad_rearm(struct k7_raw_vad *);
void k7_raw_vad_invalidate(struct k7_raw_vad *, enum k7_raw_vad_failure);
/* Returns1 numeric report,0 accumulated genuine samples,negative reject.
 * Output remains untouched unless1. Freshness is checked before AND after
 * inference against the actual session; original model endpoint has its own
 * age check. A clock/session change during inference invalidates the segment. */
int k7_raw_vad_feed(struct k7_raw_vad *, const struct k7_voice_frame *,
                    struct k7_raw_vad_report *);
int k7_raw_vad_get_stats(const struct k7_raw_vad *, struct k7_raw_vad_stats *);
void k7_raw_vad_destroy(struct k7_raw_vad *);
#endif
