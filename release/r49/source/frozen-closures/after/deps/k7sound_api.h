#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous bounded helpers for the VoiceLink coordinator. */
int k7sound_capture_grouped(unsigned frames);
int k7sound_play_prompt_tone(void);
/* Immutable interleaved stereo S32 at 16 kHz, at most 30 seconds. Optional
 * capture is armed/settled before speech and starts immediately after playback.
 * The caller retains the buffer until this synchronous operation returns. */
int k7sound_speak_pcm(const uint32_t *pcm, unsigned frames,
                      unsigned capture_frames);
/* Immutable mono S16 at 16 kHz.  Conversion occurs while feeding the FIFO;
 * no system-heap playback buffer is allocated. */
int k7sound_speak_mono16(const int16_t *pcm, unsigned frames,
                         unsigned capture_frames);
/* ECANCELED means playback cancellation and the normal cleanup succeeded. */
int k7sound_speak_mono16_cancel(const int16_t *pcm, unsigned frames,
                               int (*cancelled)(void *), void *arg);

#ifdef __cplusplus
}
#endif
