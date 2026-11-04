#include "cloud_speech_orchestrator.h"

#include <limits.h>
#include <string.h>

struct wav_view {
  const unsigned char *pcm;
  size_t pcm_bytes;
  unsigned rate;
};

static uint16_t le16(const unsigned char *p) {
  return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static int wav_pcm16_mono(const unsigned char *wav, size_t bytes,
                          struct wav_view *view) {
  size_t at = 12;
  bool have_fmt = false, have_data = false;
  struct wav_view parsed = {0};
  if (!wav || !view || bytes < 44 || memcmp(wav, "RIFF", 4) ||
      memcmp(wav + 8, "WAVE", 4) || (uint64_t)le32(wav + 4) + 8 != bytes)
    return CLOUD_SPEECH_EAUDIO;
  while (at <= bytes - 8) {
    uint32_t length = le32(wav + at + 4);
    size_t padded = (size_t)length + (length & 1u);
    if (padded > bytes - at - 8) return CLOUD_SPEECH_EAUDIO;
    if (!memcmp(wav + at, "fmt ", 4)) {
      const unsigned char *f = wav + at + 8;
      if (have_fmt || length < 16 || le16(f) != 1 || le16(f + 2) != 1 ||
          (le32(f + 4) != 16000u && le32(f + 4) != 24000u) ||
          le32(f + 8) != le32(f + 4) * 2u || le16(f + 12) != 2 ||
          le16(f + 14) != 16) return CLOUD_SPEECH_EAUDIO;
      parsed.rate = le32(f + 4);
      have_fmt = true;
    } else if (!memcmp(wav + at, "data", 4)) {
      if (have_data || (length & 1u)) return CLOUD_SPEECH_EAUDIO;
      parsed.pcm = wav + at + 8;
      parsed.pcm_bytes = length;
      have_data = true;
    }
    at += 8 + padded;
  }
  if (at != bytes || !have_fmt || !have_data || !parsed.pcm_bytes)
    return CLOUD_SPEECH_EAUDIO;
  *view = parsed;
  return 0;
}

static int make_wav16(const struct cloud_speech_audio *audio,
                      unsigned char *out, size_t capacity,
                      const unsigned char **wav, size_t *wav_bytes) {
  struct wav_view view;
  if (!audio || !audio->data || !audio->bytes || !wav || !wav_bytes)
    return CLOUD_SPEECH_EINVAL;
  if (audio->format == CLOUD_SPEECH_AUDIO_WAV_PCM16) {
    int rc = wav_pcm16_mono(audio->data, audio->bytes, &view);
    if (rc || view.rate != CLOUD_SPEECH_RATE_HZ) return CLOUD_SPEECH_EAUDIO;
    *wav = audio->data; *wav_bytes = audio->bytes;
    return 0;
  }
  if (audio->format != CLOUD_SPEECH_AUDIO_PCM16LE ||
      audio->sample_rate_hz != CLOUD_SPEECH_RATE_HZ ||
      audio->channels != 1 || audio->bits_per_sample != 16 ||
      (audio->bytes & 1u)) return CLOUD_SPEECH_EAUDIO;
  if (audio->bytes > UINT32_MAX - 36u || audio->bytes > capacity - 44u)
    return CLOUD_SPEECH_ELIMIT;
  memcpy(out, "RIFF", 4); put32(out + 4, (uint32_t)audio->bytes + 36u);
  memcpy(out + 8, "WAVEfmt ", 8); put32(out + 16, 16);
  put16(out + 20, 1); put16(out + 22, 1);
  put32(out + 24, CLOUD_SPEECH_RATE_HZ);
  put32(out + 28, CLOUD_SPEECH_RATE_HZ * 2u);
  put16(out + 32, 2); put16(out + 34, 16);
  memcpy(out + 36, "data", 4); put32(out + 40, (uint32_t)audio->bytes);
  memcpy(out + 44, audio->data, audio->bytes);
  *wav = out; *wav_bytes = audio->bytes + 44u;
  return 0;
}

static int decode_to_16k(const unsigned char *wav, size_t wav_bytes,
                         int16_t *out, size_t capacity, size_t *frames) {
  struct wav_view v;
  int rc = wav_pcm16_mono(wav, wav_bytes, &v);
  if (rc) return rc;
  size_t in_frames = v.pcm_bytes / 2u;
  if (v.rate == 16000u) {
    if (in_frames > capacity) return CLOUD_SPEECH_ELIMIT;
    for (size_t i = 0; i < in_frames; ++i)
      out[i] = (int16_t)le16(v.pcm + i * 2u);
    *frames = in_frames;
    return 0;
  }
  /* MiMo currently returns 24 kHz mono S16 WAV. 3 input frames become 2
   * output frames; the midpoint interpolation keeps the output clock exact.
   * This is a bounded batch converter, not a streaming resampler. */
  size_t n = (in_frames / 3u) * 2u;
  if (!n || n > capacity) return CLOUD_SPEECH_ELIMIT;
  for (size_t o = 0; o < n; o += 2) {
    size_t i = (o / 2u) * 3u;
    int32_t a = (int16_t)le16(v.pcm + i * 2u);
    int32_t b = (int16_t)le16(v.pcm + (i + 1u) * 2u);
    int32_t c = (int16_t)le16(v.pcm + (i + 2u) * 2u);
    out[o] = (int16_t)a;
    out[o + 1u] = (int16_t)((b + c) / 2);
  }
  *frames = n;
  return 0;
}

int cloud_speech_init(struct cloud_speech_orchestrator *o,
                      const struct cloud_speech_ops *ops, void *arg,
                      unsigned char *wav_scratch, size_t wav_capacity,
                      int16_t *pcm_scratch, size_t pcm_frames) {
  if (!o || !ops || !ops->play_fixed_prompt || !ops->cloud_asr_wav ||
      !ops->cloud_tts_wav || !ops->play_pcm16_16k || !wav_scratch ||
      wav_capacity < 46 || !pcm_scratch || !pcm_frames)
    return CLOUD_SPEECH_EINVAL;
  memset(o, 0, sizeof(*o));
  o->ops = *ops; o->ops_arg = arg;
  o->wav_scratch = wav_scratch; o->wav_scratch_capacity = wav_capacity;
  o->pcm_scratch = pcm_scratch; o->pcm_scratch_frames = pcm_frames;
  return 0;
}

int cloud_speech_set_network(struct cloud_speech_orchestrator *o,
                             bool wifi, bool ipv4) {
  if (!o || (ipv4 && !wifi)) return CLOUD_SPEECH_EINVAL;
  if (!wifi) {
    /* Every DNS/TLS/API observation belongs to the old link generation. */
    o->readiness = 0;
    o->connected_prompt_played = false;
    return 0;
  }
  if (!ipv4) {
    /* Association without a lease cannot retain cloud readiness. */
    o->readiness = CLOUD_SPEECH_WIFI_CONNECTED;
    o->connected_prompt_played = false;
    return 0;
  }
  o->readiness |= CLOUD_SPEECH_WIFI_CONNECTED | CLOUD_SPEECH_IPV4_READY;
  if (ipv4 && !o->connected_prompt_played) {
    if (o->ops.play_fixed_prompt(o->ops_arg,
        CLOUD_SPEECH_PROMPT_NETWORK_CONNECTED)) return CLOUD_SPEECH_EPLAY;
    o->connected_prompt_played = true;
  }
  return 0;
}

void cloud_speech_set_platform_readiness(struct cloud_speech_orchestrator *o,
                                         uint32_t bits) {
  const uint32_t network = CLOUD_SPEECH_WIFI_CONNECTED |
                           CLOUD_SPEECH_IPV4_READY;
  if (!o) return;
  if ((o->readiness & network) != network) {
    o->readiness &= network;
    return;
  }
  o->readiness = (o->readiness & network) | (bits & ~network);
}

bool cloud_speech_is_ready(const struct cloud_speech_orchestrator *o) {
  return o && o->connected_prompt_played &&
         (o->readiness & CLOUD_SPEECH_REQUIRED_BITS) ==
         CLOUD_SPEECH_REQUIRED_BITS;
}

static bool request_cancelled(void *arg) {
  return !cloud_speech_is_ready((const struct cloud_speech_orchestrator *)arg);
}

int cloud_speech_transcribe(struct cloud_speech_orchestrator *o,
                            const char *token,
                            const struct cloud_speech_audio *audio,
                            char *text, size_t text_capacity,
                            size_t *text_bytes) {
  const unsigned char *wav = NULL;
  size_t wav_bytes = 0;
  int rc;
  if (text_bytes) *text_bytes = 0;
  if (!o || !token || !*token || !text || !text_capacity || !text_bytes)
    return CLOUD_SPEECH_EINVAL;
  if (!cloud_speech_is_ready(o)) return CLOUD_SPEECH_ENOTREADY;
  if (o->busy) return CLOUD_SPEECH_EBUSY;
  o->busy = true;
  rc = make_wav16(audio, o->wav_scratch, o->wav_scratch_capacity,
                  &wav, &wav_bytes);
  if (!rc && o->ops.cloud_asr_wav(o->ops_arg, token, wav, wav_bytes,
                                  text, text_capacity, text_bytes,
                                  request_cancelled, o))
    rc = CLOUD_SPEECH_ECLOUD;
  if (!rc && request_cancelled(o)) rc = CLOUD_SPEECH_ENOTREADY;
  if (!rc && (*text_bytes >= text_capacity || text[*text_bytes] != '\0'))
    rc = CLOUD_SPEECH_ECLOUD;
  if (rc) { text[0] = '\0'; *text_bytes = 0; }
  o->busy = false;
  return rc;
}

int cloud_speech_synthesize_and_play(struct cloud_speech_orchestrator *o,
                                     const char *token, const char *text,
                                     const char *voice) {
  size_t wav_bytes = 0, frames = 0;
  int rc = 0;
  if (!o || !token || !*token || !text || !*text || !voice || !*voice)
    return CLOUD_SPEECH_EINVAL;
  if (!cloud_speech_is_ready(o)) return CLOUD_SPEECH_ENOTREADY;
  if (o->busy) return CLOUD_SPEECH_EBUSY;
  o->busy = true;
  if (o->ops.cloud_tts_wav(o->ops_arg, token, text, voice,
                           o->wav_scratch, o->wav_scratch_capacity,
                           &wav_bytes, request_cancelled, o))
    rc = CLOUD_SPEECH_ECLOUD;
  if (!rc && (wav_bytes > o->wav_scratch_capacity || request_cancelled(o)))
    rc = request_cancelled(o) ? CLOUD_SPEECH_ENOTREADY : CLOUD_SPEECH_ECLOUD;
  if (!rc) rc = decode_to_16k(o->wav_scratch, wav_bytes, o->pcm_scratch,
                              o->pcm_scratch_frames, &frames);
  if (!rc && o->ops.play_pcm16_16k(o->ops_arg, o->pcm_scratch, frames))
    rc = CLOUD_SPEECH_EPLAY;
  o->busy = false;
  return rc;
}

const char *cloud_speech_strerror(int error) {
  switch (error) {
    case 0: return "ok";
    case CLOUD_SPEECH_EINVAL: return "invalid argument";
    case CLOUD_SPEECH_ENOTREADY: return "cloud speech is not ready";
    case CLOUD_SPEECH_EBUSY: return "speech operation is busy";
    case CLOUD_SPEECH_ELIMIT: return "audio buffer limit exceeded";
    case CLOUD_SPEECH_EAUDIO: return "unsupported audio format";
    case CLOUD_SPEECH_ECLOUD: return "cloud request failed";
    case CLOUD_SPEECH_EPLAY: return "audio playback failed";
    default: return "unknown cloud speech error";
  }
}
