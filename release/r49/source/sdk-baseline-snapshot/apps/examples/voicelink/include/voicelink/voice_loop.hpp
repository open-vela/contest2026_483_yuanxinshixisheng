#pragma once

#include "voicelink/controller.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace voice_loop {

enum class CaptureStatus { Accepted, Recording, Complete, Busy, Timeout, Cancelled, Failed };

struct CaptureResult {
  CaptureStatus status{CaptureStatus::Failed};
  std::uint64_t request_id{};
  int error_code{};
};

class CapturePort {
 public:
  virtual ~CapturePort() = default;
  // Starts one utterance recording for the supplied Controller grammar. IDs are
  // unique for this port lifetime. Complete means an immutable capture exists.
  virtual CaptureResult begin(voicelink::Grammar grammar) noexcept = 0;
  virtual CaptureResult poll(std::uint64_t request_id) noexcept = 0;
  virtual void cancel(std::uint64_t request_id) noexcept = 0;
  // Idempotent. The capture remains owned by this port until release returns.
  virtual void release(std::uint64_t request_id) noexcept = 0;
};

enum class AsrStatus { Accepted, Recognizing, Ready, Busy, Timeout, Cancelled, Failed };

struct AsrResult {
  AsrStatus status{AsrStatus::Failed};
  std::uint64_t request_id{};
  int error_code{};
  std::string text;
};

class AsrPort {
 public:
  virtual ~AsrPort() = default;
  // The implementation reads the immutable capture synchronously or retains a
  // safe reference until its transaction terminates. For PasswordToken grammar
  // it must never log text and must erase internal transcript copies on exit.
  virtual AsrResult begin(std::uint64_t capture_id,
                          voicelink::Grammar grammar) noexcept = 0;
  virtual AsrResult poll(std::uint64_t request_id) noexcept = 0;
  virtual void cancel(std::uint64_t request_id) noexcept = 0;
};

enum class Phase { Idle, WaitingPort, Recording, Recognizing, ControllerWork, Completed, Error };

struct Config {
  std::uint64_t capture_timeout_ms{15000};
  std::uint64_t asr_timeout_ms{60000};
  std::uint64_t retry_delay_ms{50};
  std::size_t max_text_bytes{512};
};

class Loop {
 public:
  Loop(voicelink::Controller& controller, CapturePort& capture, AsrPort& asr,
       voicelink::ClockPort& clock, Config config = {});
  ~Loop();
  Loop(const Loop&) = delete;
  Loop& operator=(const Loop&) = delete;

  bool start();
  void tick();
  void cancel();

  Phase phase() const noexcept { return phase_; }
  std::uint64_t turn() const noexcept { return turn_; }
  int lastError() const noexcept { return last_error_; }

 private:
  bool speechState() const noexcept;
  void scheduleInput() noexcept;
  void beginCapture();
  void applyCapture(CaptureResult result);
  void applyAsr(AsrResult result);
  void failInput(voicelink::IoStatus status, int error);
  void releaseCapture() noexcept;
  void cancelActive() noexcept;
  void reconcileController() noexcept;

  voicelink::Controller& controller_;
  CapturePort& capture_;
  AsrPort& asr_;
  voicelink::ClockPort& clock_;
  Config config_;
  Phase phase_{Phase::Idle};
  std::uint64_t capture_id_{};
  std::uint64_t asr_id_{};
  std::uint64_t stage_started_ms_{};
  std::uint64_t retry_at_ms_{};
  std::uint64_t last_now_ms_{};
  std::uint64_t turn_{};
  int last_error_{};
  bool started_{};
};

}  // namespace voice_loop
