#ifndef CLOUD_SPEECH_ORCHESTRATOR_H
#define CLOUD_SPEECH_ORCHESTRATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The cloud path stays closed until every transport prerequisite is true.
 * Credentials are borrowed per request and are never retained here. */
enum cloud_speech_ready_bit {
  CLOUD_SPEECH_WIFI_CONNECTED = 1u << 0,
  CLOUD_SPEECH_IPV4_READY      = 1u << 1,
  CLOUD_SPEECH_DNS_READY       = 1u << 2,
  CLOUD_SPEECH_ENTROPY_READY   = 1u << 3,
  CLOUD_SPEECH_TIME_READY      = 1u << 4,
  CLOUD_SPEECH_CA_READY        = 1u << 5,
  CLOUD_SPEECH_TCP_READY       = 1u << 6,
  CLOUD_SPEECH_API_READY       = 1u << 7
};

#define CLOUD_SPEECH_REQUIRED_BITS ((uint32_t)0xffu)
#define CLOUD_SPEECH_RATE_HZ 16000u

enum cloud_speech_prompt {
  CLOUD_SPEECH_PROMPT_NETWORK_CONNECTED = 1
};

enum cloud_speech_audio_format {
  CLOUD_SPEECH_AUDIO_PCM16LE = 1,
  CLOUD_SPEECH_AUDIO_WAV_PCM16 = 2
};

enum cloud_speech_error {
  CLOUD_SPEECH_OK = 0,
  CLOUD_SPEECH_EINVAL = -3000,
  CLOUD_SPEECH_ENOTREADY,
  CLOUD_SPEECH_EBUSY,
  CLOUD_SPEECH_ELIMIT,
  CLOUD_SPEECH_EAUDIO,
  CLOUD_SPEECH_ECLOUD,
  CLOUD_SPEECH_EPLAY
};

struct cloud_speech_audio {
  enum cloud_speech_audio_format format;
  const unsigned char *data;
  size_t bytes;
  unsigned sample_rate_hz;
  unsigned channels;
  unsigned bits_per_sample;
};

struct cloud_speech_ops {
  int (*play_fixed_prompt)(void *arg, enum cloud_speech_prompt prompt);
  int (*cloud_asr_wav)(void *arg, const char *token,
                       const unsigned char *wav, size_t wav_bytes,
                       char *text, size_t text_capacity, size_t *text_bytes,
                       bool (*cancelled)(void *), void *cancel_arg);
  int (*cloud_tts_wav)(void *arg, const char *token, const char *text,
                       const char *voice, unsigned char *wav,
                       size_t wav_capacity, size_t *wav_bytes,
                       bool (*cancelled)(void *), void *cancel_arg);
  int (*play_pcm16_16k)(void *arg, const int16_t *pcm, size_t frames);
};

struct cloud_speech_orchestrator {
  struct cloud_speech_ops ops;
  void *ops_arg;
  uint32_t readiness;
  bool connected_prompt_played;
  bool busy;
  unsigned char *wav_scratch;
  size_t wav_scratch_capacity;
  int16_t *pcm_scratch;
  size_t pcm_scratch_frames;
};

/* The orchestrator is owned by one cloud worker task and is not internally
 * thread-safe. Radio/link events must be marshalled to that owner. If a link
 * is lost during an HTTP operation, the transport owner cancels the socket,
 * waits for the call to return, then applies cloud_speech_set_network(false,
 * false); the next link generation must re-run every platform gate. */

int cloud_speech_init(struct cloud_speech_orchestrator *orchestrator,
                      const struct cloud_speech_ops *ops, void *ops_arg,
                      unsigned char *wav_scratch,
                      size_t wav_scratch_capacity,
                      int16_t *pcm_scratch, size_t pcm_scratch_frames);

/* Feed the real Wi-Fi/DHCP state reported by the radio path.  A successful
 * connected transition plays the built-in "network connected" asset without
 * touching the cloud. Link loss closes the cloud gate immediately. */
int cloud_speech_set_network(struct cloud_speech_orchestrator *orchestrator,
                             bool wifi_connected, bool ipv4_ready);

/* Set only non-network readiness bits after their live checks pass. */
void cloud_speech_set_platform_readiness(
  struct cloud_speech_orchestrator *orchestrator, uint32_t readiness_bits);

bool cloud_speech_is_ready(const struct cloud_speech_orchestrator *orchestrator);

/* Current MiMo profile is batch HTTP. These calls do not claim streaming. */
int cloud_speech_transcribe(struct cloud_speech_orchestrator *orchestrator,
                            const char *token,
                            const struct cloud_speech_audio *audio,
                            char *text, size_t text_capacity,
                            size_t *text_bytes);
int cloud_speech_synthesize_and_play(
  struct cloud_speech_orchestrator *orchestrator, const char *token,
  const char *text, const char *voice);

const char *cloud_speech_strerror(int error);

#ifdef __cplusplus
}
#endif
#endif
