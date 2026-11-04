/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_TRACK_H
#define K7_TRACK_H
#include <stdbool.h>
#include <stdint.h>
#include "k7_yunet.h"
enum k7_track_axes_e { TRACK_X = 1, TRACK_Y = 2, TRACK_XY = 3 };
struct k7_track_config_s {
  bool transmit; int limit, sign_x, sign_y; unsigned int axes; bool preview;
  bool calibrated; bool photo;
  bool voice_control; /* Keep one capture session; start with motion held. */
  int min_x, max_x, min_y, max_y, center_x, center_y;
};
enum k7_track_state_e { TRACK_LOST, TRACK_CONFIRM, TRACK_ACTIVE, TRACK_STALE, TRACK_HALTED, TRACK_CLIPPED, TRACK_STATE_COUNT };
struct k7_track_s
{
  struct k7_track_config_s config;
  struct k7_face_s previous;
  bool have_previous;
  bool observing; /* Target observations active, motion controller suspended. */
  unsigned int hits_x, hits_y;
  unsigned int edge_y, edge_y_hits;
  unsigned int edge_x, edge_x_hits;
  float filtered_x, filtered_y, target_x, target_y;
  /* Command-space velocity, MCU target units/s (not degrees/s). */
  float velocity_x, velocity_y;
  uint64_t previous_time, previous_sample;
  int sent_x, sent_y;
  /* Deferred motion: owner serializes measurement/tick/hold operations. */
  bool motion_armed;
  bool motion_deferred;
  unsigned int motion_axes;
  uint64_t motion_time;
  float demand_x, demand_y, rate_x, rate_y;
};
struct k7_track_result_s
{
  enum k7_track_state_e state;
  bool update;
  int x, y;
  float dx, dy;
  bool has_face;
  struct k7_face_s box;
  unsigned int edges; /* left=1, top=2, right=4, bottom=8 */
};
int k7_track_init(struct k7_track_s *, const struct k7_track_config_s *,
                  int initial_x, int initial_y);
void k7_track_step(struct k7_track_s *, const struct k7_faces_s *,
                    uint64_t now, uint64_t sample, bool halted,
                    struct k7_track_result_s *);
/* Select and validate the same target without changing sent_x/y or producing
 * an update. A subsequent step() discards observation-time motion history and
 * confirms a fresh target before moving. Halt/stale/loss behavior is retained. */
void k7_track_observe(struct k7_track_s *, const struct k7_faces_s *,
                       uint64_t now, uint64_t sample, bool halted,
                       struct k7_track_result_s *);
/* Publish a latest observation without advancing/sending a position. */
void k7_track_measure(struct k7_track_s *, const struct k7_faces_s *,
                       uint64_t now, uint64_t sample, bool halted,
                       struct k7_track_result_s *);
/* Advance the existing bounded trajectory using a fresh measurement only. */
void k7_track_tick(struct k7_track_s *, uint64_t now, bool halted,
                    struct k7_track_result_s *);
/* Photo pause retains face association; resume needs fresh confirmation. */
void k7_track_pause(struct k7_track_s *);
#endif
