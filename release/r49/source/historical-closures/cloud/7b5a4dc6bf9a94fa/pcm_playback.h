#ifndef K7_PCM_PLAYBACK_H
#define K7_PCM_PLAYBACK_H
#include <stdint.h>
/* Caller prebuffers PCM and runs this blocking owner on CPU1. pull must never
 * block, allocate, or use the network: 1=sample, 0=definitive EOF, <0=error.
 * -EAGAIN is fatal underrun; never inserts silence. Up to 30s at 16kHz.
 * Success requires EOF, drain and normal cleanup; all errors/cancel mute.
 * Callback state must remain alive until return; caller joins producer too. */
typedef int (*k7_pcm16_pull)(void *arg,int16_t *sample);
int k7sound_speak_stream16(k7_pcm16_pull pull,void *arg,
                         int (*cancelled)(void *),void *cancel_arg);
#endif
