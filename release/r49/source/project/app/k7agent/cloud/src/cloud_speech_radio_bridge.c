#include "cloud_speech_radio_bridge.h"

#include <string.h>

static bool valid_ipv4(const uint8_t ip[4])
{
  return ip[0] > 0 && ip[0] < 224 && ip[0] != 127 &&
         !(ip[0] == 169 && ip[1] == 254);
}

static int fail_closed(struct cloud_speech_radio_bridge *bridge, int error)
{
  (void)cloud_speech_set_network(bridge->orchestrator, false, false);
  bridge->wifi_connected = false;
  bridge->ipv4_ready = false;
  memset(bridge->ipv4, 0, sizeof(bridge->ipv4));
  return error;
}

int cloud_speech_radio_bridge_init(
  struct cloud_speech_radio_bridge *bridge,
  struct cloud_speech_orchestrator *orchestrator)
{
  if (!bridge || !orchestrator) return CLOUD_SPEECH_EINVAL;
  memset(bridge, 0, sizeof(*bridge));
  bridge->orchestrator = orchestrator;
  return CLOUD_SPEECH_OK;
}

int cloud_speech_radio_bridge_apply(
  struct cloud_speech_radio_bridge *bridge,
  const struct cloud_speech_radio_snapshot *snapshot)
{
  if (!bridge || !bridge->orchestrator || !snapshot)
    return CLOUD_SPEECH_EINVAL;
  if ((snapshot->ipv4_ready && !snapshot->wifi_connected) ||
      (snapshot->ipv4_ready && !valid_ipv4(snapshot->ipv4)))
    return fail_closed(bridge, CLOUD_SPEECH_EINVAL);
  if (bridge->observed && snapshot->generation < bridge->generation)
    return fail_closed(bridge, CLOUD_SPEECH_EINVAL);
  if (bridge->observed && snapshot->generation == bridge->generation &&
      (snapshot->wifi_connected != bridge->wifi_connected ||
       snapshot->ipv4_ready != bridge->ipv4_ready ||
       memcmp(snapshot->ipv4, bridge->ipv4, sizeof(bridge->ipv4))))
    return fail_closed(bridge, CLOUD_SPEECH_EINVAL);

  /* Polling may miss the entire disconnected interval. A changed generation
   * invalidates every observation from the previous connection, even when
   * DHCP assigns the same address again. */
  if (!bridge->observed || snapshot->generation != bridge->generation)
    (void)cloud_speech_set_network(bridge->orchestrator, false, false);

  bridge->observed = true;
  bridge->generation = snapshot->generation;
  bridge->wifi_connected = snapshot->wifi_connected;
  bridge->ipv4_ready = snapshot->ipv4_ready;
  memcpy(bridge->ipv4, snapshot->ipv4, sizeof(bridge->ipv4));
  return cloud_speech_set_network(bridge->orchestrator,
                                  snapshot->wifi_connected,
                                  snapshot->ipv4_ready);
}
