/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_SESSION_CAPTURE_H
#define K7_SESSION_CAPTURE_H
#include <stdint.h>
#include <stdbool.h>
struct k7_voice_session;
/* Opt-in diagnostic only. An armed capture selects one eligible real-cloud
 * TTS generation. No-op when unarmed; diagnostics never change audio results.
 * begin: 1 selected, 0 not armed, negative diagnostic admission error.
 * The source producer calls end exactly once after fetch and finish_tx return.
 * end is nonblocking: the audio owner acknowledges freeze at its next RX
 * callback, or owner_exit acknowledges after all its diagnostic writes stop.
 * No dump/clear is permitted before both producer and owner are quiescent. */
int k7_session_capture_begin(struct k7_voice_session *, uint32_t generation);
void k7_session_capture_source(struct k7_voice_session *, uint32_t generation,
                               const int16_t *, unsigned count, uint32_t offset);
void k7_session_capture_end(struct k7_voice_session *, uint32_t generation,
                            int fetch_result, int stream_result);
/* Audio-owner only; bounded stores/CRC, no allocation, I/O or mutex waits.
 * FIFO records describe software enqueue, never DAC/acoustic timestamps. */
void k7_session_capture_microphone(struct k7_voice_session *, int16_t, uint64_t index);
void k7_session_capture_fifo_source(struct k7_voice_session *, int16_t, uint64_t index);
void k7_session_capture_fifo_zero(struct k7_voice_session *);
/* Post-pump only, after ready(false). May acquire the diagnostic control lock
 * to serialize admission; never call this from the live RX/TX loop. */
void k7_session_capture_owner_exit(struct k7_voice_session *, int result);
void k7_session_capture_owner_ready(struct k7_voice_session *, bool ready);
void k7_session_capture_tx_busy(struct k7_voice_session *, bool busy);
/* Explicit command: arm/status/clear/dump source|mic <offset> <count>. */
int k7_session_capture_command(int argc, char **argv);
#endif
