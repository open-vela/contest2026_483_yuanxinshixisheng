/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VELAVISION_FUSION_CORE_H
#define VELAVISION_FUSION_CORE_H
#include <stdbool.h>
#include <stdint.h>

enum vf_shot { VF_NONE=0, VF_FRONT=1, VF_LEFT=2, VF_RIGHT=4 };
enum vf_reason { VF_WAIT, VF_STABILIZING, VF_CAPTURE, VF_SAVE_PENDING,
                 VF_COMPLETE, VF_STALE, VF_TARGET_CHANGED, VF_QUALITY };
struct vf_pose { float yaw, pitch, roll; bool valid; };
struct vf_config {
  float side_angle, tolerance, tilt_limit, yaw_jitter;
  float min_face_px, min_sharpness;
  uint32_t stable_ms, max_gap_ms, max_age_ms, object_period_ms;
  int left_yaw_sign;
  float side_tolerance; /* Side-view yaw window, independent of front tolerance. */
};
/* All image/pose/quality fields MUST describe this exact frame and lock epoch.
 * An epoch is a spatial lock generation, never a person's identity.
 */
struct vf_frame {
  uint64_t frame_id, captured_ms, now_ms, lock_epoch;
  struct vf_pose raw_pose, filtered_pose;
  float face_size, sharpness;
  bool face_locked, ambiguous, gimbal_settled;
  bool object_busy, spare_inference_budget;
};
struct vf_result {
  enum vf_reason reason;
  enum vf_shot shot;
  uint64_t frame_id, lock_epoch;
  bool request_object;
};
struct vf_state {
  struct vf_config config;
  uint64_t epoch, last_id, last_time, stable_start, object_at;
  uint64_t pending_frame;
  enum vf_shot stable_shot, pending_shot;
  float anchor_yaw;
  unsigned samples, done;
  bool seen, pending, object_seen;
};
int vf_init(struct vf_state *, const struct vf_config *, uint64_t lock_epoch);
struct vf_result vf_step(struct vf_state *, const struct vf_frame *);
/* Call ONLY after the worker saved the exact reserved JPEG plus metadata.
 * On save failure, release reservation and retry; never mark a photo done.
 */
bool vf_saved(struct vf_state *, uint64_t frame_id, uint64_t lock_epoch,
              enum vf_shot shot, bool success);
#endif
