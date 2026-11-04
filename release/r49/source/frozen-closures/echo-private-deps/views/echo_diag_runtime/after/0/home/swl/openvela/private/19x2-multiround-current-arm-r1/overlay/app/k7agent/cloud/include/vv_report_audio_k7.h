#ifndef VV_REPORT_AUDIO_K7_H
#define VV_REPORT_AUDIO_K7_H
#include "vv_report_audio.h"
#include "vv_native_client.h"
struct vv_report_audio_k7 {
  struct vv_native_client *client;
  bool (*ready)(void *);
  void *ready_arg;
};
/* Explicit adapters only. Serialize against all other codec/cloud sessions.
 * Caller owns runner, client, contexts and aligned PCM until completion.
 * ready rechecks current network/trust state, not merely cached Wi-Fi. It is
 * called from playback cancellation polls: MUST be bounded/nonblocking and
 * must not perform network, logging, allocation or hardware reconfiguration.
 * Does not register commands/startup or initiate an operation at load time. */
extern const struct vv_report_audio_ops vv_report_audio_k7_ops;
#endif
