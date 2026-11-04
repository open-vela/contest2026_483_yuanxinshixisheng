#ifndef CLOUD_SPEECH_RADIO_BRIDGE_H
#define CLOUD_SPEECH_RADIO_BRIDGE_H

#include "cloud_speech_orchestrator.h"

#include <stdbool.h>
#include <stdint.h>

struct cloud_speech_radio_snapshot {
  uint64_t generation;
  bool wifi_connected;
  bool ipv4_ready;
  uint8_t ipv4[4];
};

struct cloud_speech_radio_bridge {
  struct cloud_speech_orchestrator *orchestrator;
  uint64_t generation;
  bool observed;
  bool wifi_connected;
  bool ipv4_ready;
  uint8_t ipv4[4];
};

int cloud_speech_radio_bridge_init(
  struct cloud_speech_radio_bridge *bridge,
  struct cloud_speech_orchestrator *orchestrator);

/* Apply one authoritative radio snapshot in the cloud worker task.  This
 * function updates only Wi-Fi/IPv4.  DNS, entropy, time, CA, TCP and API gates
 * remain closed until their independent live checks pass. */
int cloud_speech_radio_bridge_apply(
  struct cloud_speech_radio_bridge *bridge,
  const struct cloud_speech_radio_snapshot *snapshot);

#ifdef K7CLOUD_RADIO_READINESS
/* Fetches the public, locked k7radio snapshot and applies it. */
int cloud_speech_radio_bridge_sync(struct cloud_speech_radio_bridge *bridge);
#endif

#endif
