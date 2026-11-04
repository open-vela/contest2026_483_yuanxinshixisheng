/* Native integration diagnostic. No fake speech input or success backend. */
#include "voicelink/asr_runtime.h"
#include "voicelink/controller.hpp"
#include "voicelink/voice_loop.hpp"
#include "voice_wifi_adapter.hpp"
#include "k7sound_api.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#ifdef K7VOICE_NATIVE_ORT_ADD
extern "C" int k7_ort_add_probe(void);
#endif
static bool now_ms(std::uint64_t &out)
{
  timespec t{};
  if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0) return false;
  out = std::uint64_t(t.tv_sec) * 1000 + std::uint64_t(t.tv_nsec) / 1000000;
  return true;
}

#include "ui_connect.hpp"
#ifdef K7VOICE_NATIVE_TTS
#include "native_speech_output.hpp"
#include "tts_assets.hpp"
#endif
#ifdef K7VOICE_PROMPT_ASSETS
#include "prompt_asset_output.hpp"
#endif

namespace {

class ConsoleTts final : public voicelink::TtsPort
{
 public:
  bool speak(const std::string &text) override
  {
    std::printf("VOICE_PROMPT %s\n", text.c_str());
    return true;
  }

  void stop() override {}
};

class MonotonicClock final : public voicelink::ClockPort
{
 public:
  std::uint64_t nowMs() const noexcept override
  {
    std::uint64_t value = 0;
    return now_ms(value) ? value : 0;
  }
};

int run_controller_scan()
{
  auto port = voicelink::nativeWifiPort();
  if (!port)
    {
      std::puts("VOICE_FLOW native_network=0");
      return 1;
    }
  ConsoleTts tts;
  MonotonicClock clock;
  voicelink::Controller controller(tts, *port, clock);
  controller.start();
  controller.ingest("开始联网");
  while (controller.context().state == voicelink::State::ScanningWifi)
    {
      controller.poll();
      usleep(10000);
    }
  const auto &context = controller.context();
  std::printf("VOICE_FLOW state=%u grammar=%u count=%u\n",
              unsigned(context.state), unsigned(context.grammar),
              unsigned(context.candidates.size()));
  return context.state == voicelink::State::WaitingNetworkChoice ? 0 : 1;
}

#ifdef K7VOICE_NATIVE_ASR_MIC
class PromptToneTts final : public voicelink::TtsPort
{
 public:
  bool speak(const std::string &text) override
  {
    std::printf("VOICE_PROMPT %s\n", text.c_str());
    return k7sound_play_prompt_tone() == 0;
  }

  void stop() override {}
};

class SynchronousCapturePort final : public voice_loop::CapturePort
{
 public:
  voice_loop::CaptureResult begin(voicelink::Grammar) noexcept override
  {
    const auto id = ++next_id_;
    const int error = k7sound_capture_grouped(48000);
    return {error ? voice_loop::CaptureStatus::Failed
                  : voice_loop::CaptureStatus::Complete,
            id, error};
  }

  voice_loop::CaptureResult poll(std::uint64_t id) noexcept override
  {
    return {voice_loop::CaptureStatus::Failed, id, -EIO};
  }

  void cancel(std::uint64_t) noexcept override {}
  void release(std::uint64_t) noexcept override {}

 private:
  std::uint64_t next_id_{};
};

class SynchronousAsrPort final : public voice_loop::AsrPort
{
 public:
  voice_loop::AsrResult begin(std::uint64_t, voicelink::Grammar) noexcept override
  {
    const auto id = ++next_id_;
    char text[256]{};
    const int error = k7_asr_microphone_text(text, sizeof(text));
    struct Wipe { char* p; ~Wipe() { volatile char* v=p; for(unsigned i=0;i<256;++i) v[i]=0; } } wipe{text};
    if (error) return {voice_loop::AsrStatus::Failed, id, error, {}};
    return {voice_loop::AsrStatus::Ready, id, 0, text};
  }

  voice_loop::AsrResult poll(std::uint64_t id) noexcept override
  {
    return {voice_loop::AsrStatus::Failed, id, -EIO, {}};
  }

