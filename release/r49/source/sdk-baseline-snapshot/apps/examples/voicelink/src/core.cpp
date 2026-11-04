#include "voicelink/controller.hpp"
#include "voicelink/spoken_ssid.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace voicelink {
namespace {

std::string spokenCount(std::size_t value) {
  static const char* digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  // Passwords are bounded to 63 characters, avoiding a large text normalizer.
  if (value < 10) return digits[value];
  if (value > 63) return "超出范围";
  std::string out = value < 20 ? "十" : std::string(digits[value / 10]) + "十";
  if (value % 10) out += digits[value % 10];
  return out;
}

void secureWipe(std::vector<char>& value) {
  volatile char* bytes = value.empty() ? nullptr : value.data();
  for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
  value.clear();
  // Keep reserved storage: no allocation/reallocation while entering secrets.
}

}  // namespace

Controller::Controller(TtsPort& tts, WifiPort& wifi, ClockPort& clock, Config config)
    : tts_(tts), wifi_(wifi), clock_(clock), config_(std::move(config)) {
  if (config_.wake_words.empty()) config_.wake_words = defaultWakeWords();
  config_.max_networks = std::min<std::size_t>(5, config_.max_networks);
  config_.max_password_length = std::min<std::size_t>(63, config_.max_password_length);
  context_.password.reserve(config_.max_password_length);
  if (config_.min_password_length > config_.max_password_length)
    config_.min_password_length = config_.max_password_length;
}

Controller::~Controller() { cancelPending(); cancelScanPending(); wipePassword(); }

void Controller::start() { resetToWake(); }

void Controller::resetToWake() {
  wipePassword();
  cancelPending();
  cancelScanPending();
  context_.candidates.clear();
  context_.selected_index.reset();
  context_.password_mode = PasswordMode::Lower;
  context_.state = State::WaitingWake;
  context_.grammar = Grammar::WakeWord;
  // Never speak the wake phrase while arming KWS; doing so can self-trigger.
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  try { tts_.stop(); } catch (...) { context_.state = State::Error; }
#else
  tts_.stop();
#endif
}

void Controller::ingest(const std::string& text) {
  switch (context_.state) {
    case State::WaitingWake:
    case State::Idle:
      if (isWakeWord(text, config_.wake_words)) {
        repeat(config_.greeting);
        if (context_.state == State::Error) return;
        beginScan();
      }
      return;
    case State::WaitingNetworkChoice:
      handleNetwork(text);
      return;
    case State::EnteringPassword:
      handlePassword(text);
      return;
    case State::ConfirmingPassword:
      handlePasswordConfirmation(text);
      return;
    case State::CredentialsReceived:
    case State::ConnectingWifi:
    case State::ScanningWifi:
      if (isCancel(text)) cancel();
      return;
    default:
      return;
  }
}

bool Controller::handleInputStatus(IoStatus status) {
  switch (status) {
    case IoStatus::Ok:
      return true;  // caller should have used ingest()
    case IoStatus::Timeout:
      // No speech within the listen window: keep the current state and wait.
      return true;
    case IoStatus::RetryableError:
      // Brief prompt, retry the same grammar — state unchanged.
      repeat("没有听清，请再说一次");
      return context_.state != State::Error;
    case IoStatus::EndOfInput:
      cancelPending();
      cancelScanPending();
      context_.state = State::Idle;
      wipePassword();
      return false;
    case IoStatus::FatalError:
      cancelPending();
      cancelScanPending();
      wipePassword();
      context_.state = State::Error;
      return false;
  }
  return false;
}

void Controller::beginScan() {
  wipePassword();
  cancelScanPending();
  context_.state = State::ScanningWifi;
  context_.selected_index.reset();
  context_.candidates.clear();
  ScanResult result;
  scan_started_ms_ = clock_.nowMs();
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  try {
    result = wifi_.beginScan();
  } catch (...) {
    setTerminal(State::Error, "扫描失败，已停止当前流程");
    return;
  }
#else
  result = wifi_.beginScan();
#endif
  scan_request_id_ = result.request_id;
  if (!scan_request_id_) {
    setTerminal(State::Error, "扫描接口未提供事务编号");
    return;
  }
  applyScanResult(std::move(result));
}

