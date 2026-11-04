/* Interface proposal only: intentionally NO implementation or hardware calls. */
#ifndef VOICE_WIFI_SCAN_CONTRACT_H
#define VOICE_WIFI_SCAN_CONTRACT_H
#include <stdint.h>
enum vs_state { VS_PENDING, VS_DONE, VS_BUSY, VS_UNSUPPORTED,
                VS_FAILED, VS_CANCELLED, VS_TIMEOUT, VS_NOT_READY };
struct vs_ap {
  uint8_t ssid[32], ssid_len, bssid[6], security, band, channel;
  int16_t rssi;
};
struct vs_snapshot {
  uint64_t ticket, generation;
  enum vs_state state;
  int error;
  uint8_t count, truncated, held;
  struct vs_ap ap[64];
};
/* Future service-owned API. Trusted entry chooses VOICE identity internally.
 * No password, owner argument, SSID text parsing or DeviceEvent input.
 * begin is nonblocking; backend runs on dedicated worker after atomic lease.
 * poll copies one immutable complete snapshot, never shared RX memory.
 * ticket identity and generation come only from trusted service.
 * held means radio cleanup still unproven, even after timeout/cancellation.
 * These declarations must NOT be wired until shared SCAN arbitration exists. */
int voice_scan_begin(uint64_t *ticket);
int voice_scan_poll(uint64_t ticket, struct vs_snapshot *out);
int voice_scan_cancel(uint64_t ticket);
#endif
