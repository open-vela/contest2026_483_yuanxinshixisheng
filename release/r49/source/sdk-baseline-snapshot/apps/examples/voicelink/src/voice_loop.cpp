#include "voicelink/voice_loop.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace voice_loop {
namespace {

void wipe(std::string& value) noexcept {
  volatile char* bytes = value.empty() ? nullptr : &value[0];
  for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
  value.clear();
}

struct TextWiper {
  std::string& text;
  ~TextWiper() { wipe(text); }
};

bool validText(const std::string& text, std::size_t limit) noexcept {
  if (text.empty() || text.size() > limit || text.find('\0') != std::string::npos)
    return false;
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  std::size_t i = 0;
  while (i < text.size()) {
    unsigned c = p[i++], count = 0, low = 0x80, high = 0xbf;
    if (c < 0x80) { if (c < 0x20 && c != '\t' && c != '\n') return false; continue; }
    if (c >= 0xc2 && c <= 0xdf) count = 1;
    else if (c >= 0xe0 && c <= 0xef) { count = 2; if (c == 0xe0) low = 0xa0; if (c == 0xed) high = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 3; if (c == 0xf0) low = 0x90; if (c == 0xf4) high = 0x8f; }
    else return false;
    if (count > text.size() - i || p[i] < low || p[i] > high) return false;
    ++i;
    while (--count) { if (p[i] < 0x80 || p[i] > 0xbf) return false; ++i; }
  }
  return true;
}

bool elapsed(std::uint64_t now, std::uint64_t start, std::uint64_t timeout) noexcept {
  return now >= start && now - start >= timeout;
}

}  // namespace

Loop::Loop(voicelink::Controller& controller, CapturePort& capture, AsrPort& asr,
           voicelink::ClockPort& clock, Config config)
    : controller_(controller), capture_(capture), asr_(asr), clock_(clock),
      config_(config) {
  if (!config_.capture_timeout_ms) config_.capture_timeout_ms = 1;
  if (!config_.asr_timeout_ms) config_.asr_timeout_ms = 1;
  if (!config_.retry_delay_ms) config_.retry_delay_ms = 1;
  if (!config_.max_text_bytes) config_.max_text_bytes = 1;
}

Loop::~Loop() { cancelActive(); }

bool Loop::start() {
  if (started_ || phase_ != Phase::Idle) return false;
  started_ = true;
  last_error_ = 0;
  last_now_ms_ = clock_.nowMs();
  controller_.start();
  reconcileController();
  return phase_ != Phase::Error;
}

bool Loop::speechState() const noexcept {
  const auto state = controller_.context().state;
  return state == voicelink::State::WaitingWake ||
         state == voicelink::State::WaitingNetworkChoice ||
         state == voicelink::State::EnteringPassword ||
         state == voicelink::State::ConfirmingPassword;
}

void Loop::scheduleInput() noexcept {
  if (!started_ || !speechState() || capture_id_ || asr_id_) return;
  phase_ = Phase::WaitingPort;
  retry_at_ms_ = clock_.nowMs();
}

void Loop::beginCapture() {
  const auto now = clock_.nowMs();
  auto result = capture_.begin(controller_.context().grammar);
  if (result.status == CaptureStatus::Busy) {
    retry_at_ms_ = now > std::numeric_limits<std::uint64_t>::max() - config_.retry_delay_ms
                    ? std::numeric_limits<std::uint64_t>::max()
                    : now + config_.retry_delay_ms;
    return;
  }
  if (!result.request_id) { failInput(voicelink::IoStatus::FatalError, -1); return; }
  capture_id_ = result.request_id;
  stage_started_ms_ = now;
  applyCapture(std::move(result));
}

void Loop::applyCapture(CaptureResult result) {
  if (!capture_id_ || result.request_id != capture_id_) return;
  switch (result.status) {
    case CaptureStatus::Accepted:
    case CaptureStatus::Recording: phase_ = Phase::Recording; return;
    case CaptureStatus::Complete: {
      auto asr = asr_.begin(capture_id_, controller_.context().grammar);
      if (asr.status == AsrStatus::Busy) {
        releaseCapture();
        failInput(voicelink::IoStatus::RetryableError, asr.error_code);
        return;
      }
      if (!asr.request_id) {
        releaseCapture();
        failInput(voicelink::IoStatus::FatalError, asr.error_code ? asr.error_code : -1);
        return;
      }
      asr_id_ = asr.request_id;
      stage_started_ms_ = clock_.nowMs();
      phase_ = Phase::Recognizing;
      applyAsr(std::move(asr));
      return;
    }
    case CaptureStatus::Timeout: releaseCapture(); failInput(voicelink::IoStatus::Timeout, result.error_code); return;
    case CaptureStatus::Cancelled: releaseCapture(); failInput(voicelink::IoStatus::EndOfInput, result.error_code); return;
    case CaptureStatus::Busy: return;
    case CaptureStatus::Failed: releaseCapture(); failInput(voicelink::IoStatus::RetryableError, result.error_code); return;
  }
}

