/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_VOICE_SESSION_H
#define K7_VOICE_SESSION_H
#include "voice_frame_ring.h"
#include "fifo_reference.h"
#include <stdbool.h>
#include <stddef.h>
#define K7_VOICE_TX_SAMPLES 16384u
#define K7_VOICE_ANCHOR_SLOTS 64u
enum { K7_VOICE_SESSION_STALE = -20001 }; /* portable old-generation rejection */
enum k7_voice_tx_state { K7_VOICE_TX_IDLE, K7_VOICE_TX_RESETTING,
                         K7_VOICE_TX_PREFILL, K7_VOICE_TX_RUNNING,
                         K7_VOICE_TX_DRAINING, K7_VOICE_TX_PAUSED };
enum k7_voice_tx_action { K7_VOICE_TX_WAIT, K7_VOICE_TX_START,
                          K7_VOICE_TX_CONTINUE, K7_VOICE_TX_CANCEL };
struct k7_voice_session;
struct k7_voice_session_config {
  unsigned tx_prebuffer_samples; /* 8..16384; e.g.3200=200ms at16k */
  bool render_reference;         /* true requires a reference consumer */
  bool timing_anchors;           /* false by default; true requires a drain */
  bool fifo_reference;           /* default off; separate on-demand paired enqueue diagnostics */
};
enum k7_voice_anchor_event {
  K7_VOICE_ANCHOR_TX_START = 1, K7_VOICE_ANCHOR_RX_FRAME,
  K7_VOICE_ANCHOR_TX_FRAME, K7_VOICE_ANCHOR_TX_STOP,
  K7_VOICE_ANCHOR_TX_CLEAR, K7_VOICE_ANCHOR_TX_COMPLETE,
  K7_VOICE_ANCHOR_SESSION_STOP, K7_VOICE_ANCHOR_TX_PAUSE,
  K7_VOICE_ANCHOR_TX_RESUME, K7_VOICE_ANCHOR_AMP_OFF
};
enum k7_voice_anchor_flags {
  K7_VOICE_ANCHOR_GAP = 1u, K7_VOICE_ANCHOR_IO_ERROR = 2u,
  K7_VOICE_ANCHOR_CLOCK_INVALID = 4u, K7_VOICE_ANCHOR_COUNT_INVALID = 8u
};
/* All times use the same pio_port.us monotonic counter, in microseconds.
 * Reads are sequential and bracketed, not simultaneous. RX is software-delivered
 * mono samples; TX queued is the next per-generation FIFO enqueue index.
 * FIFO/discarded fields count 32-bit words, NOT mono samples. No data-counter
 * register is consulted. These are FIFO timing observations, NOT DAC/acoustic
 * timestamps or a validated AEC alignment; codec/physical latency is unknown. */
struct k7_voice_anchor {
  uint64_t before_us, after_us, rx_samples;
  uint32_t generation, tx_queued_samples;
  uint32_t rx_fifo_words, tx_fifo_words, discarded_words;
  uint32_t epoch, event, flags;
  int result;
};
struct k7_voice_tx_result {
  uint32_t generation;
  int result;
  unsigned queued_samples, discarded_words, pauses;
};
struct k7_voice_session_status {
  bool running, ready, stop_requested, finished;
  int result;
  uint64_t microphone_samples;
  uint32_t input_epoch, asr_epoch, asr_failed_epoch;
  unsigned kws_overflows, asr_overflows, render_overflows;
  unsigned anchor_overflows, anchor_faults;
  uint32_t anchor_epoch;
  enum k7_voice_tx_state tx_state;
  uint32_t tx_generation;
  unsigned tx_pauses;
  /* Valid only after finished/join. Runtime failure is independent of verified
   * physical cleanup; false prohibits replacing this owner with a new one. */
  bool cleanup_ok;
  int cleanup_error;
};
/* run executes in one CPU1 worker on NuttX/SMP. It must acquire the physical
 * audio owner before touching hardware, poll stop_requested continuously and
 * release hardware before return. It may not expose simulated readiness.
 * create allocates queues once; no allocation or locks in the audio callbacks. */
typedef int (*k7_voice_session_run)(void *, struct k7_voice_session *);
int k7_voice_session_create(const struct k7_voice_session_config *,
                            struct k7_voice_session **);
int k7_voice_session_start(struct k7_voice_session *, k7_voice_session_run, void *);
void k7_voice_session_stop(struct k7_voice_session *);
int k7_voice_session_join(struct k7_voice_session *);
int k7_voice_session_destroy(struct k7_voice_session *);
size_t k7_voice_session_storage_bytes(void);
void k7_voice_session_status(const struct k7_voice_session *, struct k7_voice_session_status *);
/* One network producer, concurrent audio consumer. begin never replaces an
 * active generation: cancel it and await result before beginning another.
 * Generations strictly increase within a session; reuse is rejected.
 * write is all-or-nothing, -EAGAIN means producer-side queue pressure; it does
 * not block audio or manufacture silence. Old-generation writes are rejected. */
int k7_voice_session_tx_begin(struct k7_voice_session *, uint32_t generation);
/* Opt-in network stream: 8..16384 sample buffers, 1..5000ms per refill wait,
 * pauses are counted without a small fixed retry limit. The caller must bound
 * the overall turn (production speaker: 90s and 480000 source samples).
 * A pause retains this generation and sample cursor;
 * the physical stream remains active with separately counted gap zeros.
 * These zeros are not source/render PCM. Ordinary tx_begin keeps fail-on-gap.
 * Software starvation may pause; a physical FIFO error is always a failure. */
int k7_voice_session_tx_begin_stream(struct k7_voice_session *, uint32_t generation,
                                    unsigned prebuffer, unsigned rebuffer,
                                    unsigned pause_timeout_ms);
