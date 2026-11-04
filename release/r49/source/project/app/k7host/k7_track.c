/* SPDX-License-Identifier: Apache-2.0 */
#include "k7_track.h"
#include <errno.h>
#include <math.h>
#include <string.h>
/* MCU target units, not degrees. Keep physical range in the run config. */
#define FILTER_NEW 0.75f
#define X_DEADBAND 0.03f
#define Y_DEADBAND 0.04f
#define X_GAIN 300.0f /* 1.5x legacy integral gain; MCU units, not degrees */
#define Y_GAIN 180.0f /* 1.5x legacy integral gain; MCU units, not degrees */
#define X_PROPORTIONAL 105.0f
#define Y_PROPORTIONAL 67.5f
#define MAX_STEP 37.5f /* bound a single clipped-face recovery update */
#define SLEW_RATE 375.0f
#define ACCEL_RATE 1500.0f /* MCU target units/s^2; soften starts/reversals */
#define X_EDGE_RATE 225.0f /* bounded horizontal clipped-face recovery */
#define Y_EDGE_RATE 60.0f /* slower than full correction; legacy units/sec */
static float clamp(float x, float lo, float hi)
{ return fmaxf(lo, fminf(hi, x)); }
static float correction(float filtered, float raw, float deadband)
{
  /* Old filter history must not command motion in the wrong direction
   * when the latest observation has already crossed the image center.
   * Removing the deadband width makes its boundary continuous.
   */
  if (filtered*raw <= 0) return 0;
  return copysignf(fmaxf(fabsf(filtered)-deadband,0),filtered);
}
static float axis_pi(float *integral, float error, float seconds,
                     float gain, float proportional, int sign, int sent,
                     float low, float high)
{
  /* Keep the integral in MCU command units; no frame-rate-dependent gain.
   * The proportional term reacts immediately to the latest image offset.
   * Back-calculation prevents accumulating an unreachable integral at a limit.
   */
  float p=error*proportional*sign;
  if (error==0) { *integral=sent; return (float)sent; }
  /* Retain the quiet near-center gain. A large, confirmed full-face
   * offset needs faster sustained catch-up before clipping, not a larger
   * motion speed/acceleration limit. Smoothly raise integral gain up to 3x
   * across normalized corrected error 0.20..0.50; retain near-center gain. */
  float catchup=clamp((fabsf(error)-0.20f)/0.30f,0,1);
  gain*=1.0f+2.0f*catchup*catchup*(3.0f-2.0f*catchup);
  float next=clamp(*integral+error*gain*seconds*sign,low,high);
  /* Bound unseen accumulated travel to 10 MCU command units ahead of
   * the position already sent. This prevents large-error windup while
   * the existing trajectory accelerates toward its unchanged speed cap. */
  if ((next-sent)*error*sign > 0 && fabsf(next-sent)>10.0f)
    next=clamp(sent+copysignf(10.0f,error*sign),low,high);
  float demand=clamp(next+p,low,high);
  /* Retire an accumulated target that would move away from the
   * current confirmed face offset. Keep the existing motion ramp. */
  if ((demand-sent)*error*sign < 0)
    {
      next=clamp((float)sent-p,low,high);
      demand=clamp(next+p,low,high);
    }
  if (demand != next+p) next=clamp(demand-p,low,high);
  *integral=next;
  return demand;
}
static float axis_motion(float *velocity, float sent, float target,
                         float seconds, float max_rate,
                         float low, float high)
{
  if (seconds <= 0)
    {
      *velocity=0;
      return sent;
    }
  float distance=target-sent;
  /* A deadband hold has target == sent. Retaining velocity here would
   * command motion away from an already reached target on every sample. */
  if (fabsf(distance)<0.0001f)
    {
      *velocity+=clamp(-*velocity,-ACCEL_RATE*seconds,ACCEL_RATE*seconds);
      return sent;
    }
  /* Stopping-distance speed target plus acceleration limiting gives a
   * smooth ramp through starts, stops and direction reversals.
   */
  float wanted=copysignf(fminf(max_rate,
                  sqrtf(2.0f*ACCEL_RATE*fabsf(distance))),distance);
  float old=*velocity;
  float dv=clamp(wanted-old,-ACCEL_RATE*seconds,ACCEL_RATE*seconds);
  float next_velocity=clamp(old+dv,-max_rate,max_rate);
  float next=sent+(old+next_velocity)*0.5f*seconds;
  /* Brake obsolete motion sooner, without accelerating through zero at the
   * stronger rate. Normal acceleration/stopping-distance limits resume on
   * the next tick; use the actual time to zero, not the whole tick. */
  if (old*distance < 0 && fabsf(old) <= max_rate)
    {
      const float brake_rate=2.0f*ACCEL_RATE;
      float braking_seconds=fminf(seconds,fabsf(old)/brake_rate);
      next_velocity=old+clamp(-old,-brake_rate*seconds,brake_rate*seconds);
      next=sent+(old+next_velocity)*0.5f*braking_seconds;
    }
  /* Discrete updates can cross a nearby target despite the continuous
   * stopping-distance formula. Settle at it instead of hunting around it. */
  if ((distance>0 && next>=target) || (distance<0 && next<=target))
    {
      next=target;
      next_velocity=old+clamp(-old,-ACCEL_RATE*seconds,ACCEL_RATE*seconds);
    }
  if (next < low || next > high)
    {
      next=clamp(next,low,high);
      next_velocity=0;
    }
  *velocity=next_velocity;
  return next;
}
static float iou(const struct k7_face_s *a, const struct k7_face_s *b)
{
  float w = fminf(a->x+a->width,b->x+b->width)-fmaxf(a->x,b->x);
  float h = fminf(a->y+a->height,b->y+b->height)-fmaxf(a->y,b->y);
  float intersect = fmaxf(w,0)*fmaxf(h,0);
  return intersect/(a->width*a->height+b->width*b->height-intersect);
}
static bool valid(const struct k7_face_s *f)
{
  return isfinite(f->x) && isfinite(f->y) && isfinite(f->width) &&
    isfinite(f->height) && isfinite(f->score) && f->score >= .7f &&
    f->score <= 1 && f->x >= 0 && f->y >= 0 &&
    f->width > 0 && f->height > 0 &&
    f->x+f->width <= 160.01f && f->y+f->height <= 120.01f;
}
int k7_track_init(struct k7_track_s *s, const struct k7_track_config_s *c,
                  int x, int y)
{
  if (!s || !c ||
      c->axes < TRACK_X || c->axes > TRACK_XY ||
      (c->sign_x != -1 && c->sign_x != 1) ||
      (c->sign_y != -1 && c->sign_y != 1))
    return -EINVAL;
  if (c->calibrated)
    {
      if (c->min_x < -800 || c->min_x > 800 || c->max_x < -800 || c->max_x > 800 ||
          c->min_y < -120 || c->min_y > 1030 || c->max_y < -120 || c->max_y > 1030 ||
          c->center_x < c->min_x+20 || c->center_x > c->max_x-20 ||
          c->center_y < c->min_y+20 || c->center_y > c->max_y-20 ||
          x < c->min_x || x > c->max_x || y < c->min_y || y > c->max_y)
        return -EINVAL;
    }
  else if (c->limit < 1 || c->limit > 150 ||
           x < -c->limit || x > c->limit || y < -c->limit ||
           y > c->limit || y < -120) return -EINVAL;
  memset(s,0,sizeof(*s)); s->config=*c;
  s->target_x=s->sent_x=x; s->target_y=s->sent_y=y;
  return 0;
}
static void track_step(struct k7_track_s *s, const struct k7_faces_s *faces,
                       uint64_t now, uint64_t sample, bool halted, bool observe,
                       bool deferred,
                       struct k7_track_result_s *out)
{
  if (!observe && (s->observing || deferred!=s->motion_deferred))
    {
      /* Do not replay error, velocity or integration accumulated while the
       * user's head was changing pose in front of a stationary camera. */
      struct k7_track_config_s config=s->config;
      k7_track_init(s,&config,s->sent_x,s->sent_y);
    }
  if(!observe)s->motion_deferred=deferred;
  bool was_armed=s->motion_armed;
  if(deferred||observe)s->motion_armed=false;
  s->observing=observe;
  if (observe)
    {
      s->target_x=s->sent_x;s->target_y=s->sent_y;
      s->velocity_x=0;s->velocity_y=0;
    }
  *out=(struct k7_track_result_s){.state=TRACK_LOST,.x=s->sent_x,.y=s->sent_y};
  if (halted) { out->state=TRACK_HALTED; goto reset; }
  if (sample > now || now-sample > 250000 ||
      (s->previous_sample && sample <= s->previous_sample) ||
      (s->previous_time && now <= s->previous_time))
    { out->state=TRACK_STALE; goto reset; }
  bool gap=s->previous_time && now-s->previous_time > 400000;
  uint64_t dt=s->previous_time && !gap ? now-s->previous_time : 0;
  s->previous_sample=sample; s->previous_time=now;
  if (gap) { s->have_previous=false; s->hits_x=0; s->hits_y=0;
             s->edge_y=0; s->edge_y_hits=0; s->edge_x=0; s->edge_x_hits=0; }
  if (gap) { s->velocity_x=0; s->velocity_y=0;
             s->target_x=s->sent_x; s->target_y=s->sent_y; }
  if (!faces || faces->count > K7_FACE_MAX) goto reset;
  int chosen=-1;
  float best=-1;
  if (s->have_previous)
    for (unsigned i=0;i<faces->count;i++)
      if (valid(&faces->face[i]))
        {
          float score=iou(&s->previous,&faces->face[i]);
          if (score >= .15f && score > best) { chosen=i;best=score; }
        }
  if (chosen < 0)
    {
      s->hits_x=0; s->hits_y=0; s->filtered_x=0; s->filtered_y=0;
      s->edge_y=0; s->edge_y_hits=0; s->edge_x=0; s->edge_x_hits=0;
      for (unsigned i=0;i<faces->count;i++)
        if (valid(&faces->face[i]))
          {
            const struct k7_face_s *f=&faces->face[i];
            float dx=f->x+f->width/2-80,dy=f->y+f->height/2-60;
            float priority=f->width*f->height*f->score*
              (1.25f-.25f*fminf(sqrtf(dx*dx+dy*dy)/100,1));
            if(priority>best) {chosen=i;best=priority;}
          }
    }
  if (chosen < 0) goto reset;
  out->has_face=true;
  out->box=faces->face[chosen];
  out->dx=out->box.x+out->box.width/2-80;
  out->dy=out->box.y+out->box.height/2-60;
  out->edges=(out->box.x <= 0 ? 1u:0) |
             (out->box.y <= 0 ? 2u:0) |
             (out->box.x+out->box.width >= 160 ? 4u:0) |
             (out->box.y+out->box.height >= 120 ? 8u:0);
  /* A one-sided clipped box gives direction, not a reliable full error.
   * Treat the outer pixel as the same edge so rounding does not repeatedly
   * reset confirmation. Opposite edges / centered boxes remain ambiguous.
   * Two consecutive observations permit bounded X recovery; no blind search.
   */
  unsigned int x_edge=(out->box.x<=1.0f ? 1u:0) |
    (out->box.x+out->box.width>=159.0f ? 4u:0);
  bool recover_x=(s->config.axes & TRACK_X) &&
    ((x_edge==1u && out->dx < -80*X_DEADBAND) ||
     (x_edge==4u && out->dx > 80*X_DEADBAND));
  if (recover_x)
    {
      if (s->edge_x != x_edge)
        { s->edge_x_hits=0; s->target_x=s->sent_x; }
      s->edge_x=x_edge;
      if (s->edge_x_hits<2) s->edge_x_hits++;
    }
  else { s->edge_x=0; s->edge_x_hits=0; }
  bool valid_x=(s->config.axes & TRACK_X) && !x_edge;
  unsigned int y_edge=(out->box.y<=1.0f ? 2u:0) |
    (out->box.y+out->box.height>=119.0f ? 8u:0);
  bool valid_y=(s->config.axes & TRACK_Y) && !y_edge;
  bool recover_y=(s->config.axes & TRACK_Y) &&
    ((y_edge==8u && out->dy>60*Y_DEADBAND) ||
     (y_edge==2u && out->dy < -60*Y_DEADBAND));
  if (recover_y)
    {
      if (s->edge_y != y_edge)
        { s->edge_y_hits=0; s->target_y=s->sent_y; }
      s->edge_y=y_edge;
      if (s->edge_y_hits<2) s->edge_y_hits++;
    }
  else { s->edge_y=0; s->edge_y_hits=0; }
  s->previous=faces->face[chosen]; s->have_previous=true;
  out->dx=s->previous.x+s->previous.width/2-80;
  out->dy=s->previous.y+s->previous.height/2-60;
  if (valid_x)
    {
      if (s->hits_x < 2) s->hits_x++;
      s->filtered_x=(1-FILTER_NEW)*s->filtered_x+FILTER_NEW*out->dx/80;
    }
  else
    {
      s->hits_x=0; s->filtered_x=0;
      if (!recover_x || s->edge_x_hits<2)
        { s->target_x=s->sent_x; s->velocity_x=0; }
    }
  if (valid_y)
    {
      if (s->hits_y < 2) s->hits_y++;
      s->filtered_y=(1-FILTER_NEW)*s->filtered_y+FILTER_NEW*out->dy/60;
    }
  else
    {
      s->hits_y=0; s->filtered_y=0;
      if (!recover_y || s->edge_y_hits<2)
        { s->target_y=s->sent_y; s->velocity_y=0; }
    }
  if (!valid_x && !valid_y && !recover_y && !recover_x) {out->state=TRACK_CLIPPED;return;}
  if (s->hits_x < 2 && s->hits_y < 2 && s->edge_y_hits<2 && s->edge_x_hits<2)
    {out->state=TRACK_CONFIRM;return;}
  out->state=TRACK_ACTIVE;
  if (observe) return;
  /* The legacy MCU's negative Y command hits 500us near -133 units.
   * Keep margin at -120; a symmetric large range would wind up at the MCU.
   * X +/-800 maps to 2452..532us with MCU v1.3 MID=1492.
   */
  float seconds=fminf((float)dt/1000000,.2f);
  float low_x=s->config.calibrated ? s->config.min_x : -s->config.limit;
  float high_x=s->config.calibrated ? s->config.max_x : s->config.limit;
  float low_y=s->config.calibrated ? s->config.min_y : fmaxf(-s->config.limit,-120);
  float high_y=s->config.calibrated ? s->config.max_y : s->config.limit;
  float x=s->sent_x,y=s->sent_y;
  float demand_x=s->target_x,demand_y=s->target_y;
  if (s->hits_x>=2)
    demand_x=axis_pi(&s->target_x,
              correction(s->filtered_x,out->dx,X_DEADBAND),
              seconds,X_GAIN,X_PROPORTIONAL,s->config.sign_x,s->sent_x,
              low_x,high_x);
  if (s->hits_y>=2)
    demand_y=axis_pi(&s->target_y,
              correction(s->filtered_y,out->dy,Y_DEADBAND),
              seconds,Y_GAIN,Y_PROPORTIONAL,s->config.sign_y,s->sent_y,
              low_y,high_y);
  if (recover_x && s->edge_x_hits>=2)
    {
      float step=fminf(MAX_STEP,X_EDGE_RATE*seconds);
      s->target_x=clamp(s->target_x+copysignf(step,out->dx)*
                         s->config.sign_x,low_x,high_x);
      demand_x=s->target_x;
    }
  if(deferred)
    {
      if(recover_y&&s->edge_y_hits>=2)
        {
          s->target_y=clamp(s->target_y+copysignf(Y_EDGE_RATE*seconds,out->dy)*
                            s->config.sign_y,low_y,high_y);
          demand_y=s->target_y;
        }
      s->motion_axes=0;
      if(s->hits_x>=2 || (recover_x && s->edge_x_hits>=2))
        s->motion_axes|=TRACK_X;
      else {s->velocity_x=0;s->target_x=s->sent_x;demand_x=s->sent_x;}
      if(s->hits_y>=2 || (recover_y && s->edge_y_hits>=2))
        s->motion_axes|=TRACK_Y;
      else {s->velocity_y=0;s->target_y=s->sent_y;demand_y=s->sent_y;}
      s->demand_x=demand_x;s->demand_y=demand_y;
      s->rate_x=recover_x?X_EDGE_RATE:SLEW_RATE;
      s->rate_y=recover_y?Y_EDGE_RATE:SLEW_RATE;
      s->motion_armed=s->motion_axes!=0;
      if(!was_armed)s->motion_time=now;
      out->x=s->sent_x;out->y=s->sent_y;out->update=false;
      return;
    }
  if (s->hits_x>=2 || (recover_x && s->edge_x_hits>=2))
    x=axis_motion(&s->velocity_x,s->sent_x,demand_x,seconds,
                  recover_x ? X_EDGE_RATE : SLEW_RATE,low_x,high_x);
  else
    { s->velocity_x=0; s->target_x=s->sent_x; }
  if (recover_y && s->edge_y_hits>=2)
    {
      s->target_y=clamp(s->target_y+copysignf(Y_EDGE_RATE*seconds,out->dy)*
                         s->config.sign_y,low_y,high_y);
      demand_y=s->target_y;
    }
  if (s->hits_y>=2 || (recover_y && s->edge_y_hits>=2))
    y=axis_motion(&s->velocity_y,s->sent_y,demand_y,seconds,
                  recover_y ? Y_EDGE_RATE : SLEW_RATE,low_y,high_y);
  else
    { s->velocity_y=0; s->target_y=s->sent_y; }
  out->x=(int)lroundf(x);out->y=(int)lroundf(y);
  out->update=out->x!=s->sent_x || out->y!=s->sent_y;
  s->sent_x=out->x;s->sent_y=out->y;
  return;
reset:
  s->motion_armed=false;s->motion_axes=0;s->motion_time=0;
  s->have_previous=false;s->hits_x=0;s->hits_y=0;s->filtered_x=0;s->filtered_y=0;
  s->edge_y=0;s->edge_y_hits=0;s->edge_x=0;s->edge_x_hits=0;
  s->velocity_x=0;s->velocity_y=0;
  /* Freeze at last requested position, not at a mechanical zero. */
  s->target_x=s->sent_x;s->target_y=s->sent_y;
}

