#pragma once
#include "voicelink/ports.hpp"
extern "C" {
#include "wifi_scan.h"
#include "k7_radio_service.h"
}
namespace voicelink {
// Service and dispatcher outlive this object. No pump, hardware or global BLE access.
class SharedWifiPort final : public WifiPort {
 public:
  SharedWifiPort(k7wd_dispatch& d, ws_service& s, std::uint64_t timeout_ms=30000) noexcept
    : d_(d), s_(s), timeout_(timeout_ms) {}
  ScanResult beginScan() override;
  ScanResult pollScan(std::uint64_t) override;
  void cancelScan(std::uint64_t) noexcept override;
  ConnectResult beginConnect(const std::string&,const char*,std::size_t) noexcept override;
  ConnectResult pollConnect(std::uint64_t) noexcept override;
  void cancelConnect(std::uint64_t) noexcept override;
 private:
  k7wd_dispatch& d_; ws_service& s_; std::uint64_t timeout_;
};
// Nullopt until the real process-lifetime radio service is published.
// Caller owns this optional; create once on the serial Controller task.
std::optional<SharedWifiPort> nativeWifiPort(std::uint64_t timeout_ms=30000) noexcept;
}
