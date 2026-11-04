/* Local GUI handoff. Credentials never appear in command arguments or output. */
#pragma once
#include <termios.h>
#include <poll.h>

static int run_ui_connect()
{
  auto wifi = voicelink::nativeWifiPort();
  if (!wifi) return 1;
  struct termios saved{}, quiet{};
  if (tcgetattr(0, &saved)) return 1;
  quiet = saved;
  quiet.c_lflag &= ~(ECHO | ICANON);
  quiet.c_cc[VMIN] = 0; quiet.c_cc[VTIME] = 1;
  if (tcsetattr(0, TCSANOW, &quiet)) return 1;
  struct InputGuard {
    struct termios saved;
    char password[64]{};
    ~InputGuard() {
      volatile char *p = password;
      for (unsigned i = 0; i < sizeof(password); ++i) p[i] = 0;
      tcflush(0, TCIFLUSH);
      tcsetattr(0, TCSANOW, &saved);
    }
  } guard{saved, {}};
  auto line = [](char *dest, size_t capacity) {
    uint64_t start = 0, now = 0;
    if (!now_ms(start)) return false;
    size_t used = 0;
    while (now_ms(now) && now >= start && now - start < 30000) {
      struct pollfd fd{0, POLLIN, 0};
      if (poll(&fd, 1, 100) <= 0) continue;
      char c;
      if (read(0, &c, 1) != 1) continue;
      if (c == '\n') { dest[used] = 0; return true; }
      if (c < 32 || c > 126 || used + 1 >= capacity) return false;
      dest[used++] = c;
    }
    return false;
  };
  std::puts("VOICE_UI INPUT_READY echo=off format=ssid_hex,password");
  std::fflush(stdout);
  char hex[65]{};
  if (!line(hex, sizeof(hex)) || !line(guard.password, sizeof(guard.password))) {
    std::puts("VOICE_UI failed input"); return 1;
  }
  auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
                                  c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  size_t length = std::strlen(hex), password_length = std::strlen(guard.password);
  if (!length || length % 2 || (password_length && password_length < 8)) {
    std::puts("VOICE_UI failed validation"); return 1;
  }
  std::string ssid;
  for (size_t i = 0; i < length; i += 2) {
    int a = nibble(hex[i]), b = nibble(hex[i + 1]);
    if (a < 0 || b < 0 || !(a * 16 + b)) { std::puts("VOICE_UI failed ssid"); return 1; }
    ssid.push_back(char(a * 16 + b));
  }
  auto result = wifi->beginConnect(ssid, guard.password, password_length);
  volatile char *secret = guard.password;
  for (unsigned i = 0; i < sizeof(guard.password); ++i) secret[i] = 0;
  uint64_t start = 0, now = 0;
  const uint64_t id = result.request_id;
  if (!now_ms(start)) { if (id) wifi->cancelConnect(id); return 1; }
  std::puts("VOICE_UI connecting"); std::fflush(stdout);
  while (result.status == voicelink::ConnectStatus::CredentialsReceived ||
         result.status == voicelink::ConnectStatus::Connecting) {
    if (!now_ms(now) || now < start || now - start >= 45000) {
      wifi->cancelConnect(id); std::puts("VOICE_UI failed timeout"); return 1;
    }
    usleep(10000);
    result = wifi->pollConnect(id);
  }
  if (result.status == voicelink::ConnectStatus::IpReady && !result.ipv4.empty()) {
    std::printf("VOICE_UI connected ip=%s\n", result.ipv4.c_str()); return 0;
  }
  if (id) wifi->cancelConnect(id);
  std::printf("VOICE_UI failed status=%u error=%d\n", unsigned(result.status), result.error_code);
  return 1;
}
