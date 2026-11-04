/* SPDX-License-Identifier: Apache-2.0 */
#include "fusion_core.h"
#include <math.h>
#include <string.h>

static void reset_stability(struct vf_state *s)
{ s->samples=0; s->stable_shot=VF_NONE; }
static bool finite_pose(struct vf_pose p)
{ return p.valid && isfinite(p.yaw) && isfinite(p.pitch) && isfinite(p.roll); }
static enum vf_shot classify(const struct vf_config *c,struct vf_pose p)
{
  if(!finite_pose(p) || fabsf(p.pitch)>c->tilt_limit || fabsf(p.roll)>c->tilt_limit)
    return VF_NONE;
  if(fabsf(p.yaw)<=c->tolerance) return VF_FRONT;
  if(fabsf(fabsf(p.yaw)-c->side_angle)>c->side_tolerance) return VF_NONE;
  return p.yaw*c->left_yaw_sign>0 ? VF_LEFT : VF_RIGHT;
}
int vf_init(struct vf_state *s,const struct vf_config *c,uint64_t epoch)
{
  if(!s || !c || !epoch || !isfinite(c->side_angle) || !isfinite(c->tolerance) ||
     !isfinite(c->tilt_limit) || !isfinite(c->yaw_jitter) || !isfinite(c->side_tolerance) ||
     !isfinite(c->min_face_px) || !isfinite(c->min_sharpness) ||
     c->side_angle<5 || c->side_angle>80 || c->tolerance<1 ||
     c->tolerance>20 || c->side_tolerance<1 || c->side_tolerance>20 ||
     c->side_angle<=c->tolerance+c->side_tolerance ||
     c->side_angle+c->side_tolerance>89 || c->tilt_limit<5 || c->tilt_limit>45 ||
     c->yaw_jitter<=0 || c->min_face_px<=0 || c->min_sharpness<=0 ||
     c->stable_ms<300 || c->stable_ms>3000 || !c->max_gap_ms ||
     !c->max_age_ms || !c->object_period_ms ||
     (c->left_yaw_sign!=1 && c->left_yaw_sign!=-1)) return -1;
  memset(s,0,sizeof(*s)); s->config=*c; s->epoch=epoch; return 0;
}
struct vf_result vf_step(struct vf_state *s,const struct vf_frame *f)
{
  struct vf_result r={.reason=VF_WAIT,.frame_id=f->frame_id,.lock_epoch=f->lock_epoch};
  const struct vf_config *c=&s->config;
  if(f->captured_ms>f->now_ms || f->now_ms-f->captured_ms>c->max_age_ms ||
     (s->seen && (f->frame_id<=s->last_id || f->captured_ms<=s->last_time))) {
    reset_stability(s); r.reason=VF_STALE; return r;
  }
  bool gap=s->seen && f->captured_ms-s->last_time>c->max_gap_ms;
  s->seen=true; s->last_id=f->frame_id; s->last_time=f->captured_ms;
  if(gap) reset_stability(s);
  /* Advisory dispatch only. A separate latest-frame worker owns object jobs.
   * This core never blocks, runs inference, writes files, or commands motors.
   */
  if(!f->object_busy && f->spare_inference_budget &&
     (!s->object_seen || (f->now_ms>=s->object_at &&
                          f->now_ms-s->object_at>=c->object_period_ms))) {
    r.request_object=true; s->object_seen=true; s->object_at=f->now_ms;
  }
  if(s->pending) {r.reason=VF_SAVE_PENDING;return r;}
  if(s->done==(VF_FRONT|VF_LEFT|VF_RIGHT)) {r.reason=VF_COMPLETE;return r;}
  if(!f->face_locked || f->ambiguous || f->lock_epoch!=s->epoch) {
    reset_stability(s);r.reason=VF_TARGET_CHANGED;return r;
  }
  if(!f->gimbal_settled || !isfinite(f->face_size) || !isfinite(f->sharpness) ||
     f->face_size<c->min_face_px || f->sharpness<c->min_sharpness) {
    reset_stability(s);r.reason=VF_QUALITY;return r;
  }
  enum vf_shot shot=classify(c,f->filtered_pose);
  /* Require current raw pose as well as filtered pose, including FRONT. */
  if(shot==VF_NONE || classify(c,f->raw_pose)!=shot || (s->done&shot)) {
    reset_stability(s);return r;
  }
  if(!s->samples || s->stable_shot!=shot ||
     fabsf(f->filtered_pose.yaw-s->anchor_yaw)>c->yaw_jitter) {
    s->stable_start=f->captured_ms; s->samples=0;
    s->stable_shot=shot;s->anchor_yaw=f->filtered_pose.yaw;
  }
  if(s->samples<3) s->samples++;
  r.reason=VF_STABILIZING;
  if(s->samples>=3 && f->captured_ms-s->stable_start>=c->stable_ms) {
    s->pending=true;s->pending_frame=f->frame_id;s->pending_shot=shot;
    r.reason=VF_CAPTURE;r.shot=shot;reset_stability(s);
  }
  return r;
}
bool vf_saved(struct vf_state *s,uint64_t id,uint64_t epoch,enum vf_shot shot,bool ok)
{
  if(!s->pending || id!=s->pending_frame || epoch!=s->epoch || shot!=s->pending_shot)
    return false;
  if(ok) s->done|=shot;
  s->pending=false;s->pending_shot=VF_NONE;reset_stability(s);return true;
}
