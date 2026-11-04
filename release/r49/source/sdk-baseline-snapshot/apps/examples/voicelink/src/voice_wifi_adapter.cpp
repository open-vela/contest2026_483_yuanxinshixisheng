#include "voice_wifi_adapter.hpp"
#include <cstdio>
namespace voicelink {
std::optional<SharedWifiPort> nativeWifiPort(std::uint64_t timeout_ms) noexcept {
 auto *d=k7_wifi_dispatch(); auto *s=k7_wifi_scans();
 if(!timeout_ms || !d || !s || s->d!=d || d->scan!=s) return std::nullopt;
 return std::optional<SharedWifiPort>(std::in_place,*d,*s,timeout_ms);
}

// Validate UTF-8 without rewriting original SSID bytes.
static bool text_ssid(const uint8_t* p,unsigned n) {
 for(unsigned i=0;i<n;) {
  unsigned c=p[i++],k,lo=0x80,hi=0xbf;
  if(c<0x80) {if(c<32 || c==127)return false;continue;}
  if(c>=0xc2&&c<=0xdf)k=1;
  else if(c>=0xe0&&c<=0xef) {k=2;if(c==0xe0)lo=0xa0;if(c==0xed)hi=0x9f;}
  else if(c>=0xf0&&c<=0xf4) {k=3;if(c==0xf0)lo=0x90;if(c==0xf4)hi=0x8f;}
  else return false;
  if(k>n-i || p[i]<lo || p[i]>hi)return false;
  ++i;while(--k) {if(p[i]<0x80||p[i]>0xbf)return false;++i;}
 }
 return true;
}
static bool busy(k7wd_rc r) { return r==WD_BUSY || r==WD_FULL; }
ScanResult SharedWifiPort::beginScan() {
 ScanResult r; if(s_.d!=&d_) {r.error_code=-22;return r;}
 auto rc=ws_voice_begin(&s_,timeout_,&r.request_id);
 r.status=rc==WD_OK?ScanStatus::Received:busy(rc)?ScanStatus::Busy:ScanStatus::Failed;
 r.error_code=rc==WD_OK?0:-int(rc); return r;
}
ScanResult SharedWifiPort::pollScan(std::uint64_t id) {
 ScanResult r; r.request_id=id; vs_snapshot v{};
 auto rc=ws_voice_poll(&s_,id,&v);
 if(rc!=WD_OK) {r.error_code=-int(rc);return r;}
 r.error_code=v.error;
 switch(v.state) {
 case VS_PENDING:r.status=v.held?ScanStatus::Scanning:ScanStatus::Received;break;
 case VS_BUSY:r.status=ScanStatus::Busy;break;
 case VS_UNSUPPORTED:r.status=ScanStatus::Unsupported;break;
 case VS_CANCELLED:r.status=ScanStatus::Cancelled;break;
 case VS_TIMEOUT:r.status=ScanStatus::Timeout;break;
 case VS_DONE:
  if(v.held || v.generation!=id || v.count>64) {r.error_code=-22;break;}
  // Preserve SSID bytes exactly: no normalization, truncation or BSSID merging.
  // Hidden/embedded-NUL names cannot be safely represented by current selection path.
  for(unsigned i=0;i<v.count;i++) {
   const auto& a=v.ap[i]; if(!a.ssid_len) continue;
   if(a.ssid_len>32 || a.security>3) {r.access_points.clear();r.error_code=-22;return r;}
   if(!text_ssid(a.ssid,a.ssid_len)) {
    continue;
   }
   r.access_points.push_back({std::string(reinterpret_cast<const char*>(a.ssid),a.ssid_len),a.rssi,a.security!=0});
  }
  r.status=ScanStatus::Ready;break;
 default:break;
 }
 return r;
}
void SharedWifiPort::cancelScan(std::uint64_t id) noexcept {(void)ws_voice_cancel(&s_,id);}
ConnectResult SharedWifiPort::beginConnect(const std::string& ssid,const char* p,std::size_t n) noexcept {
 ConnectResult r; auto rc=k7wd_voice_begin(&d_,ssid.data(),ssid.size(),p,n,timeout_,&r.request_id);
 r.status=rc==WD_OK?ConnectStatus::CredentialsReceived:busy(rc)?ConnectStatus::Busy:ConnectStatus::Failed;
 r.error_code=rc==WD_OK?0:-int(rc);return r;
}
ConnectResult SharedWifiPort::pollConnect(std::uint64_t id) noexcept {
 ConnectResult r; r.request_id=id; k7wd_view v{};
 auto rc=k7wd_voice_poll(&d_,id,&v); if(rc!=WD_OK) {r.error_code=-int(rc);return r;}
 switch(v.status) {
 case WB_RECEIVED:r.status=ConnectStatus::CredentialsReceived;break;
 case WB_CONNECTING:r.status=ConnectStatus::Connecting;break;
 case WB_IP_READY:
  if(!v.held || !v.ip[0] || v.ip[0]==127 || v.ip[0]>=224 || (v.ip[0]==169&&v.ip[1]==254)) {r.error_code=-22;break;}
#if defined(__cpp_exceptions)
  try {
#endif
   char ip[16];std::snprintf(ip,sizeof(ip),"%u.%u.%u.%u",v.ip[0],v.ip[1],v.ip[2],v.ip[3]);
   r.ipv4=ip;r.status=ConnectStatus::IpReady;
#if defined(__cpp_exceptions)
  } catch(...) {r.error_code=-12;}
#endif
  break;
 case WB_CANCELLED:r.status=ConnectStatus::Cancelled;break;
 case WB_TIMEOUT:r.status=ConnectStatus::Timeout;break;
 case WB_BUSY:r.status=ConnectStatus::Busy;break;
 case WB_UNSUPPORTED:r.status=ConnectStatus::Unsupported;break;
 default:r.error_code=-int(v.status);break;
 }
 return r;
}
void SharedWifiPort::cancelConnect(std::uint64_t id) noexcept {(void)k7wd_voice_cancel(&d_,id);}
}
