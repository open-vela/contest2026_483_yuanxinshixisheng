#include "cloud_speech_mimo_adapter.h"

#include "k7sound_api.h"

#include <limits.h>

static int fixed_prompt(void *arg, enum cloud_speech_prompt prompt) {
  struct cloud_speech_mimo_adapter *a = arg;
  if (!a || prompt != CLOUD_SPEECH_PROMPT_NETWORK_CONNECTED ||
      !a->network_connected_pcm16 || !a->network_connected_frames)
    return -1;
  if (a->network_connected_frames > UINT_MAX) return -1;
  return k7sound_speak_mono16(a->network_connected_pcm16,
                              (unsigned)a->network_connected_frames, 0);
}

static int asr(void *arg, const char *token, const unsigned char *wav,
               size_t wav_bytes, char *text, size_t text_capacity,
               size_t *text_bytes, bool (*cancelled)(void *),
               void *cancel_arg) {
  struct cloud_speech_mimo_adapter *a = arg;
  struct mimo_asr_request request = {token, wav, wav_bytes, "wav"};
  const struct vv_https_cancel cancel = {cancelled, cancel_arg};
  if (!a || !a->mimo) return -1;
  return mimo_cloud_transcribe(a->mimo, &request, text, text_capacity,
                               text_bytes, &cancel);
}

static int tts(void *arg, const char *token, const char *text,
               const char *voice, unsigned char *wav, size_t capacity,
               size_t *wav_bytes, bool (*cancelled)(void *),
               void *cancel_arg) {
  struct cloud_speech_mimo_adapter *a = arg;
  struct mimo_tts_request request = {token, text, voice};
  const struct vv_https_cancel cancel = {cancelled, cancel_arg};
  if (!a || !a->mimo) return -1;
  return mimo_cloud_synthesize(a->mimo, &request, wav, capacity, wav_bytes,
                               &cancel);
}

static int play(void *arg, const int16_t *pcm, size_t frames) {
  (void)arg;
  if (frames > 16000u * 30u) return -1;
  return k7sound_speak_mono16(pcm, (unsigned)frames, 0);
}

int cloud_speech_mimo_adapter_ops(struct cloud_speech_ops *ops) {
  if (!ops) return CLOUD_SPEECH_EINVAL;
  *ops = (struct cloud_speech_ops){fixed_prompt, asr, tts, play};
  return 0;
}
