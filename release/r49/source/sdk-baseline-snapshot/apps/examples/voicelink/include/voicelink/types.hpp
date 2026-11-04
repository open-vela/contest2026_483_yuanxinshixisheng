#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace voicelink {

enum class State {
  WaitingWake,
  ScanningWifi,
  WaitingNetworkChoice,
  EnteringPassword,
  ConfirmingPassword,
  CredentialsReceived,
  ConnectingWifi,
  Completed,
  Error,
  Idle,
};

enum class Grammar { WakeWord, NetworkSelection, PasswordToken, Confirmation };
enum class PasswordControl { Finish, DeleteLast, Clear, Restart, Cancel, ReportLength,
                             SwitchUpper, SwitchLower };
enum class PasswordMode { Lower, Upper };

struct AccessPoint {
  std::string ssid;
  int rssi{};
  bool secured{};
};

struct PasswordToken {
  std::optional<char> character;
  std::optional<PasswordControl> control;
  bool case_explicit{false}; /* true if 大写/小写 prefix was used */
};

// 用户于 2026-09-13 指定「你好」前缀唤醒，并保留「开始联网」。
inline const std::vector<std::string>& defaultWakeWords() {
  static const std::vector<std::string> words{"你好", "开始联网"};
  return words;
}

struct Config {
  std::vector<std::string> wake_words{defaultWakeWords()};
  std::string greeting{"开始扫描网络"};
  std::size_t max_networks{5};
  std::uint64_t connect_timeout_ms{30000};
  std::uint64_t scan_timeout_ms{30000};
  std::size_t min_password_length{8};
  std::size_t max_password_length{63};
};

// Receipt/association is not IP readiness. No legacy synchronous Ok status.
enum class ConnectStatus {
  CredentialsReceived, Connecting, IpReady, WrongPassword, Timeout,
  NetworkNotFound, Failed, Busy, Unsupported, Cancelled
};
struct ConnectResult {
  ConnectStatus status{ConnectStatus::Failed};
  int error_code{};
  std::uint64_t request_id{};
  std::string ipv4;
};

struct Context {
  State state{State::WaitingWake};
  Grammar grammar{Grammar::WakeWord};
  std::vector<AccessPoint> candidates;
  std::optional<std::size_t> selected_index;
  std::vector<char> password;
  PasswordMode password_mode{PasswordMode::Lower};
};

}  // namespace voicelink
