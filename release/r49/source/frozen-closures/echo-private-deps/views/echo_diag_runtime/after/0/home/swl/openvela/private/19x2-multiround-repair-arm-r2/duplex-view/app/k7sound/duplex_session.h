/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_DUPLEX_SESSION_H
#define K7_DUPLEX_SESSION_H
#include "pio.h"
#include "voice_session.h"
struct k7_duplex_session_result {
  uint64_t captured, queued, turns, cancelled_turns, failed_turns;
  unsigned grouped_mismatches, rx_overflows, tx_underflows, max_rx_fifo, max_tx_fifo;
  int result, stop_result, amp_off_result, held;
  int codec_stop, platform_restore, i2c_restore;
  uint64_t tx_pauses, tx_resumes;
  /* Explicit serial gap filler, excluded from queued/render/source counts. */
  uint64_t tx_silence_samples, tx_discarded_silence_words;
  unsigned tx_provenance_errors;
};
/* No finite conversation-duration cutoff. stop_requested ends the lifetime;
 * 100ms RX progress and 1ms TX stop/clear guards still bound stuck hardware.
 * clocks_started arms both converters once. streams_started is a repeatable
 * GPIO-only amp-enable callback (not kd_arm or the one-shot kd_enable_amp).
 * TX failures are reported per generation; verified TX cleanup keeps RX live.
 * RX errors/unknown cleanup end the session with a latched fault. */
/* Streaming refills keep TXS and the amplifier enabled within one generation,
 * feeding explicit zero samples while waiting. These are not source/render
 * samples; tx_silence_samples counts them. Pause/resume anchors invalidate
 * source-to-acoustic time mapping across each gap. Normal
 * stream EOF keeps the analog path for a nonblocking 30ms after TX_IDLE/clear;
 * this conservative guard is pending physical validation, not a codec spec.
 * Cancellation/failure/session stop bypass the guard and mute immediately.
 * First TX still enables the amp after TXS; startup audibility is unverified. */
int pio_duplex_session(const struct pio_port *, int prepared, uint32_t mclk,
                        struct k7_voice_session *, struct k7_duplex_session_result *);
/* Explicit experimental service: creates one owner worker and returns before
 * ADC setup completes. Poll ready/finished. Caller owns all consumer workers
 * and must stop/join them before destroying the session. No KWS/AEC is started
 * by this API, and ASR is initially disabled. */
int k7sound_session_start(const struct k7_voice_session_config *, struct k7_voice_session **);
#endif
