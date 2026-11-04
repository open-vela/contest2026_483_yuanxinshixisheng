/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VV_MICROCRYSTAL_H
#define VV_MICROCRYSTAL_H
#include <stdbool.h>
#include <stdint.h>

/* Fixed demonstration sequence, not a treatment selected from skin scores.
 * Adapter calls must be bounded and must never reconnect and resume output.
 * write_ascii succeeds only for the configured, discovered control channel.
 * stop_confirmed must represent the device's verified stop policy; a queued
 * GATT request alone must not be advertised as physical confirmation.
 */
struct vv_microcrystal_ops {
  void *arg;
  uint64_t (*now_ms)(void *);
  void (*sleep_ms)(void *, unsigned);
  bool (*cancelled)(void *);
  int (*connect)(void *);
  bool (*connected)(void *);
  int (*write_ascii)(void *, char);
  bool (*stop_confirmed)(void *);
  void (*disconnect)(void *);
};
struct vv_microcrystal_result {
  int error;
  int stop_error;
  bool output_may_be_active;
  bool stop_written;
  bool stopped_confirmed;
  bool resume_allowed;
  unsigned steps_completed;
  uint64_t elapsed_ms;
};
/* Only a successfully completed genuine report can admit the sequence. */
int vv_microcrystal_run(const struct vv_microcrystal_ops *, bool genuine_report,
                       bool playback_completed, struct vv_microcrystal_result *);
#endif