void Controller::cancelScanPending() {
  const auto id = scan_request_id_;
  scan_request_id_ = 0;
  if (id) wifi_.cancelScan(id);
}

void Controller::applyScanResult(ScanResult result) {
  if (result.status == ScanStatus::Received || result.status == ScanStatus::Scanning) return;
  if (result.status != ScanStatus::Ready || result.access_points.size() > 64) {
    cancelScanPending();
    switch (result.status) {
      case ScanStatus::Busy: setTerminal(State::Idle, "网络服务忙，请稍后重新唤醒"); break;
      case ScanStatus::Unsupported: setTerminal(State::Idle, "当前不支持扫描，保留已有网络连接"); break;
      case ScanStatus::Timeout: setTerminal(State::Idle, "扫描超时，请稍后重新唤醒"); break;
      case ScanStatus::Cancelled: setTerminal(State::Idle, "已取消配置"); break;
      default: setTerminal(State::Error, "扫描失败，已停止当前流程"); break;
    }
    return;
  }
  // The service owns cleanup and releases its scan lease only after fencing RX.
  scan_request_id_ = 0;
  std::unordered_map<std::string, AccessPoint> best;
  for (auto& ap : result.access_points) {
    if (ap.ssid.empty()) continue;
    const auto found = best.find(ap.ssid);
    if (found == best.end() || ap.rssi > found->second.rssi) best[ap.ssid] = ap;
  }
  for (auto& item : best) context_.candidates.push_back(std::move(item.second));
  std::sort(context_.candidates.begin(), context_.candidates.end(),
            [](const AccessPoint& a, const AccessPoint& b) { return a.rssi > b.rssi; });
  if (context_.candidates.size() > config_.max_networks)
    context_.candidates.resize(config_.max_networks);
  context_.state = State::WaitingNetworkChoice;
  context_.grammar = Grammar::NetworkSelection;
  announceNetworks();
}

void Controller::announceNetworks() {
  if (context_.candidates.empty()) {
    repeat("没有发现网络，请说重新扫描或取消");
    return;
  }
  static const char* names[] = {"一", "二", "三", "四", "五"};
  repeat("发现以下网络。");
  for (std::size_t i = 0; i < context_.candidates.size() && context_.state != State::Error; ++i)
    repeat("网络" + std::string(names[i]) + "，" + spokenSsid(context_.candidates[i].ssid) + "。");
  if (context_.state != State::Error) repeat("请说网络编号，例如选择一。");
}

void Controller::handleNetwork(const std::string& text) {
  if (isRescan(text)) {
    beginScan();
    return;
  }
  if (isCancel(text)) {
    setTerminal(State::Idle, "已取消配置");
    return;
  }
  const auto index = parseNetworkIndex(text);
  if (!index || *index < 1 || static_cast<std::size_t>(*index) > context_.candidates.size()) {
    repeat("编号无效，请重新选择网络");
    return;
  }
  context_.selected_index = static_cast<std::size_t>(*index - 1);
  const auto& ap = context_.candidates[*context_.selected_index];
  if (!ap.secured) {
    context_.state = State::ConfirmingPassword;
    context_.grammar = Grammar::Confirmation;
    repeat("已选择开放网络" + spokenSsid(ap.ssid) + "，确认连接请说确认提交");
    return;
  }
  context_.state = State::EnteringPassword;
  context_.grammar = Grammar::PasswordToken;
  context_.password_mode = PasswordMode::Lower;
  repeat("已选择" + spokenSsid(ap.ssid) + "，请逐个输入密码字符，默认小写，说切换大写输入大写，完成后说完成");
}

void Controller::handlePassword(const std::string& text) {
  if (isNetworkRestart(text)) {
    beginScan();
    return;
  }
  const auto token = parsePasswordToken(text);
  if (!token) {
    repeat("没有听清，请再说一次当前字符");
    return;
  }
  if (token->control) {
    handlePasswordControl(*token->control);
    return;
  }
  auto& password = context_.password;
  if (password.size() >= config_.max_password_length) {
    repeat("已达到最大长度，请说完成、删除、清空、重输或取消");
    return;
  }
  char ch = *token->character;
  /* Apply current mode to bare lowercase letters (a-z) only when
   * no explicit 大写/小写 prefix was used. Digits and symbols are
   * unaffected. Explicit prefix forms (upper_X, 大写X, 小写X) have
   * case_explicit=true and are already resolved by parsePasswordToken. */
  if (!token->case_explicit && ch >= 'a' && ch <= 'z' &&
      context_.password_mode == PasswordMode::Upper)
    ch = static_cast<char>(ch - 'a' + 'A');
  password.push_back(ch);
  repeat("已输入第" + spokenCount(password.size()) + "个字符");
}

