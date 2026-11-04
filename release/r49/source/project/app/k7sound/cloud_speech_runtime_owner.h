#ifndef CLOUD_SPEECH_RUNTIME_OWNER_H
#define CLOUD_SPEECH_RUNTIME_OWNER_H

#include "cloud_speech_radio_bridge.h"

/* All calls belong to one task, including callbacks. No locks, thread, timer,
 * allocation, network request or credential storage are created here. */
struct cloud_speech_runtime_owner {
  struct cloud_speech_orchestrator speech;
  struct cloud_speech_radio_bridge radio;
  int (*read_radio)(void *arg, struct cloud_speech_radio_snapshot *snapshot);
  void *radio_arg;
  bool initialized;
  bool running;
  bool dispatching;
  int last_error;
};

int cloud_speech_runtime_owner_init(
  struct cloud_speech_runtime_owner *owner,
  const struct cloud_speech_ops *ops, void *ops_arg,
  unsigned char *wav_scratch, size_t wav_capacity,
  int16_t *pcm_scratch, size_t pcm_frames,
  int (*read_radio)(void *, struct cloud_speech_radio_snapshot *),
  void *radio_arg);

/* Starts with every gate closed and immediately samples the radio. A failed
 * first sample keeps the polling lifecycle running, with gates closed; later
 * polls may recover. Repeated start is EBUSY. Init is for a fresh object only. */
int cloud_speech_runtime_owner_start(struct cloud_speech_runtime_owner *owner);
int cloud_speech_runtime_owner_poll(struct cloud_speech_runtime_owner *owner);

/* Owner-task cancellation stops polling and clears every gate. It is
 * idempotent. During an in-flight synchronous speech call it returns EBUSY:
 * the transport must be cancelled/joined before calling this API. */
int cloud_speech_runtime_owner_cancel(struct cloud_speech_runtime_owner *owner);
bool cloud_speech_runtime_owner_ready(
  const struct cloud_speech_runtime_owner *owner);

#endif
