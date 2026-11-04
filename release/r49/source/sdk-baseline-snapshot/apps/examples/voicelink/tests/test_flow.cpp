#include "voicelink/controller.hpp"
#include "voicelink/parsers.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace voicelink;

struct Clock final : ClockPort {
  std::uint64_t ms{};
  std::uint64_t nowMs() const noexcept override { return ms; }
};

struct Speaker final : TtsPort {
  std::vector<std::string> messages;
  bool speak(const std::string& text) override { messages.push_back(text); return true; }
  void stop() override {}
  bool saidConnected() const {
    for (const auto& s : messages) if (s.find("已连接到") != std::string::npos) return true;
    return false;
  }
};

// Explicit simulation: no radio, model, microphone or speaker is accessed.
struct Wifi final : WifiPort {
  unsigned scans{}, scanCancellations{}, submissions{}, cancellations{};
  bool scanFails{}, verifiedCredentials{};
  std::string submittedSsid;
  std::size_t submittedLength{};
  ScanResult scanResult{ScanStatus::Ready,
      {{"Lab", -70, true}, {"VoiceTest", -30, true}, {"VoiceTest", -60, true}}, 0, 501};
  ConnectResult result{ConnectStatus::CredentialsReceived, 0, 701, {}};
  ScanResult beginScan() override {
    ++scans;
    if (scanFails) return {ScanStatus::Failed, {}, -38, 500 + scans};
    return {ScanStatus::Received, {}, 0, 500 + scans};
  }
  ScanResult pollScan(std::uint64_t) override { return scanResult; }
  void cancelScan(std::uint64_t) noexcept override { ++scanCancellations; }
  ConnectResult beginConnect(const std::string& ssid, const char* password,
                             std::size_t length) noexcept override {
    ++submissions;
    submittedSsid = ssid;
    submittedLength = length;
    verifiedCredentials = ssid == "VoiceTest" && length == 8 && password != nullptr;
    const char expected[] = "abcd1234"; // public synthetic fixture, never real credentials
    for (std::size_t i = 0; verifiedCredentials && i < length; ++i)
      verifiedCredentials = password[i] == expected[i];
    return result;
  }
  ConnectResult pollConnect(std::uint64_t) noexcept override { return result; }
  void cancelConnect(std::uint64_t) noexcept override { ++cancellations; }
};

struct Fixture {
  Speaker speaker;
  Wifi wifi;
  Clock clock;
  Controller ctl{speaker, wifi, clock};
  void choose() {
    ctl.start();
    ctl.ingest("普通对话");
    assert(wifi.scans == 0);
    ctl.ingest("开始联网");
    assert(ctl.context().state == State::ScanningWifi);
    ctl.poll();
    assert(ctl.context().state == State::WaitingNetworkChoice);
    assert(ctl.context().candidates.size() == 2);
    assert(ctl.context().candidates[0].ssid == "VoiceTest");
    ctl.ingest("网络一");
    assert(ctl.context().state == State::EnteringPassword);
  }
  void submit() {
    choose();
    for (const auto* token : {"a", "b", "c", "d", "一", "二", "三", "四"}) ctl.ingest(token);
    ctl.ingest("完成");
    assert(ctl.context().state == State::ConfirmingPassword);
    assert(wifi.submissions == 0);
    ctl.ingest("随便提交");
    assert(ctl.context().state == State::ConfirmingPassword);
    assert(wifi.submissions == 0);
    ctl.ingest("确认提交");
    assert(wifi.verifiedCredentials);
    assert(ctl.context().password.empty());
    assert(ctl.context().state == State::CredentialsReceived);
    assert(!speaker.saidConnected());
  }
};