void Controller::handlePasswordControl(PasswordControl control) {
  auto& password = context_.password;
  switch (control) {
    case PasswordControl::Finish:
      finishPasswordRound();
      break;
    case PasswordControl::DeleteLast:
      if (!password.empty()) {
        password.back() = 0;
        password.pop_back();
      }
      repeat("当前长度为" + spokenCount(password.size()));
      break;
    case PasswordControl::Clear:
      secureWipe(password);
      context_.password_mode = PasswordMode::Lower;
      repeat("已清空，请重新输入密码，默认小写");
      break;
    case PasswordControl::Restart:
      wipePassword();
      context_.state = State::EnteringPassword;
      context_.password_mode = PasswordMode::Lower;
      repeat("已重新开始，请输入密码，默认小写");
      break;
    case PasswordControl::Cancel:
      setTerminal(State::Idle, "已取消配置");
      break;
    case PasswordControl::ReportLength:
      repeat("当前长度为" + spokenCount(password.size()));
      break;
    case PasswordControl::SwitchUpper:
      context_.password_mode = PasswordMode::Upper;
      repeat("已切换到大写模式");
      break;
    case PasswordControl::SwitchLower:
      context_.password_mode = PasswordMode::Lower;
      repeat("已切换到小写模式");
      break;
  }
}

void Controller::finishPasswordRound() {
  auto& password = context_.password;
  if (password.size() < config_.min_password_length) {
    repeat("密码长度不足，请继续输入");
    return;
  }
  context_.state = State::ConfirmingPassword;
  context_.grammar = Grammar::Confirmation;
  repeat("密码共" + spokenCount(password.size()) +
         "个字符，确认无误请说确认提交，修改请说重新输入");
}

void Controller::handlePasswordConfirmation(const std::string& text) {
  if (isCancel(text)) {
    setTerminal(State::Idle, "已取消配置");
    return;
  }
  if (isPasswordRetry(text)) {
    wipePassword();
    context_.state = State::EnteringPassword;
    context_.grammar = Grammar::PasswordToken;
    context_.password_mode = PasswordMode::Lower;
    repeat("已清空，请重新输入密码，默认小写");
    return;
  }
  if (!isSubmitConfirmation(text)) {
    repeat("尚未提交，确认请说确认提交，修改请说重新输入");
    return;
  }
  const char* password = context_.password.empty() ? nullptr : context_.password.data();
  tryConnect(password, context_.password.size());
}

void Controller::tryConnect(const char* password, std::size_t password_len) {
  if (!context_.selected_index ||
      *context_.selected_index >= context_.candidates.size()) {
    setTerminal(State::Error, "连接失败，已停止当前流程");
    return;
  }
  const auto& ap = context_.candidates[*context_.selected_index];
  started_ms_ = clock_.nowMs();
  const auto result = wifi_.beginConnect(ap.ssid, password, password_len);
  wipePassword();
  request_id_ = result.request_id;
  if (!request_id_) {
    setTerminal(State::Error, "连接接口未提供事务编号");
    return;
  }
  applyConnectResult(result);
}

void Controller::cancelPending() {
  const auto id = request_id_;
  request_id_ = 0;
  if (id) wifi_.cancelConnect(id);
}

void Controller::cancel() {
  cancelPending();
  cancelScanPending();
  setTerminal(State::Idle, "已取消配置");
}