  void cancel(std::uint64_t) noexcept override {}

 private:
  std::uint64_t next_id_{};
};

int run_controller_microphone_wake()
{
  auto port = voicelink::nativeWifiPort();
  if (!port)
    {
      std::puts("VOICE_FLOW native_network=0");
      return 1;
    }
  char text[256]{};
  const int asr = k7_asr_microphone_text(text, sizeof(text));
  if (asr)
    {
      std::printf("VOICE_FLOW asr_error=%d\n", asr);
      return 1;
    }
  std::printf("VOICE_FLOW wake_text=%s bytes=%u\n", text,
              unsigned(std::strlen(text)));
  ConsoleTts tts;
  MonotonicClock clock;
  voicelink::Controller controller(tts, *port, clock);
  controller.start();
  controller.ingest(text);
  while (controller.context().state == voicelink::State::ScanningWifi)
    {
      controller.poll();
      usleep(10000);
    }
  const auto &context = controller.context();
  std::printf("VOICE_FLOW mic_state=%u grammar=%u count=%u\n",
              unsigned(context.state), unsigned(context.grammar),
              unsigned(context.candidates.size()));
  for (const auto &ap : context.candidates)
    {
      std::printf("VOICE AP ssid_hex=");
      for (unsigned char c : ap.ssid) std::printf("%02x", unsigned(c));
      std::printf("\n");
    }
  return context.state == voicelink::State::WaitingNetworkChoice ? 0 : 1;
}

#ifdef K7VOICE_NATIVE_TTS
class SpokenTts final : public voicelink::TtsPort {
 public:
  explicit SpokenTts(NativeSpeechOutput& output) : output_(output) {}
  bool speak(const std::string& text) override { return output_.say(text) == 0; }
  void stop() override {}
 private:
  NativeSpeechOutput& output_;
};

class SpokenCapture final : public voice_loop::CapturePort {
 public:
  explicit SpokenCapture(NativeSpeechOutput& output) : output_(output) {}
  voice_loop::CaptureResult begin(voicelink::Grammar grammar) noexcept override {
    const auto id = ++next_id_;
    // This is a sequential setup session, not simultaneous playback and KWS.
    // The driver settles ADC before playing, then records after the prompt.
    const int result = output_.say(grammar == voicelink::Grammar::WakeWord ?
        "设备还没有联网，请联网。" : "请说话。", 48000);
    return {result ? voice_loop::CaptureStatus::Failed : voice_loop::CaptureStatus::Complete, id, result};
  }
  voice_loop::CaptureResult poll(std::uint64_t id) noexcept override {
    return {voice_loop::CaptureStatus::Failed, id, -EIO};
  }
  void cancel(std::uint64_t) noexcept override {}
  void release(std::uint64_t) noexcept override {}
 private:
  NativeSpeechOutput& output_;
  std::uint64_t next_id_{};
};
#endif

#ifdef K7VOICE_PROMPT_ASSETS
class AssetTts final : public voicelink::TtsPort {
 public:
  explicit AssetTts(PromptAssetOutput& output) : output_(output) {}
  bool speak(const std::string& text) override { return output_.say(text) == 0; }
  void stop() override {}
 private:
  PromptAssetOutput& output_;
};

class AssetCapture final : public voice_loop::CapturePort {
 public:
  explicit AssetCapture(PromptAssetOutput& output) : output_(output) {}
  voice_loop::CaptureResult begin(voicelink::Grammar grammar) noexcept override {
    const auto id = ++next_id_;
    const int result = output_.say(grammar == voicelink::Grammar::WakeWord ?
        "设备还没有联网，请联网。" : "请说话。", 48000);
    return {result ? voice_loop::CaptureStatus::Failed :
                     voice_loop::CaptureStatus::Complete, id, result};
  }
  voice_loop::CaptureResult poll(std::uint64_t id) noexcept override {
    return {voice_loop::CaptureStatus::Failed, id, -EIO};
  }
  void cancel(std::uint64_t) noexcept override {}
  void release(std::uint64_t) noexcept override {}
 private:
  PromptAssetOutput& output_;
  std::uint64_t next_id_{};
};
#endif

int run_controller_microphone_provision()
{
  auto port = voicelink::nativeWifiPort();
  if (!port)
    {
      std::puts("VOICE_FLOW native_network=0");
      return 1;
    }
#if defined(K7VOICE_PROMPT_ASSETS)
  PromptAssetOutput speech;
  AssetTts tts(speech);
  AssetCapture capture(speech);
#elif defined(K7VOICE_NATIVE_TTS)
  if (mount_tts_assets()) {
    std::puts("VOICE_PROVISION verified TTS resources unavailable");
    return 1;
  }
  NativeSpeechOutput speech("/mnt/k7tts/vits.ort", "/mnt/k7tts/tokens.txt", "/mnt/k7tts/lexicon.txt");
  SpokenTts tts(speech);
  SpokenCapture capture(speech);
#else
  PromptToneTts tts;
  SynchronousCapturePort capture;
#endif
  MonotonicClock clock;
  voicelink::Controller controller(tts, *port, clock);
  SynchronousAsrPort asr;
  voice_loop::Config config;
  config.capture_timeout_ms = 10000;
  config.asr_timeout_ms = 180000;
  config.max_text_bytes = 255;
  voice_loop::Loop loop(controller, capture, asr, clock, config);
  std::puts("VOICE_PROVISION ready");
  if (!loop.start()) return 1;
  unsigned previous_phase = ~0u;
  unsigned previous_state = ~0u;
  unsigned previous_grammar = ~0u;
  std::uint64_t previous_turn = ~std::uint64_t{};
  auto report_status = [&]() {
    const auto &context = controller.context();
    const unsigned phase = unsigned(loop.phase());
    const unsigned state = unsigned(context.state);
    const unsigned grammar = unsigned(context.grammar);
    const auto turn = loop.turn();
    if (phase == previous_phase && state == previous_state &&
        grammar == previous_grammar && turn == previous_turn) return;
    std::printf("VOICE_PROVISION_STATUS phase=%u state=%u grammar=%u "
                "turn=%llu candidates=%u selected=%u password_length=%u\n",
                phase, state, grammar,
                static_cast<unsigned long long>(turn),
                unsigned(context.candidates.size()),
                context.selected_index.has_value() ? 1u : 0u,
                unsigned(context.password.size()));
    std::fflush(stdout);
    previous_phase = phase;
    previous_state = state;
    previous_grammar = grammar;
    previous_turn = turn;
  };
  report_status();
  const auto started = clock.nowMs();
  while (loop.phase() != voice_loop::Phase::Completed &&
         loop.phase() != voice_loop::Phase::Error &&
         loop.phase() != voice_loop::Phase::Idle)
    {
      const auto now = clock.nowMs();
      if (now < started || now - started >= 600000)
        {
          loop.cancel();
          std::puts("VOICE_PROVISION timeout");
          return 1;
        }
      loop.tick();
      report_status();
      usleep(10000);
    }
  report_status();
  std::printf("VOICE_PROVISION phase=%u turns=%llu error=%d\n",
              unsigned(loop.phase()),
              static_cast<unsigned long long>(loop.turn()), loop.lastError());
  return loop.phase() == voice_loop::Phase::Completed ? 0 : 1;
}
#endif

}  // namespace

