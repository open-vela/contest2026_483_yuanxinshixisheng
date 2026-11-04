/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_SAI_PIO_V3_H
#define K7_SAI_PIO_V3_H
#include <stdint.h>
#define PIO_FRAMES 48000u
#define PIO_STREAM_FRAMES 160000u
#define PIO_PLAYBACK_FRAMES (16000u * 30u)
#define PIO_MAX_AMPLITUDE (UINT32_C(1)<<27)
/* Called on the capture task for each real stereo frame. Must be nonblocking:
 * no socket, allocation or log calls. Nonzero aborts capture and runs cleanup. */
/* 0 continues, positive ends capture successfully after this sample,
 * negative aborts. All exits retain normal SAI/amp cleanup. */
typedef int (*pio_sample_sink)(void *, uint32_t left, uint32_t right);
/* Nonblocking: 1 supplies one S16 sample, 0 definitive EOF, negative fails.
 * In particular -EAGAIN is an underrun, never silence substitution. */
typedef int (*pio_sample_source)(void *, int16_t *sample);
struct pio_port;
struct pio_result;
int pio_play_stream16(const struct pio_port *,int,uint32_t,
                     pio_sample_source,void *,struct pio_result *);
int pio_run_grouped_sink(const struct pio_port *, int, uint32_t, uint32_t *,
 unsigned, unsigned, struct pio_result *, pio_sample_sink, void *);
struct pio_port {
 void *ctx;
 int (*read)(void *,uint32_t,uint32_t *);
 int (*write)(void *,uint32_t,uint32_t);
 uint64_t (*us)(void *);
 /* Called after CLK/FS start, before either stream or TX prefill. May call
  * kd_arm with bounded I2C/delay and AMP LOW, within100ms, no reentry.
  * Nonzero is failure even if it partially enabled codec/amp. */
 int (*clocks_started)(void *);
 /* Immediately after TXS/RXS, pureGPIO kd_enable_amp, <100us, no I2C/delay. */
 int (*streams_started)(void *);
 /* Always called on post-configuration exit, before stream stop. Idempotent
  * fast amp-off MMIO only (<100us), NO I2C/delay/codec teardown here.
  * Root may do codec mute/I2C after stream stop; failure retains held. */
 int (*amp_off_fast)(void *);
 uint32_t amplitude; /* 1..2^27; applies to playback signed32 tone */
 /* Optional nonblocking playback cancellation. Cleanup still mutes the amp
  * and stops SAI; callback must not perform I/O or take a blocking lock. */
 int (*cancelled)(void *);
 void *cancel_arg;
};
#define PIO_RX_TRACE_MAX 32u
struct pio_rx_trace_entry {
 uint64_t time_us;
 uint32_t fifo_before,word,fifo_after;
 int before_rc,word_rc,after_rc;
};
struct pio_rx_trace {
 /* Sampling window: after stream hook, before cleanup. Zero when not entered.
  * init[] order MONO_CR,RX_SHIFT,RXCR,PATH_SEL,DMACR,XFER; negative rc means
  * invalid value. Snapshots sequential, not atomic; tracing perturbs timing. */
 uint64_t start_us,end_us;
 uint32_t init[6];int init_rc[6];
 unsigned window_started,count;
 struct pio_rx_trace_entry entry[PIO_RX_TRACE_MAX];
};
struct pio_result {
 uint64_t stream_start_us; /* TX/RX enabled and amplifier hook completed; not acoustic onset. */
 uint64_t pause_start_us,pause_end_us;
 uint32_t pause_before,pause_after;
 unsigned frames,polls,prefill_words,max_fifo;
 uint32_t first_fifo_raw,last_fifo_raw;
 int result,stop_result,held,activation_result,start_result,amp_off_result;
 int dma_restore_result,mono_restore_result;
 unsigned grouped_words,grouped_mismatches;
 struct pio_rx_trace rx_trace;
};
struct pio_stats {int64_t sum[2];int32_t min[2],max[2];unsigned nonzero[2];};
int pio_summarize(const uint32_t *,unsigned,struct pio_stats *);
/* Sum four6-bit fields as vendor get_fifo_count; never infer physical bank
 * distribution or sample format from one field. Reserved high bits ignored. */
unsigned pio_fifo_count(uint32_t);
/* V1 platform prerequisites retained: exclusive, codec raw32, actualMCLK,
 * IRQ/DMA off, MMIO ordering, nonblocking read/write/time, VERSION23073576.
 * Exactly16 prefill words with TX stopped; sum4 readback must be16. During TX
 * enqueue pair only at count<=14; never knowingly exceed16. Observed FIFO
 * occupancy total is recorded; first/last raw registers preserve all fields.
 * For RX first raw is first loop observation (may be zero); TX is prefill.
 * Full physical depth >=16 inferred from vendor
 * threshold16/maxburst8, still requires actual FIFO evidence on this board.
 * Capture capacity>=2*frames, frames1..48000 (384000bytes maximum).
 * Playback frames8..3200 only; first root probe uses3200. Transfer deadline
 * frames/16000 seconds +100ms begins after hook; polls limited4096/frame.
 * Callback deadline checked after return (cannot interrupt blocked callback).
 * amplitude bound is digital only; root must enforce safe codec/amp gain.
 * Result success requires transfer+ampoff+SAI stop; codec teardown afterward.
 */
