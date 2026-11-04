#ifndef CLOUD_SPEECH_MIMO_ADAPTER_H
#define CLOUD_SPEECH_MIMO_ADAPTER_H

#include "cloud_speech_orchestrator.h"
#include "mimo_cloud_client.h"

struct cloud_speech_mimo_adapter {
  struct mimo_cloud_client *mimo;
  const int16_t *network_connected_pcm16;
  size_t network_connected_frames;
};

/* Produces operations for the existing non-streaming MiMo client and the
 * 16 kHz mono k7sound playback API. The fixed prompt is caller-owned data. */
int cloud_speech_mimo_adapter_ops(struct cloud_speech_ops *ops);

#endif