void Loop::applyAsr(AsrResult result) {
  TextWiper text_wiper{result.text};
  if (!asr_id_ || result.request_id != asr_id_) return;
  switch (result.status) {
    case AsrStatus::Accepted:
    case AsrStatus::Recognizing: return;
    case AsrStatus::Ready: {
      const bool valid = validText(result.text, config_.max_text_bytes);
      asr_id_ = 0;
      releaseCapture();
      if (!valid) {
        failInput(voicelink::IoStatus::RetryableError, result.error_code ? result.error_code : -1);
        return;
      }
      ++turn_;
      controller_.ingest(result.text);
      reconcileController();
      return;
    }
    case AsrStatus::Timeout:
      asr_id_ = 0; releaseCapture();
      failInput(voicelink::IoStatus::Timeout, result.error_code); return;
    case AsrStatus::Cancelled:
      asr_id_ = 0; releaseCapture();
      failInput(voicelink::IoStatus::EndOfInput, result.error_code); return;
    case AsrStatus::Busy: return;
    case AsrStatus::Failed:
      asr_id_ = 0; releaseCapture();
      failInput(voicelink::IoStatus::RetryableError, result.error_code); return;
  }
}

void Loop::releaseCapture() noexcept {
  const auto id = capture_id_;
  capture_id_ = 0;
  if (id) capture_.release(id);
}

void Loop::failInput(voicelink::IoStatus status, int error) {
  last_error_ = error;
  if (!controller_.handleInputStatus(status)) {
    reconcileController();
    return;
  }
  reconcileController();
}

void Loop::cancelActive() noexcept {
  if (asr_id_) { asr_.cancel(asr_id_); asr_id_ = 0; }
  if (capture_id_) { capture_.cancel(capture_id_); releaseCapture(); }
}

void Loop::cancel() {
  if (!started_) return;
  cancelActive();
  controller_.cancel();
  reconcileController();
}

void Loop::reconcileController() noexcept {
  switch (controller_.context().state) {
    case voicelink::State::Completed: phase_ = Phase::Completed; return;
    case voicelink::State::Error: phase_ = Phase::Error; return;
    case voicelink::State::Idle: phase_ = Phase::Idle; started_ = false; return;
    case voicelink::State::ScanningWifi:
    case voicelink::State::CredentialsReceived:
    case voicelink::State::ConnectingWifi: phase_ = Phase::ControllerWork; return;
    default: scheduleInput(); return;
  }
}

void Loop::tick() {
  if (!started_ || phase_ == Phase::Idle || phase_ == Phase::Completed || phase_ == Phase::Error) return;
  const auto now = clock_.nowMs();
  if (now < last_now_ms_) {
    last_error_ = -2;
    cancelActive();
    controller_.handleInputStatus(voicelink::IoStatus::FatalError);
    reconcileController();
    return;
  }
  last_now_ms_ = now;
  controller_.poll();
  reconcileController();
  if (phase_ == Phase::WaitingPort && now >= retry_at_ms_) { beginCapture(); return; }
  if (phase_ == Phase::Recording) {
    if (elapsed(now, stage_started_ms_, config_.capture_timeout_ms)) {
      capture_.cancel(capture_id_); releaseCapture();
      failInput(voicelink::IoStatus::Timeout, -3);
      return;
    }
    applyCapture(capture_.poll(capture_id_));
    return;
  }
  if (phase_ == Phase::Recognizing) {
    if (elapsed(now, stage_started_ms_, config_.asr_timeout_ms)) {
      asr_.cancel(asr_id_); asr_id_ = 0; releaseCapture();
      failInput(voicelink::IoStatus::Timeout, -4);
      return;
    }
    applyAsr(asr_.poll(asr_id_));
  }
}

}  // namespace voice_loop