int pio_run(const struct pio_port *,int prepared,uint32_t actual_mclk,
 int playback,uint32_t *capture,unsigned capacity_words,unsigned frames,
 struct pio_result *);
/* Explicit S16 stereo-slot capture experiment. RXDR words remain 32-bit MMIO
 * containers; SJM_L places each signed 16-bit sample in the high half. */
int pio_run16(const struct pio_port *,int prepared,uint32_t actual_mclk,
 uint32_t *capture,unsigned capacity_words,unsigned frames,struct pio_result *);
/* Diagnostic capture matching the vendor Linux start/stop DMA-request gate:
 * RDL=16 and RDE=1 while RX runs, then restore the exact prior DMACR fields.
 * CPU still reads RXDR; this tests whether the request gate changes FIFO
 * selection and is not a claim that DMA completed any transfer. */
int pio_run_rde(const struct pio_port *,int prepared,uint32_t actual_mclk,
 uint32_t *capture,unsigned capacity_words,unsigned frames,struct pio_result *);
/* Two bounded RX routing diagnostics. Both use documented CSR=4. The first
 * retains the platform's lane1..3 routes; the second maps every active lane
 * to SDI0. No zero suppression or resampling is performed. */
int pio_run_rx4(const struct pio_port *,int prepared,uint32_t actual_mclk,
 uint32_t *capture,unsigned capacity_words,unsigned frames,struct pio_result *);
int pio_run_rx4_all(const struct pio_port *,int prepared,uint32_t actual_mclk,
 uint32_t *capture,unsigned capacity_words,unsigned frames,struct pio_result *);
/* Documented receive-mono diagnostic: select slot0 on SDI0, read exactly one
 * RXDR word per 16kHz frame, and mirror that word into the interleaved output
 * buffer solely to preserve the existing consumer ABI. MONO_CR is restored
 * after a successful stream stop; failure retains held state. */
int pio_run_mono(const struct pio_port *,int prepared,uint32_t actual_mclk,
 uint32_t *capture,unsigned capacity_words,unsigned frames,struct pio_result *);
/* RK3576 RXDR advances across four FIFO banks and two slot positions. This
 * bounded candidate maps all four receive paths to SDI0, waits until every
 * bank owns an entry, and reads two complete eight-word hardware cycles. The
 * first selected word is the left slot and the second is the right slot, which
 * together form one 16kHz stereo frame. grouped_mismatches reports disagreement
 * among the four SDI0 copies; callers must retain it as an acceptance gate. */
int pio_run_grouped(const struct pio_port *,int prepared,uint32_t actual_mclk,
 uint32_t *capture,unsigned capacity_words,unsigned frames,struct pio_result *);
/* Read-only rawPCM32 interleaved L,R buffer, frames1..48000; words>=2*frames.
 * Caller owns immutable storage, nonoverlapping result/port, for full call.
 * Exact words sent, no synthesis/zero padding/gain/format conversion; caller
 * must pass only actual captured frames, not unused capacity. amplitude field
 * does NOT attenuate this API. Root controls safe codec/amp gain. Hardware
 * cannot prove source provenance; caller must retain real capture evidence.
 * Short inputs prefill min(16,2*frames), exact FIFO sum must match that count.
 * Existing tone pio_run API/limits and all terminal logic remain unchanged. */
int pio_play_buffer(const struct pio_port *,int prepared,uint32_t actual_mclk,
 const uint32_t *source,unsigned source_words,unsigned frames,struct pio_result *);
/* Immutable mono S16 source, expanded to identical signed S32 stereo words as
 * each frame enters the FIFO.  This avoids a large temporary playback buffer. */
int pio_play_mono16(const struct pio_port *,int prepared,uint32_t actual_mclk,
 const int16_t *source,unsigned source_frames,unsigned frames,struct pio_result *);
/* Opt-in 20us bounded wait after DIV, immediately before CLK/FS enable. */
int pio_run_clockwait(const struct pio_port *,int,uint32_t,int,
 uint32_t *,unsigned,unsigned,struct pio_result *);
/* Diagnostic only: after eight pairs, observe 250us with no RXDR accesses.
 * Fixed 128 pairs; result must not be used as an ordinary audio recording. */
int pio_run_pause250(const struct pio_port *,int,uint32_t,
 uint32_t *,unsigned,struct pio_result *);
#endif
