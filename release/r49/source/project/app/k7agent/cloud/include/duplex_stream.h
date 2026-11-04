/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_DUPLEX_STREAM_H
#define K7_DUPLEX_STREAM_H
#include "pio.h"
#define K7_DUPLEX_MAX_CAPTURE_FRAMES (16000u * 30u)
/* All callbacks execute in the single audio-owner thread and must not block,
 * allocate, log, call the network or run inference. 0=accepted, negative=error.
 * sample_index is per-direction enqueue/capture order, NOT acoustic time. */
typedef int (*k7_duplex_sample_sink)(void *, int16_t sample, uint64_t sample_index);
struct k7_duplex_stream_config {
  unsigned max_capture_frames; /* 320..480000, multiple of 320; bounded probe */
  k7_duplex_sample_sink microphone;
  void *microphone_arg;
  pio_sample_source playback;  /* 1=sample; 0=EOF; -EAGAIN is a real underrun */
  void *playback_arg;
  k7_duplex_sample_sink render; /* exact samples queued to TX, optional */
  void *render_arg;
  int (*stop_all)(void *);     /* mandatory; atomic/nonblocking */
  void *stop_arg;
  int (*cancel_tx)(void *);    /* optional; only TX stops, RX stays enabled */
  void *cancel_tx_arg;
};
struct k7_duplex_stream_result {
  uint64_t start_us, end_us, tx_stop_us;
  unsigned captured, tx_queued, tx_discarded_words, rx_after_tx_stop;
  unsigned grouped_words, grouped_mismatches, max_rx_fifo, max_tx_fifo;
  unsigned rx_overflows, tx_underflows, polls;
  int tx_cancelled, tx_eof, result, stop_result, amp_off_result, held;
  int codec_stop, platform_restore, i2c_restore;
};
/* Shared clocks, TX lane1 and the validated grouped RX4/SDI0 route. One
 * exclusive owner; both ADC and DAC must be armed by clocks_started.
 * TX cancel/EOF stops TX only. Capture continues until its bound/stop_all.
 * A cancelled TX tail is reported, never counted as acoustically played.
 * No AEC implementation or board-level duplex acceptance is implied. */
int pio_duplex_stream16(const struct pio_port *, int prepared, uint32_t mclk,
                        const struct k7_duplex_stream_config *,
                        struct k7_duplex_stream_result *);
/* Available only when CONFIG_EXAMPLES_K7SOUND_DUPLEX is enabled. Runs on the
 * caller thread; caller pins it to CPU1 and joins it before freeing callbacks. */
int k7sound_duplex_stream16(const struct k7_duplex_stream_config *,
                            struct k7_duplex_stream_result *);
#endif
