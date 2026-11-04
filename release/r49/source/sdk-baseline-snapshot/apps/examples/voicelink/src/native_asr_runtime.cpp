#include "voicelink/asr_runtime.h"
#include "sherpa-onnx/c-api/c-api.h"
#include "test_assets.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <pthread.h>
#include <unistd.h>

extern "C" int k7_tls_selftest(void);
extern "C" int k7sound_copy_mono(float *, unsigned, unsigned *);

namespace {

constexpr unsigned kSampleRate = 16000;
constexpr unsigned kMaxFrames = 48000;
constexpr uint64_t kInternalBudgetMs = 180000;

struct Handles {
  const SherpaOnnxOnlineRecognizer *recognizer = nullptr;
  const SherpaOnnxOnlineStream *stream = nullptr;
  const SherpaOnnxOnlineRecognizerResult *result = nullptr;

  ~Handles() {
    if (result) SherpaOnnxDestroyOnlineRecognizerResult(result);
    if (stream) SherpaOnnxDestroyOnlineStream(stream);
    if (recognizer) SherpaOnnxDestroyOnlineRecognizer(recognizer);
  }
};

int monotonic_ms(uint64_t &out) noexcept {
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC, &value) || value.tv_sec < 0) return -EIO;
  out = uint64_t(value.tv_sec) * 1000 + uint64_t(value.tv_nsec) / 1000000;
  return 0;
}

pthread_mutex_t asr_gate = PTHREAD_MUTEX_INITIALIZER;

int stop_error(const k7_asr_audio_request &request, uint64_t internal_deadline) {
  if (request.cancel_requested && request.cancel_requested(request.cancel_ctx))
    return -ECANCELED;
  uint64_t now = 0;
  const int clock_error = monotonic_ms(now);
  if (clock_error) return clock_error;
  if ((request.deadline_ms && now >= request.deadline_ms) ||
      now >= internal_deadline)
    return -ETIMEDOUT;
  return 0;
}

int copy_text(const char *source, char *destination, size_t capacity) {
  if (!destination || !capacity) return -EINVAL;
  destination[0] = '\0';
  if (!source || !source[0]) return -ENODATA;
  size_t length = 0;
  while (length < capacity && source[length]) ++length;
  if (length == capacity) return -ENOSPC;
  std::memcpy(destination, source, length + 1);
  return 0;
}

int recognize_audio(const k7_asr_audio_request &request, char *utf8_text,
                    size_t capacity) {
  uint64_t start = 0;
  int error = monotonic_ms(start);
  if (error) return error;
  const uint64_t internal_deadline = start + kInternalBudgetMs;
  error = stop_error(request, internal_deadline);
  if (error) return error;

  const int lock_error = pthread_mutex_trylock(&asr_gate);
  if (lock_error) return -lock_error;
  struct Gate {
    ~Gate() { pthread_mutex_unlock(&asr_gate); }
  } gate;

  try {
    Handles handles;
    if ((error = stop_error(request, internal_deadline))) return error;
    if (k7_tls_selftest()) return -EIO;
    if ((error = stop_error(request, internal_deadline))) return error;

    SherpaOnnxOnlineRecognizerConfig config{};
    config.feat_config.sample_rate = kSampleRate;
    config.feat_config.feature_dim = 80;
    config.model_config.paraformer.encoder = "k7ram:encoder";
    config.model_config.paraformer.decoder = "k7ram:decoder";
    config.model_config.tokens_buf = reinterpret_cast<const char *>(test_tokens);
    config.model_config.tokens_buf_size = sizeof(test_tokens) - 1;
    config.model_config.num_threads = 1;
    config.model_config.provider = "cpu";
    config.model_config.model_type = "paraformer";
    config.decoding_method = "greedy_search";

    std::puts("ASR_INFER BEGIN recognizer");
    std::fflush(stdout);
    handles.recognizer = SherpaOnnxCreateOnlineRecognizer(&config);
    if (!handles.recognizer) return -EIO;
    if ((error = stop_error(request, internal_deadline))) return error;
    handles.stream = SherpaOnnxCreateOnlineStream(handles.recognizer);
    if (!handles.stream) return -EIO;

    unsigned steps = 0;
    auto decode = [&]() -> int {
      while (SherpaOnnxIsOnlineStreamReady(handles.recognizer, handles.stream)) {
        const int stopped = stop_error(request, internal_deadline);
        if (stopped) return stopped;
        if (++steps > 128) return -ETIMEDOUT;
        SherpaOnnxDecodeOnlineStream(handles.recognizer, handles.stream);
        uint64_t now = 0;
        if (monotonic_ms(now)) return -EIO;
        std::printf("ASR_INFER chunk=%u elapsed=%.3f\n", steps,
                    double(now - start) / 1000.0);
        std::fflush(stdout);
        const int after = stop_error(request, internal_deadline);
        if (after) return after;
        usleep(1000);
      }
      return 0;
    };

    for (unsigned offset = 0; offset < request.frames;) {
      if ((error = stop_error(request, internal_deadline))) return error;
      unsigned count = request.frames - offset;
      if (count > 1600) count = 1600;
      SherpaOnnxOnlineStreamAcceptWaveform(handles.stream,
                                           request.sample_rate_hz,
                                           request.samples + offset, count);
      offset += count;
      if ((error = decode())) return error;
    }

    float silence[1600]{};
    for (unsigned index = 0; index < 3; ++index) {
      if ((error = stop_error(request, internal_deadline))) return error;
      SherpaOnnxOnlineStreamAcceptWaveform(handles.stream, kSampleRate,
                                           silence, 1600);
    }
    SherpaOnnxOnlineStreamInputFinished(handles.stream);
    if ((error = decode())) return error;
    if ((error = stop_error(request, internal_deadline))) return error;
    handles.result =
        SherpaOnnxGetOnlineStreamResult(handles.recognizer, handles.stream);
    if (!handles.result || !handles.result->text || !handles.result->text[0])
      return -ENODATA;
    const int copied = copy_text(handles.result->text, utf8_text, capacity);
    if (copied) return copied;
    uint64_t now = 0;
    if (monotonic_ms(now)) return -EIO;
    // This entry also recognizes password tokens; never log their contents.
    std::printf("ASR_INFER RESULT seconds=%.3f bytes=%u\n",
                double(now - start) / 1000.0,
                unsigned(std::strlen(utf8_text)));
    return 0;
  } catch (const std::bad_alloc &) {
    return -ENOMEM;
  } catch (...) {
    return -EIO;
  }
}