/* Explicit complete-segment natural TTS policy: the same queue/generation and
 * 8..16384 sample buffers, but 1..20000ms per software refill. Only a caller
 * retaining its own total deadline and cancellation coordinator may opt in.
 * This does not change tx_begin_stream's 5000ms cap or hardware-fault handling. */
int k7_voice_session_tx_begin_verified_stream(struct k7_voice_session *, uint32_t generation,
                                             unsigned prebuffer, unsigned rebuffer,
                                             unsigned pause_timeout_ms);
int k7_voice_session_tx_write(struct k7_voice_session *, uint32_t generation,
                              const int16_t *, unsigned samples);
int k7_voice_session_tx_end(struct k7_voice_session *, uint32_t generation);
int k7_voice_session_tx_cancel(struct k7_voice_session *, uint32_t generation);
int k7_voice_session_tx_result(const struct k7_voice_session *, uint32_t generation,
                               struct k7_voice_tx_result *);
/* 0 disables ASR; use a new nonzero epoch for every fresh utterance. A full
 * ASR queue fails only that utterance. The caller must not transcribe across
 * an -ENOSPC result or reuse the failed epoch. KWS continues independently. */
int k7_voice_session_asr_epoch(struct k7_voice_session *, uint32_t epoch);
int k7_voice_session_take_asr(struct k7_voice_session *, uint32_t epoch,
                              struct k7_voice_frame *);
/* KWS overflow increments input epoch and invalidates old queued frames.
 * -EPIPE requests a model reset. Recheck input_epoch before executing a model
 * event, since overflow may race inference. Never treat old audio as fresh. */
int k7_voice_session_take_kws(struct k7_voice_session *, struct k7_voice_frame *);
uint32_t k7_voice_session_input_epoch(const struct k7_voice_session *);
int k7_voice_session_take_render(struct k7_voice_session *, struct k7_voice_frame *);
/* One dedicated consumer, continuously drained when enabled. 1=record,0=empty.
 * -EPIPE invalidates prior correlation after loss/invalid observations; old
 * queued records are discarded. Check epoch again before committing a derived
 * mapping because overflow can race processing. No wrap/reuse: -EOVERFLOW
 * requires a fresh session if continuity epochs are exhausted. */
int k7_voice_session_take_anchor(struct k7_voice_session *, struct k7_voice_anchor *);
uint32_t k7_voice_session_anchor_epoch(const struct k7_voice_session *);

/* Diagnostic-only FIFO timeline; parent session must remain pinned until the
 * single consumer retires. Exact instance+epoch+generation recheck is required
 * after take and before using a record. END is revoked even on a drained EOF.
 * Off returns 0 records/-ENOTSUP status, allocates no reference buffer. */
int k7_voice_session_take_fifo_reference(struct k7_voice_session *, struct k7_fifo_ref_record *);
/* Alternate SOLE diagnostic drain, never concurrent/mixed with reference
 * take. Old-epoch records are preserved after revoke, but never become clean
 * or eligible again. It does not change PIO, source cursors or audio state. */
int k7_voice_session_take_fifo_archive(struct k7_voice_session *,struct k7_fifo_ref_record *);
int k7_voice_session_fifo_status(const struct k7_voice_session *, struct k7_fifo_ref_status *);
bool k7_voice_session_fifo_matches(const struct k7_voice_session *, uint64_t instance,
                                   uint32_t epoch, uint32_t generation);
bool k7_voice_session_fifo_enabled(const struct k7_voice_session *);
/* PIO-only, non-blocking observations. Never use their result to stop audio. */
void k7_voice_session_fifo_begin(struct k7_voice_session *, uint32_t generation);
void k7_voice_session_fifo_pair(struct k7_voice_session *, uint32_t generation,
  int16_t sample, bool explicit_zero, uint64_t source_index, uint64_t before_us, uint64_t after_us);
void k7_voice_session_fifo_revoke(struct k7_voice_session *, unsigned faults);
void k7_voice_session_fifo_end(struct k7_voice_session *, int result,
                              unsigned discarded_words, bool fifo_empty);

/* Audio-owner-only backend interface. No mutex/network/inference. */
int k7_voice_session_stop_requested(const struct k7_voice_session *);
void k7_voice_session_ready(struct k7_voice_session *, bool ready);
/* Physical wrapper calls once, after ALL audio/codec/clock/bus cleanup. A
 * backend that never reports cleanup remains unverified, even if run returns 0. */
void k7_voice_session_cleanup_complete(struct k7_voice_session *, int error);
int k7_voice_session_microphone(struct k7_voice_session *, int16_t, uint64_t index);
enum k7_voice_tx_action k7_voice_session_tx_action(const struct k7_voice_session *);
int k7_voice_session_tx_started(struct k7_voice_session *);
int k7_voice_session_tx_draining(struct k7_voice_session *);
int k7_voice_session_tx_paused(struct k7_voice_session *);
unsigned k7_voice_session_tx_pause_timeout_ms(const struct k7_voice_session *);
int k7_voice_session_tx_pull(struct k7_voice_session *, int16_t *, uint64_t *index);
int k7_voice_session_render(struct k7_voice_session *, int16_t, uint64_t index);
bool k7_voice_session_anchors_enabled(const struct k7_voice_session *);
uint32_t k7_voice_session_tx_generation(const struct k7_voice_session *);
/* Producer is the PIO owner only. Bounded SPSC publication: no allocation,
 * mutex, logging or wait. Negative result affects diagnostics only, never RX. */
int k7_voice_session_record_anchor(struct k7_voice_session *, const struct k7_voice_anchor *);
void k7_voice_session_tx_complete(struct k7_voice_session *, int result,
                                  unsigned queued_samples, unsigned discarded_words);
#endif
