#ifndef K7_RADIO_SERVICE_H
#define K7_RADIO_SERVICE_H
#include <stdbool.h>
#include <stdint.h>
#include "wifi_scan.h"
/* Returned after explicit wifi-service-start succeeds; no backend fallback.
 * Returned service lives for the process lifetime. Never mutate its fields. */
struct k7wd_dispatch *k7_wifi_dispatch(void);
struct ws_service *k7_wifi_scans(void);

/* Read-only, lock-consistent cloud handoff.  generation changes whenever a
 * DHCP-backed link is published or withdrawn.  The current implementation is
 * deliberately conservative: wifi_connected is true only while the native
 * worker is alive with a validated DHCP IPv4 lease. */
struct k7radio_link_snapshot {
  uint64_t generation;
  bool wifi_connected;
  bool ipv4_ready;
  uint8_t ipv4[4];
};

int k7radio_get_link_snapshot(struct k7radio_link_snapshot *snapshot);
#endif