int validate(const k7_asr_audio_request *request, char *utf8_text,
             size_t capacity) {
  if (utf8_text && capacity) utf8_text[0] = '\0';
  if (!request || !utf8_text || !capacity) return -EINVAL;
  if (!request->samples || !request->frames) return -ENODATA;
  if (request->frames > kMaxFrames) return -EOVERFLOW;
  if (request->sample_rate_hz != kSampleRate) return -EINVAL;
  return 0;
}

}  // namespace

extern "C" int k7_asr_audio_text(const k7_asr_audio_request *request,
                                  char *utf8_text, size_t capacity) {
  const int error = validate(request, utf8_text, capacity);
  if (error) return error;
  const int result = recognize_audio(*request, utf8_text, capacity);
  if (result) utf8_text[0] = '\0';
  return result;
}

extern "C" int k7_asr_model_session_probe(void) {
  // The immutable diagnostic WAV is longer than the 3-second microphone
  // contract. Bound it separately; do not relax the public capture limit.
  static_assert(test_frames > 0 && test_frames <= 15u * kSampleRate,
                "fixed diagnostic WAV must fit the 15-second budget");
  static_assert(sizeof(test_pcm) >= 2u * test_frames,
                "fixed diagnostic PCM is truncated");
  std::unique_ptr<float[]> samples(new (std::nothrow) float[test_frames]);
  if (!samples) return -ENOMEM;
  for (unsigned index = 0; index < test_frames; ++index) {
    const unsigned byte = 2 * index;
    int value = test_pcm[byte] | (unsigned(test_pcm[byte + 1]) << 8);
    if (value >= 32768) value -= 65536;
    samples[index] = float(value) / 32768.0f;
  }
  char text[256]{};
  const k7_asr_audio_request request{samples.get(), test_frames, kSampleRate,
                                     0, nullptr, nullptr};
  const int result = recognize_audio(request, text, sizeof(text));
  std::printf("ASR_MODEL result=%d frames=%u bytes=%u\n", result,
              test_frames, result ? 0u : unsigned(std::strlen(text)));
  return result;
}

extern "C" int k7_asr_microphone_text(char *utf8_text, size_t capacity) {
  if (utf8_text && capacity) utf8_text[0] = '\0';
  if (!utf8_text || !capacity) return -EINVAL;
  std::unique_ptr<float[]> samples(new (std::nothrow) float[kMaxFrames]);
  if (!samples) return -ENOMEM;
  unsigned frames = 0;
  const int capture_error =
      k7sound_copy_mono(samples.get(), kMaxFrames, &frames);
  if (capture_error) return capture_error;
  const k7_asr_audio_request request{samples.get(), frames, kSampleRate,
                                     0, nullptr, nullptr};
  return k7_asr_audio_text(&request, utf8_text, capacity);
}

extern "C" int k7_asr_microphone_probe(void) {
  char text[256];
  return k7_asr_microphone_text(text, sizeof(text));
}