void k7_track_step(struct k7_track_s *s, const struct k7_faces_s *faces,
                    uint64_t now, uint64_t sample, bool halted,
                    struct k7_track_result_s *out)
{
  track_step(s,faces,now,sample,halted,false,false,out);
}

void k7_track_observe(struct k7_track_s *s, const struct k7_faces_s *faces,
                       uint64_t now, uint64_t sample, bool halted,
                       struct k7_track_result_s *out)
{
  track_step(s,faces,now,sample,halted,true,false,out);
}
void k7_track_measure(struct k7_track_s *s,const struct k7_faces_s *faces,
                       uint64_t now,uint64_t sample,bool halted,
                       struct k7_track_result_s *out)
{
  track_step(s,faces,now,sample,halted,false,true,out);
}
void k7_track_pause(struct k7_track_s*s)
{
  s->motion_armed=false;s->motion_axes=0;s->motion_time=0;
  s->velocity_x=s->velocity_y=0;
  s->target_x=s->sent_x;s->target_y=s->sent_y;
  s->observing=true;
}
void k7_track_tick(struct k7_track_s *s,uint64_t now,bool halted,
                    struct k7_track_result_s *out)
{
  *out=(struct k7_track_result_s){.state=TRACK_HALTED,
    .x=s->sent_x,.y=s->sent_y};
  if(!s->motion_deferred)return;
  if(halted||!s->previous_sample||
     now<s->previous_time||now<s->previous_sample||
     now-s->previous_sample>250000||now<s->motion_time)
    {
      s->motion_armed=false;s->motion_axes=0;s->motion_time=0;
      s->velocity_x=s->velocity_y=0;
      s->target_x=s->sent_x;s->target_y=s->sent_y;
      s->have_previous=false;s->hits_x=s->hits_y=0;
      s->filtered_x=s->filtered_y=0;
      s->edge_x=s->edge_y=s->edge_x_hits=s->edge_y_hits=0;
      out->state=halted?TRACK_HALTED:TRACK_STALE;
      return;
    }
  if(!s->motion_armed)
    {
      /* Normal first-frame confirmation must survive intervening ticks. */
      out->state=s->have_previous?TRACK_CONFIRM:TRACK_LOST;
      return;
    }
  out->state=TRACK_ACTIVE;
  if(!s->motion_time||now==s->motion_time){s->motion_time=now;return;}
  float seconds=fminf((float)(now-s->motion_time)/1000000,.2f);
  s->motion_time=now;
  float lx=s->config.calibrated?s->config.min_x:-s->config.limit;
  float hx=s->config.calibrated?s->config.max_x:s->config.limit;
  float ly=s->config.calibrated?s->config.min_y:fmaxf(-s->config.limit,-120);
  float hy=s->config.calibrated?s->config.max_y:s->config.limit;
  float x=s->sent_x,y=s->sent_y;
  if(s->motion_axes&TRACK_X)
    x=axis_motion(&s->velocity_x,x,s->demand_x,seconds,s->rate_x,lx,hx);
  if(s->motion_axes&TRACK_Y)
    y=axis_motion(&s->velocity_y,y,s->demand_y,seconds,s->rate_y,ly,hy);
  out->x=(int)lroundf(x);out->y=(int)lroundf(y);
  out->update=out->x!=s->sent_x||out->y!=s->sent_y;
  s->sent_x=out->x;s->sent_y=out->y;
}
