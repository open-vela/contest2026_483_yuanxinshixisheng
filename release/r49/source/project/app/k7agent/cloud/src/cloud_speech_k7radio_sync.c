#include "cloud_speech_radio_bridge.h"
#include "k7_radio_service.h"

int cloud_speech_radio_bridge_sync(struct cloud_speech_radio_bridge *bridge)
{
  struct k7radio_link_snapshot radio;
  struct cloud_speech_radio_snapshot snapshot;
  int rc;

  if (!bridge || !bridge->orchestrator) return CLOUD_SPEECH_EINVAL;
  rc = k7radio_get_link_snapshot(&radio);
  if (rc)
    {
      (void)cloud_speech_set_network(bridge->orchestrator, false, false);
      return CLOUD_SPEECH_ENOTREADY;
    }
  snapshot.generation = radio.generation;
  snapshot.wifi_connected = radio.wifi_connected;
  snapshot.ipv4_ready = radio.ipv4_ready;
  snapshot.ipv4[0] = radio.ipv4[0];
  snapshot.ipv4[1] = radio.ipv4[1];
  snapshot.ipv4[2] = radio.ipv4[2];
  snapshot.ipv4[3] = radio.ipv4[3];
  return cloud_speech_radio_bridge_apply(bridge, &snapshot);
}