int main() {
  {
    Fixture f;
    f.ctl.start();
    f.ctl.ingest("你好联网");
    assert(f.wifi.scans == 1 && f.wifi.submissions == 0);
    f.ctl.poll();
    assert(f.ctl.context().state == State::WaitingNetworkChoice);
    f.ctl.ingest("你好网络一");
    assert(f.ctl.context().state == State::WaitingNetworkChoice);
    f.ctl.ingest("网络一");
    f.ctl.ingest("你好");
    assert(f.ctl.context().state == State::EnteringPassword);
    assert(f.ctl.context().password.empty() && f.wifi.submissions == 0);
  }
  {
    const auto& wake_words = defaultWakeWords();
    assert(isWakeWord("开始联网", wake_words));
    assert(isWakeWord("开始联网。", wake_words));
    assert(isWakeWord("你好，OpenVela", wake_words));
    assert(isWakeWord("你好 open", wake_words));
    assert(isWakeWord("你好联网", wake_words));
    assert(!isWakeWord("不要你好", wake_words));
    assert(!isWakeWord("关闭了", wake_words));
    assert(!isWakeWord("不要开始联网", wake_words));
    assert(!isWakeWord("开始联网123", wake_words));
    assert(!isWakeWord("开始联网开始联网", wake_words));
    assert(!isWakeWord("你好open", std::string("开始联网")));
    assert(!isWakeWord("open", wake_words));
    assert(isWakeWord("你好", wake_words));
    assert(isWakeWord("你好 open 123", wake_words));
  }
  {
    Fixture f;
    f.choose();
    for (const auto* token : {"a", "b", "c", "d", "一", "二", "三", "四"}) f.ctl.ingest(token);
    f.ctl.ingest("完成");
    f.ctl.ingest("否");
    assert(f.ctl.context().state == State::EnteringPassword);
    assert(f.ctl.context().password.empty());
    assert(f.wifi.submissions == 0);
  }
  {
    Fixture f;
    f.wifi.scanResult = {ScanStatus::Ready, {{"OpenGuest", -10, false}}, 0, 501};
    f.ctl.start();
    f.ctl.ingest("开始联网");
    f.ctl.poll();
    f.ctl.ingest("网络一");
    assert(f.ctl.context().state == State::ConfirmingPassword);
    assert(f.wifi.submissions == 0);
    f.ctl.ingest("确认");
    assert(f.wifi.submissions == 1);
    assert(f.wifi.submittedSsid == "OpenGuest");
    assert(f.wifi.submittedLength == 0);
  }
  {
    assert(isSubmitConfirmation("确认"));
    assert(isSubmitConfirmation("确认提交"));
    assert(!isSubmitConfirmation("随便提交"));
    assert(isPasswordRetry("否"));
    assert(isPasswordRetry("不确认"));
    assert(isPasswordRetry("重新输入"));
  }
  {
    Fixture f;
    f.submit();
    f.wifi.result = {ConnectStatus::Connecting, 0, 701, {}};
    f.ctl.poll();
    assert(f.ctl.context().state == State::ConnectingWifi);
    assert(!f.speaker.saidConnected());
    f.wifi.result = {ConnectStatus::IpReady, 0, 999, "192.0.2.9"};
    f.ctl.poll();
    assert(!f.speaker.saidConnected()); // a different transaction cannot complete us
    f.wifi.result.request_id = 701;
    f.ctl.poll();
    assert(f.ctl.context().state == State::Completed);
    assert(f.speaker.saidConnected());
    assert(f.wifi.cancellations == 0); // established network stays owned by provider
    for (const auto& text : f.speaker.messages) assert(text.find("abcd1234") == std::string::npos);
  }
  {
    Fixture f;
    f.submit();
    f.wifi.result = {ConnectStatus::WrongPassword, -1, 701, {}};
    f.ctl.poll();
    assert(f.ctl.context().state == State::EnteringPassword);
    assert(f.ctl.context().password.empty());
    assert(!f.speaker.saidConnected());
  }
  for (const auto failure : {ConnectStatus::Unsupported, ConnectStatus::NetworkNotFound}) {
    Fixture f;
    f.submit();
    f.wifi.result = {failure, -1, 701, {}};
    f.ctl.poll();
    assert(f.ctl.context().state == State::WaitingNetworkChoice);
    assert(f.ctl.context().grammar == Grammar::NetworkSelection);
    assert(!f.ctl.context().selected_index);
    f.ctl.ingest("网络二");
    assert(f.ctl.context().state == State::EnteringPassword);
    assert(*f.ctl.context().selected_index == 1);
  }
  {
    Fixture f;
    f.submit();
    f.clock.ms = 30000;
    f.wifi.result = {ConnectStatus::IpReady, 0, 701, "192.0.2.9"};
    f.ctl.poll();
    assert(!f.speaker.saidConnected()); // deadline beats simultaneously ready event
    assert(f.wifi.cancellations == 1);
  }
  {
    Fixture f;
    f.submit();
    f.ctl.ingest("取消");
    f.wifi.result = {ConnectStatus::IpReady, 0, 701, "192.0.2.9"};
    f.ctl.poll();
    assert(f.ctl.context().state == State::Idle);
    assert(!f.speaker.saidConnected());
    assert(f.wifi.cancellations == 1);
  }
  {
    Fixture f;
    f.wifi.scanFails = true;
    f.ctl.start();
    f.ctl.ingest("开始联网");
    assert(f.ctl.context().state == State::Error);
    assert(f.wifi.submissions == 0);
    assert(!f.speaker.saidConnected());
  }
  {
    Fixture f;
    f.ctl.start();
    f.ctl.ingest("开始联网");
    f.wifi.scanResult = {ScanStatus::Scanning, {}, 0, 501};
    f.ctl.poll();
    assert(f.ctl.context().state == State::ScanningWifi);
    f.wifi.scanResult = {ScanStatus::Ready, {{"Foreign", -1, false}}, 0, 502};
    f.ctl.poll();
    assert(f.ctl.context().state == State::ScanningWifi);
    assert(f.ctl.context().candidates.empty());
  }
  {
    Fixture f;
    f.ctl.start();
    f.ctl.ingest("开始联网");
    f.clock.ms = 30000;
    f.ctl.poll();
    assert(f.ctl.context().state == State::Idle);
    assert(f.wifi.scanCancellations == 1);
    assert(f.ctl.context().candidates.empty());
  }
  {
    Fixture f;
    f.ctl.start();
    f.ctl.ingest("开始联网");
    f.ctl.ingest("取消");
    f.ctl.poll(); // a late Ready snapshot is not consumed after cancel
    assert(f.ctl.context().state == State::Idle);
    assert(f.wifi.scanCancellations == 1);
    assert(f.wifi.submissions == 0);
  }
  for (const auto status : {ScanStatus::Busy, ScanStatus::Unsupported}) {
    Fixture f;
    f.ctl.start();
    f.ctl.ingest("开始联网");
    f.wifi.scanResult = {status, {}, -1, 501};
    f.ctl.poll();
    assert(f.ctl.context().state == State::Idle);
    assert(f.ctl.context().candidates.empty());
    assert(f.wifi.submissions == 0);
    assert(f.wifi.cancellations == 0); // scan failure does not cancel a connection
    f.ctl.ingest("开始联网");
    assert(f.ctl.context().state == State::ScanningWifi);
    assert(f.wifi.scans == 2); // new wake works after the recoverable rejection
  }
  {
    Fixture f;
    f.ctl.start();
    f.ctl.ingest("开始联网");
    f.wifi.scanResult.access_points.resize(65);
    f.ctl.poll();
    assert(f.ctl.context().state == State::Error);
    assert(f.wifi.scanCancellations == 1);
  }
  {
    Fixture f;
    f.ctl.start();
    f.ctl.ingest("开始联网");
    f.wifi.scanResult.access_points.clear();
    f.ctl.poll();
    assert(f.ctl.context().state == State::WaitingNetworkChoice);
    assert(f.ctl.context().candidates.empty());
    assert(f.wifi.scanCancellations == 0);
  }
  {
    for (const auto* text : {"反斜杠", "反斜线", "\\"}) {
      const auto token = parsePasswordToken(text);
      assert(token && token->character == '\\' && !token->control);
    }
    for (const auto* text : {"正斜杠", "斜杠", "/"}) {
      const auto token = parsePasswordToken(text);
      assert(token && token->character == '/' && !token->control);
    }
    assert(!parsePasswordToken("反斜杠艾特")); // one character per utterance
  }
  {
    Fixture f;
    f.choose();
    for (const auto* token : {"大写A", "反斜杠", "艾特", "正斜杠"}) f.ctl.ingest(token);
    const std::vector<char> expected{'A', '\\', '@', '/'}; // public fixture
    assert(f.ctl.context().password == expected);
    assert(f.wifi.submissions == 0);
    f.ctl.ingest("取消");
    assert(f.ctl.context().password.empty());
    assert(!f.speaker.saidConnected());
  }
  std::cout << "20 text-flow scenarios passed; all audio/network ports simulated\n";
}
