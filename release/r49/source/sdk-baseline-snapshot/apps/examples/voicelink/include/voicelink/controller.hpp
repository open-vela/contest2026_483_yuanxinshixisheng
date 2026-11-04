#pragma once

#include "voicelink/parsers.hpp"
#include "voicelink/ports.hpp"

#include <string>

namespace voicelink {

class Controller {
 public:
  Controller(TtsPort& tts, WifiPort& wifi, ClockPort& clock, Config config = {});
  ~Controller();

  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  void start();
  void poll(); // schedule independently of ASR (recommended <= 50 ms)
  void cancel();
  void ingest(const std::string& recognized_text);
  // Handle a non-Ok input status. Returns true if the loop may continue, false
  // if the caller must stop the run loop. On EndOfInput passwords are wiped; on
  // FatalError the state becomes Error. Timeout keeps the current state.
  bool handleInputStatus(IoStatus status);
  void resetToWake();
  const Context& context() const { return context_; }
  const Config& config() const { return config_; }

 private:
  void beginScan();
  void applyScanResult(ScanResult result);
  void cancelScanPending();
  void announceNetworks();
  void handleNetwork(const std::string& text);
  void handlePassword(const std::string& text);
  void handlePasswordConfirmation(const std::string& text);
  void handlePasswordControl(PasswordControl control);
  void finishPasswordRound();
  void tryConnect(const char* password, std::size_t password_len);
  void applyConnectResult(const ConnectResult& result);
  void cancelPending();
  void repeat(const std::string& text);
  void wipePassword();
  void setTerminal(State state, const std::string& text);

  TtsPort& tts_;
  WifiPort& wifi_;
  ClockPort& clock_;
  std::uint64_t request_id_{};
  std::uint64_t started_ms_{};
  std::uint64_t scan_request_id_{};
  std::uint64_t scan_started_ms_{};
  Config config_;
  Context context_;
};

}  // namespace voicelink