void Controller::poll() {
  if (scan_request_id_) {
    const auto now = clock_.nowMs();
    if (now < scan_started_ms_ || now - scan_started_ms_ >= config_.scan_timeout_ms) {
      cancelScanPending();
      applyScanResult({ScanStatus::Timeout, {}, 0, 0});
      return;
    }
    ScanResult scan;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try { scan = wifi_.pollScan(scan_request_id_); }
    catch (...) {
      cancelScanPending();
      setTerminal(State::Error, "扫描失败，已停止当前流程");
      return;
    }
#else
    scan = wifi_.pollScan(scan_request_id_);
#endif
    if (scan.request_id == scan_request_id_) applyScanResult(std::move(scan));
    return;
  }
  if (!request_id_) return;
  // Timeout wins at the exact deadline, including a simultaneously ready event.
  if (clock_.nowMs() - started_ms_ >= config_.connect_timeout_ms) {
    const auto id = request_id_;
    cancelPending();
    applyConnectResult({ConnectStatus::Timeout, 0, id, {}});
    return;
  }
  auto result = wifi_.pollConnect(request_id_);
  if (result.request_id != request_id_) return; // stale/foreign event
  applyConnectResult(result);
}

namespace {
bool usableIpv4(const std::string& ip) {
  unsigned octets[4]{};
  std::size_t pos = 0;
  for (unsigned i = 0; i < 4; ++i) {
    std::size_t digits = 0;
    while (pos < ip.size() && ip[pos] >= '0' && ip[pos] <= '9') {
      if (++digits > 3) return false;
      octets[i] = octets[i] * 10 + static_cast<unsigned>(ip[pos++] - '0');
    }
    if (!digits || octets[i] > 255) return false;
    if (i < 3 && (pos == ip.size() || ip[pos++] != '.')) return false;
  }
  return pos == ip.size() && octets[0] != 0 && octets[0] != 127 &&
         octets[0] < 224 && !(octets[0] == 169 && octets[1] == 254);
}
}

void Controller::applyConnectResult(const ConnectResult& result) {
  if (result.status == ConnectStatus::CredentialsReceived) {
    if (context_.state != State::ConnectingWifi &&
        context_.state != State::CredentialsReceived) {
      context_.state = State::CredentialsReceived;
      repeat("已收到凭据，等待联网结果");
    }
    return;
  }
  if (result.status == ConnectStatus::Connecting) {
    if (context_.state != State::ConnectingWifi) {
      context_.state = State::ConnectingWifi;
      repeat("正在连接网络");
    }
    return;
  }
  if (result.status == ConnectStatus::IpReady && usableIpv4(result.ipv4)) {
    request_id_ = 0; // provider retains ownership of the established network
    setTerminal(State::Completed, "已连接到" +
                spokenSsid(context_.candidates[*context_.selected_index].ssid));
    return;
  }
  cancelPending(); // terminal failures must release provider-side state
  wipePassword();
  context_.password_mode = PasswordMode::Lower;
  const bool reselect = result.status == ConnectStatus::Unsupported ||
                        result.status == ConnectStatus::NetworkNotFound;
  const bool secured = !reselect && context_.selected_index &&
      context_.candidates[*context_.selected_index].secured;
  if (reselect) context_.selected_index.reset();
  context_.state = secured ? State::EnteringPassword : State::WaitingNetworkChoice;
  context_.grammar = secured ? Grammar::PasswordToken : Grammar::NetworkSelection;
  switch (result.status) {
    case ConnectStatus::WrongPassword: repeat("密码错误，请重新输入"); break;
    case ConnectStatus::Timeout: repeat("连接超时，请重试"); break;
    case ConnectStatus::Busy: repeat("网络服务忙，请稍后重试"); break;
    case ConnectStatus::Unsupported: repeat("暂不支持该网络，请重新选择网络"); break;
    case ConnectStatus::NetworkNotFound: repeat("找不到该网络，请重新选择网络"); break;
    case ConnectStatus::Cancelled: setTerminal(State::Idle, "已取消配置"); break;
    default: repeat("连接失败，请重试"); break;
  }
}

void Controller::repeat(const std::string& text) {
  bool spoken = false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  try {
    tts_.stop();
    spoken = tts_.speak(text);
  } catch (...) { spoken = false; }
#else
  tts_.stop();
  spoken = tts_.speak(text);
#endif
  if (!spoken) {
    cancelPending();
    cancelScanPending();
    wipePassword();
    context_.state = State::Error;
  }
}

void Controller::wipePassword() { secureWipe(context_.password); }

void Controller::setTerminal(State state, const std::string& text) {
  wipePassword();
  context_.state = state;
  repeat(text);
}

}  // namespace voicelink