extern "C" int main(int argc, char **argv)
{
#ifdef K7VOICE_NATIVE_ASR_MODEL
  struct AsrRuntimeCleanup {
    ~AsrRuntimeCleanup() { k7_asr_runtime_reset(); }
  } asr_runtime_cleanup;
#endif
#ifdef K7VOICE_NATIVE_TTS
  if (argc == 2 && !std::strcmp(argv[1], "tts-smoke")) {
    const int assets = mount_tts_assets();
    if (assets) { std::printf("VOICE_TTS assets_error=%d\n", assets); return 1; }
    NativeSpeechOutput speech("/mnt/k7tts/vits.ort", "/mnt/k7tts/tokens.txt", "/mnt/k7tts/lexicon.txt");
    const int result = speech.say("你好，网络配置已开始。");
    std::printf("VOICE_TTS result=%d\n", result);
    return result ? 1 : 0;
  }
#endif
#ifdef K7VOICE_PROMPT_ASSETS
  if (argc == 2 && !std::strcmp(argv[1], "prompt-smoke")) {
    PromptAssetOutput speech;
    const int result = speech.say("设备还没有联网，请联网。");
    std::printf("VOICE_PROMPT_SMOKE result=%d\n", result);
    return result ? 1 : 0;
  }
#endif
  if (argc == 2 && !std::strcmp(argv[1], "connect-ui")) return run_ui_connect();
#ifdef K7VOICE_NATIVE_ASR_MODEL
  if (argc == 2 && !std::strcmp(argv[1], "asr-model"))
    return k7_asr_model_session_probe();
#ifdef K7VOICE_NATIVE_ASR_MIC
  if (argc == 2 && !std::strcmp(argv[1], "asr-mic"))
    return k7_asr_microphone_probe();
#endif
#endif
#ifdef K7VOICE_NATIVE_ORT_ADD
  if (argc == 2 && !std::strcmp(argv[1], "ort-add"))
    return k7_ort_add_probe();
#endif
#ifdef K7VOICE_NATIVE_ASR_MIC
  if (argc == 2 && !std::strcmp(argv[1], "flow-mic-wake"))
    return run_controller_microphone_wake();
  if (argc == 2 && !std::strcmp(argv[1], "flow-mic-provision"))
    return run_controller_microphone_provision();
#endif
  if (argc == 2 && !std::strcmp(argv[1], "flow-scan"))
    return run_controller_scan();
  if (argc != 2 || (std::strcmp(argv[1], "probe") && std::strcmp(argv[1], "scan")))
    {
      std::puts("k7voice probe|scan|flow-scan|flow-mic-wake|flow-mic-provision|asr-model|asr-mic");
      std::puts("flow-scan uses the formal Controller with real Wi-Fi and console prompts");
      std::puts("flow-mic-wake consumes the last completed microphone capture");
      return 1;
    }
  auto port = voicelink::nativeWifiPort();
  std::printf("VOICE native_network=%d asr=0 tts=0 speech_provisioning=0\n", !!port);
  if (!port) return 1;
  if (!std::strcmp(argv[1], "probe")) return 0;
  std::uint64_t id = 0, start = 0, now = 0;
  if (!now_ms(start)) return 1;
  try
    {
      auto r = port->beginScan(); id = r.request_id;
      std::printf("VOICE scan accepted=%d id=%llu error=%d\n",
        r.status == voicelink::ScanStatus::Received,
        static_cast<unsigned long long>(id), r.error_code);
      if (r.status != voicelink::ScanStatus::Received) return 1;
      for (;;)
        {
          if (!now_ms(now) || now < start || now - start >= 35000)
            { port->cancelScan(id); std::puts("VOICE scan timeout; cancellation requested, service retains cleanup ownership"); return 1; }
          r = port->pollScan(id);
          if (r.status == voicelink::ScanStatus::Received || r.status == voicelink::ScanStatus::Scanning)
            { usleep(10000); continue; }
          if (r.status != voicelink::ScanStatus::Ready)
            { port->cancelScan(id); std::printf("VOICE scan terminal=%u error=%d\n", unsigned(r.status), r.error_code); return 1; }
          std::printf("VOICE scan_done id=%llu count=%u\n", static_cast<unsigned long long>(id), unsigned(r.access_points.size()));
          // Names are rendered as bytes to prevent SSID control text affecting logs.
          for (const auto &ap : r.access_points)
            {
              std::printf("VOICE AP ssid_hex=");
              for (unsigned char c : ap.ssid) std::printf("%02x", unsigned(c));
              std::printf("\n");
            }
          return 0;
        }
    }
  catch (...)
    { if (id) port->cancelScan(id); std::puts("VOICE scan exception; cleanup owned by service"); return 1; }
}
