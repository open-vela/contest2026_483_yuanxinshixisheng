/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_PIPELINE_H
#define K7_PIPELINE_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "k7_yunet.h"
#include "k7_track.h"
struct k7_pipeline_s;
/* Actual selected tracking target, in unmirrored RGB160x120 coordinates.
 * sample_us is parser publication time, not sensor exposure time. */
struct k7_pipeline_track_snapshot_s
{
  bool valid;
  unsigned int sequence;
  uint64_t sample_us, observed_us;
  enum k7_track_state_e state;
  struct k7_face_s box;
};
/* Nonblocking display-only snapshot. 0 includes an invalid/expired target;
 * -EAGAIN means short metadata lock busy (out is cleared); -EINVAL for NULL.
 * Targets older than 250 ms are hidden. No inference or hardware access. */
int k7_pipeline_track_snapshot(struct k7_pipeline_track_snapshot_s *out);
struct k7_pipeline_stats_s
{
  unsigned int published, decoded, failed, dropped, peak_pending;
  unsigned int last_sequence, width, height;
  size_t bytes;
  uint64_t decode_us, max_decode_us, max_age_us, elapsed_us;
  int last_error;
  unsigned int face_inferences, face_frames, face_failures, last_face_count;
  uint64_t inference_us, max_inference_us;
  struct k7_face_s primary;
  unsigned int track_observations, track_updates, track_tx, track_errors;
  unsigned int track_states[TRACK_STATE_COUNT], track_max_step;
  int track_x, track_y;
  int track_error;
  unsigned int sink_presented, sink_calls, sink_dropped;
  uint64_t sink_us, max_sink_us, max_sink_age_us;
  int sink_error;

};
/* Single producer, one decoder. Caller keeps RGB alive through finish.
 * finish joins the worker before releasing its private JPEG slots.
 * Age starts at publication by the parser, NOT sensor exposure time.
 */
int k7_pipeline_start(struct k7_pipeline_s **out, size_t jpeg_capacity,
                      unsigned int scale, unsigned int delay_ms,
                      uint8_t *rgb, size_t rgb_capacity, bool detect_faces,
                      const struct k7_track_config_s *track);
/* Camera-only RGB640 sink: callback must not retain rgb after returning.
 * Three private RGB slots separate decode from synchronous presentation.
 * Each worker targets 30 Hz; slow frames incur no extra delay. Latest pending
 * JPEG/RGB is selected; owned decode/present buffers are never overwritten. First
 * callback error stops this worker and is retained until finish. Existing
 * start() behavior is unchanged. Caller keeps arg alive through finish, which
 * joins both workers (including any admitted callback) before freeing slots.
 * sink_dropped counts decoded RGB discarded before presentation separately
 * from dropped JPEG input; max_sink_age_us is parser-to-present completion. */
struct k7_pipeline_sink_s
{
  void *arg;
  int (*present)(void *arg, const uint8_t *rgb, unsigned int width,
                 unsigned int height, size_t stride, size_t bytes);
};
int k7_pipeline_start_sink(struct k7_pipeline_s **out, size_t jpeg_capacity,
                          uint8_t *rgb, size_t rgb_capacity,
                          const struct k7_pipeline_sink_s *sink);
/* Optional independent LCD sidecar: feed the same complete JPEG already
 * submitted to the vision pipeline. No second camera session, no borrowed
 * vision RGB. Uses private RGB slots and a separate decode CPU when available.
 * A failure stops this sidecar only; caller must not propagate its status to
 * the vision pipeline. Stop publishing, then finish/join before releasing arg.
 * This function itself never opens, closes or writes display hardware. */
int k7_pipeline_start_lcd_preview(struct k7_pipeline_s **out,
                                 size_t jpeg_capacity,
                                 const struct k7_pipeline_sink_s *sink);
/* Producer polls between USB banks; 0 until a retained sink failure. */
int k7_pipeline_sink_status(struct k7_pipeline_s *p);
/* Halt stops future visual target updates, not MCU PWM or already queued bytes. */
void k7_pipeline_halt(void);
/* Voice session modes: 0 hold, 1 tracking, 2 photo. Does not reopen UVC.
 * Return 0 means accepted; VOICE MODE applied is emitted by the worker.
 */
int k7_pipeline_voice_mode(unsigned int mode);
/* -EAGAIN while worker has not applied the request; 0 only after application. */
int k7_pipeline_voice_mode_status(unsigned int mode);
/* Active pipeline independent of currently requested/applied voice mode. */
int k7_pipeline_voice_session_status(void);
/* Resume only if no stop/mode request occurred since this held revision. */
int k7_pipeline_hold_revision(unsigned int *revision);
int k7_pipeline_resume_tracking(unsigned int revision);
void k7_pipeline_publish(struct k7_pipeline_s *p,
                         const uint8_t *jpeg, size_t bytes);
int k7_pipeline_finish(struct k7_pipeline_s *p,
                       struct k7_pipeline_stats_s *stats);
#endif
