/* SPDX-License-Identifier: Apache-2.0 */
/* Bounded camera-to-decoder queue. No hardware or DMA buffers are owned here. */
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include "k7_pipeline.h"
#include <math.h>
#include "k7_jpeg.h"
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT && defined(CONFIG_EXAMPLES_K7DISPLAY)
#include "../k7display/rk3576_display_native.h"
static void display_policy_changed(void)
{
  /* Never acquire display owner while voice/photo/pipeline lock is held. */
  (void)k7display_video_stage_invalidate();
}
#endif
static pthread_mutex_t g_target_lock = PTHREAD_MUTEX_INITIALIZER;
static struct k7_pipeline_track_snapshot_s g_target;

static uint64_t monotonic_us(void);

int k7_pipeline_track_snapshot(struct k7_pipeline_track_snapshot_s *out)
{
  if (!out) return -EINVAL;
  memset(out,0,sizeof(*out));
  if (pthread_mutex_trylock(&g_target_lock)) return -EAGAIN;
  uint64_t now=monotonic_us();
  if (g_target.valid &&
      (now<g_target.sample_us || now-g_target.sample_us>250000))
    {
      g_target.valid=false;
      memset(&g_target.box,0,sizeof(g_target.box));
    }
  *out=g_target;
  pthread_mutex_unlock(&g_target_lock);
  return 0;
}

#if defined(CONFIG_EXAMPLES_K7HOST_TRACK) || defined(K7_PIPELINE_SNAPSHOT_TEST)
static void track_snapshot_clear(void)
{
  pthread_mutex_lock(&g_target_lock);
  memset(&g_target,0,sizeof(g_target));
  pthread_mutex_unlock(&g_target_lock);
}

static void track_snapshot_update(const struct k7_track_result_s *r,
                                   unsigned int sequence, uint64_t sample,
                                   uint64_t now)
{
  struct k7_pipeline_track_snapshot_s next={0};
  next.sequence=sequence;next.sample_us=sample;next.observed_us=now;
  next.state=r->state;
  const struct k7_face_s *b=&r->box;
  next.valid=r->has_face && sample<=now && now-sample<=250000 &&
    (r->state==TRACK_CONFIRM || r->state==TRACK_ACTIVE || r->state==TRACK_CLIPPED) &&
    isfinite(b->x) && isfinite(b->y) && isfinite(b->width) &&
    isfinite(b->height) && isfinite(b->score) && b->x>=0 && b->y>=0 &&
    b->width>0 && b->height>0 && b->x+b->width<=160.01f &&
    b->y+b->height<=120.01f;
  if (next.valid) next.box=*b;
  pthread_mutex_lock(&g_target_lock);
  g_target=next;
  pthread_mutex_unlock(&g_target_lock);
}
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
#include <stdatomic.h>
#include "../gimbal/gimbal_link.h"
#include "k7_preview.h"
#include "k7_photo.h"
#include "k7_photo_target.h"
static atomic_bool g_track_halt;
static pthread_mutex_t g_voice_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_voice_active;
static atomic_uint g_voice_mode;
static unsigned int g_voice_revision;
static unsigned int g_voice_applied_revision;
static unsigned int g_voice_applied_mode;
static int g_voice_applied_result;
int k7_pipeline_voice_session_status(void)
{
  pthread_mutex_lock(&g_voice_lock);
  int ret = g_voice_active ? 0 : -ENODEV;
  pthread_mutex_unlock(&g_voice_lock);
  return ret;
}
int k7_pipeline_voice_mode_status(unsigned int mode)
{
  pthread_mutex_lock(&g_voice_lock);
  int ret = !g_voice_active ? -ENODEV :
    mode != atomic_load(&g_voice_mode) ? -ECANCELED :
    g_voice_applied_revision != g_voice_revision ? -EAGAIN :
    g_voice_applied_result ? g_voice_applied_result :
    g_voice_applied_mode != mode ? -EIO : 0;
  pthread_mutex_unlock(&g_voice_lock);
  return ret;
}
int k7_pipeline_hold_revision(unsigned int *revision)
{
  if (!revision) return -EINVAL;
  pthread_mutex_lock(&g_voice_lock);
  int ret = !g_voice_active ? -ENODEV :
    atomic_load(&g_voice_mode) != 0 || !atomic_load(&g_track_halt) ||
    g_voice_applied_revision != g_voice_revision || g_voice_applied_result ? -EAGAIN : 0;
  if (!ret) *revision = g_voice_revision;
  pthread_mutex_unlock(&g_voice_lock);
  return ret;
}
int k7_pipeline_resume_tracking(unsigned int revision)
{
  pthread_mutex_lock(&g_voice_lock);
  int ret = !g_voice_active ? -ENODEV :
    g_voice_revision != revision || atomic_load(&g_voice_mode) != 0 ||
    !atomic_load(&g_track_halt) ? -ECANCELED : 0;
  if (!ret) { g_voice_revision++; atomic_store(&g_voice_mode, 1); }
  pthread_mutex_unlock(&g_voice_lock);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT && defined(CONFIG_EXAMPLES_K7DISPLAY)
  if (!ret) display_policy_changed();
#endif
  return ret;
}
void k7_pipeline_halt(void)
{
  pthread_mutex_lock(&g_voice_lock);
  atomic_store(&g_track_halt, true);
  atomic_store(&g_voice_mode, 0);
  g_voice_revision++;
  pthread_mutex_unlock(&g_voice_lock);
  track_snapshot_clear();
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT && defined(CONFIG_EXAMPLES_K7DISPLAY)
  display_policy_changed();
#endif
}
int k7_pipeline_voice_mode(unsigned int mode)
{
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT && defined(CONFIG_EXAMPLES_K7DISPLAY)
  unsigned before_revision;
#endif
  if (mode > 2) return -EINVAL;
  pthread_mutex_lock(&g_voice_lock);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT && defined(CONFIG_EXAMPLES_K7DISPLAY)
  before_revision=g_voice_revision;
#endif
  int ret = g_voice_active ? 0 : -ENODEV;
  if (!ret)
    {
      if ((mode == 2 && k7_photo_needs_restart()) ||
          mode != atomic_load(&g_voice_mode) ||
          (mode && atomic_load(&g_track_halt))) g_voice_revision++;
      atomic_store(&g_voice_mode, mode);
      /* Worker alone releases hold after resetting stale filter history. */
      if (!mode) atomic_store(&g_track_halt, true);
    }
 #if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT && defined(CONFIG_EXAMPLES_K7DISPLAY)
  bool display_changed=before_revision!=g_voice_revision;
 #endif
  pthread_mutex_unlock(&g_voice_lock);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT && defined(CONFIG_EXAMPLES_K7DISPLAY)
  if (display_changed) display_policy_changed();
#endif
  return ret;
}
#endif

#define K7_PIPE_SLOTS 8
#define K7_SINK_PERIOD_US 33334u
#define K7_RGB_SLOTS 3
#define K7_RGB_BYTES (640u * 480u * 3u)
enum slot_state_e { SLOT_FREE, SLOT_WRITING, SLOT_PENDING, SLOT_DECODING };
struct slot_s
{
  enum slot_state_e state;
  uint8_t *data;
  size_t bytes;
  unsigned int sequence;
  uint64_t published_us;
};
struct rgb_slot_s
{
  enum slot_state_e state;
  uint8_t *data;
  struct k7_jpeg_result_s result;
  uint64_t published_us;
};
struct k7_pipeline_s
{
  pthread_mutex_t lock;
  pthread_cond_t ready;
  pthread_t thread, sink_thread;
  bool sink_thread_started;
  uint8_t *sink_storage;
  struct rgb_slot_s rgb_slots[K7_RGB_SLOTS];
  struct slot_s slots[K7_PIPE_SLOTS];
  uint8_t *storage;
  uint8_t *rgb;
  size_t stride, rgb_capacity;
  unsigned int scale, delay_ms, pending;
  bool stopping;
  struct k7_pipeline_sink_s sink;
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
  bool staged, stage_pending, stage_admitted, decoder_joined;
  struct k7ds_ticket stage_ticket;
  uint64_t stage_published_us;
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  bool tracking;
  bool voice_control;
  unsigned int applied_mode;
  unsigned int applied_revision;
  struct k7_photo_s *photo;
  struct k7_preview_queue_s *preview;
  atomic_bool motion_done;
#ifdef K7_TRACK_CONTROL_CLOCK
  pthread_mutex_t control_lock;
  pthread_t control_thread;
  bool control_enabled,control_lock_ready,control_started;
  atomic_bool control_stop;
  uint64_t control_ticks,control_last_us,control_max_gap_us;
#endif
  int gimbal_fd;
  struct k7_track_s controller;
#endif
  uint64_t started_us;
  struct k7_pipeline_stats_s stats;
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
  struct k7_yunet_s *yunet;
#endif
};

static uint64_t monotonic_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
static bool photo_observation_only(const struct k7_pipeline_s *p)
{
#ifdef CONFIG_EXAMPLES_K7HOST_PHOTO_OBSERVE
  /* A newly accepted PHOTO request blocks motion even before the worker
   * applies it. Keep blocking during the reverse transition until reset. */
  return p->voice_control ? p->applied_mode==2 ||
    atomic_load(&g_voice_mode)==2 : p->controller.config.photo;
#else
  (void)p;
  return false;
#endif
}

static void voice_mode_apply(struct k7_pipeline_s *p)
{
  if (!p->voice_control) return;
#ifdef K7_TRACK_CONTROL_CLOCK
  bool log_applied=false;unsigned log_mode=0;int log_result=0;
#endif
  pthread_mutex_lock(&g_voice_lock);
#ifdef K7_TRACK_CONTROL_CLOCK
  if(p->control_enabled)pthread_mutex_lock(&p->control_lock);
#endif
  unsigned int mode = atomic_load(&g_voice_mode);
  if (g_voice_revision != p->applied_revision)
    {
      struct k7_track_config_s config = p->controller.config;
      int ret = p->stats.track_error;
      bool retain_target=false;
#ifdef CONFIG_EXAMPLES_K7HOST_PHOTO_OBSERVE
      /* Retain the selected person across follow -> photo. observe() freezes
       * motion state without discarding the face used by the pose gate. */
      retain_target=mode==2 && p->applied_mode==1 &&
        !atomic_load(&g_track_halt);
#endif
      if (!ret && !retain_target) ret = k7_track_init(&p->controller, &config,
                                    p->controller.sent_x, p->controller.sent_y);
      if (ret < 0) { p->stats.track_error = ret; mode = 0; }
      if (mode == 2) k7_photo_reset();
      atomic_store(&g_track_halt, mode == 0 || p->stats.track_error != 0);
      p->applied_mode = mode;
      p->applied_revision = g_voice_revision;
      g_voice_applied_revision = g_voice_revision;
      g_voice_applied_mode = mode;
      g_voice_applied_result = ret;
#ifdef K7_TRACK_CONTROL_CLOCK
      log_applied=true;log_mode=mode;log_result=ret;
#else
      printf("VOICE MODE applied=%u result=%d\n", mode, ret);
#endif
    }
#ifdef K7_TRACK_CONTROL_CLOCK
  if(p->control_enabled)pthread_mutex_unlock(&p->control_lock);
#endif
  pthread_mutex_unlock(&g_voice_lock);
#ifdef K7_TRACK_CONTROL_CLOCK
  if(log_applied)printf("VOICE MODE applied=%u result=%d\n",log_mode,log_result);
#endif
}

#ifdef K7_TRACK_CONTROL_CLOCK
/* Lock order is voice admission -> controller. Inference and logs stay outside
 * both locks; exactly this owner sends UART positions on the clocked path. */
static void control_once(struct k7_pipeline_s *p)
{
  if(p->voice_control)pthread_mutex_lock(&g_voice_lock);
  pthread_mutex_lock(&p->control_lock);
  uint64_t now=monotonic_us();
  if(p->control_last_us&&now>=p->control_last_us&&
     now-p->control_last_us>p->control_max_gap_us)
    p->control_max_gap_us=now-p->control_last_us;
  p->control_last_us=now;p->control_ticks++;
  bool halt=atomic_load(&p->control_stop)||atomic_load(&p->motion_done)||
    atomic_load(&g_track_halt)||p->stats.track_error;
  struct k7_track_result_s r={0};
  int x=p->controller.sent_x,y=p->controller.sent_y;
  if(!halt&&photo_observation_only(p))k7_track_pause(&p->controller);
  else k7_track_tick(&p->controller,now,halt,&r);
  if(r.update)
    {
      int ret=gimbal_link_send(p->gimbal_fd,r.x,r.y);
      if(ret<0)
        {
          p->controller.sent_x=x;p->controller.sent_y=y;
          p->stats.track_errors++;p->stats.track_error=ret;
          atomic_store(&p->motion_done,true);
        }
      else
        {
          unsigned step=(unsigned)(abs(r.x-x)>abs(r.y-y)?abs(r.x-x):abs(r.y-y));
          if(step>p->stats.track_max_step)p->stats.track_max_step=step;
          p->stats.track_tx++;p->stats.track_updates++;
        }
    }
  p->stats.track_x=p->controller.sent_x;p->stats.track_y=p->controller.sent_y;
  pthread_mutex_unlock(&p->control_lock);
  if(p->voice_control)pthread_mutex_unlock(&g_voice_lock);
}
static void *control_worker(void *arg)
{
  struct k7_pipeline_s *p=arg;uint64_t next=monotonic_us();
  while(!atomic_load(&p->control_stop))
    {
      uint64_t now=monotonic_us();
      if(now<next)
        {
          uint64_t wait=next-now;
          struct timespec pause={(time_t)(wait/1000000),(long)(wait%1000000)*1000};
          (void)nanosleep(&pause,NULL);continue;
        }
      control_once(p);
      uint64_t after=monotonic_us();next+=20000;
      if(next<=after)next=after+20000; /* skip missed slots; never burst */
    }
  return NULL;
}
static int control_start(struct k7_pipeline_s *p)
{
  pthread_attr_t a;int e=pthread_attr_init(&a);if(e)return e;
  e=pthread_attr_setstacksize(&a,16384);
#ifdef __NuttX__
  if(!e)e=pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);
  if(!e)e=pthread_attr_setschedpolicy(&a,SCHED_FIFO);
  struct sched_param priority={.sched_priority=80};
  if(!e)e=pthread_attr_setschedparam(&a,&priority);
#if defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 1
  cpu_set_t cpus;CPU_ZERO(&cpus);
#if CONFIG_SMP_NCPUS > 3
  CPU_SET(3,&cpus);
#else
  CPU_SET(1,&cpus);
#endif
  if(!e)e=pthread_attr_setaffinity_np(&a,sizeof(cpus),&cpus);
#endif
#endif
  if(!e)e=pthread_create(&p->control_thread,&a,control_worker,p);
  pthread_attr_destroy(&a);if(!e)p->control_started=true;return e;
}
static void clock_observation(struct k7_pipeline_s*p,const struct k7_faces_s*faces,
                              unsigned sequence,uint64_t sample,
                              struct k7_track_result_s*saved)
{
  if(p->voice_control)pthread_mutex_lock(&g_voice_lock);
  pthread_mutex_lock(&p->control_lock);
  uint64_t now=monotonic_us();struct k7_track_result_s r;
  bool halt=atomic_load(&g_track_halt)||atomic_load(&p->motion_done)||p->stats.track_error;
  if(photo_observation_only(p))k7_track_observe(&p->controller,faces,now,sample,halt,&r);
  else k7_track_measure(&p->controller,faces,now,sample,halt,&r);
  track_snapshot_update(&r,sequence,sample,now);
  if(halt)track_snapshot_clear();
  p->stats.track_observations++;p->stats.track_states[r.state]++;
  *saved=r;unsigned tx=p->stats.track_tx;
  uint64_t ticks=p->control_ticks,gap=p->control_max_gap_us;
  pthread_mutex_unlock(&p->control_lock);
  if(p->voice_control)pthread_mutex_unlock(&g_voice_lock);
  if(r.has_face)printf("TRACK BOX q=%u x=%.1f y=%.1f w=%.1f h=%.1f edge=%u\n",
    sequence,r.box.x,r.box.y,r.box.width,r.box.height,r.edges);
  printf("TRACK q=%u s=%d dx=%.1f dy=%.1f x=%d y=%d tx=%u age=%u\n",
    sequence,r.state,r.dx,r.dy,r.x,r.y,tx,(unsigned)(now-sample));
  if(sequence%30==0)printf("TRACK CLOCK ticks=%llu tx=%u max_gap_us=%llu period_us=20000\n",
    (unsigned long long)ticks,tx,(unsigned long long)gap);
}
#endif
static void track_observation(struct k7_pipeline_s *p,
                              const struct k7_faces_s *faces,
                              unsigned int sequence, uint64_t sample, struct k7_track_result_s *saved)
{
#ifdef K7_TRACK_CONTROL_CLOCK
  if(p->control_enabled){clock_observation(p,faces,sequence,sample,saved);return;}
#endif
  struct k7_track_result_s r;
  int old_x = p->controller.sent_x, old_y = p->controller.sent_y;
  bool halt = atomic_load(&g_track_halt) ||
              atomic_load(&p->motion_done) || p->stats.track_error;
  uint64_t now = monotonic_us();
  if (photo_observation_only(p))
    k7_track_observe(&p->controller,faces,now,sample,halt,&r);
  else
    k7_track_step(&p->controller, faces, now, sample, halt, &r);
  track_snapshot_update(&r,sequence,sample,now);
  if (atomic_load(&g_track_halt) || atomic_load(&p->motion_done))
    track_snapshot_clear();
  p->stats.track_observations++;
  p->stats.track_states[r.state]++;
  if (r.update)
    {
      p->stats.track_updates++;
      unsigned int step = abs(r.x-old_x) > abs(r.y-old_y) ?
                          abs(r.x-old_x) : abs(r.y-old_y);
      if (step > p->stats.track_max_step) p->stats.track_max_step = step;
      if (p->gimbal_fd >= 0)
        {
#ifdef CONFIG_EXAMPLES_K7HOST_PHOTO_OBSERVE
          /* Serialize admission of this bounded UART pair with voice mode
           * requests. An already admitted pair may finish (50 ms maximum);
           * after PHOTO/STOP returns, no later pair can be admitted. */
          if (p->voice_control) pthread_mutex_lock(&g_voice_lock);
#endif
          /* Recheck after inference. An in-flight UART pair cannot be recalled. */
          if (atomic_load(&g_track_halt) || atomic_load(&p->motion_done) ||
              photo_observation_only(p))
            {
              p->controller.sent_x=old_x;p->controller.sent_y=old_y;
#ifdef CONFIG_EXAMPLES_K7HOST_PHOTO_OBSERVE
              r.x=old_x;r.y=old_y;r.update=false;
#endif
            }
          else
            {
              int ret = gimbal_link_send(p->gimbal_fd, r.x, r.y);
              if (ret < 0)
                {
                  p->stats.track_errors++; p->stats.track_error=ret;
                  p->controller.sent_x=old_x; p->controller.sent_y=old_y;
                }
              else p->stats.track_tx++;
            }
#ifdef CONFIG_EXAMPLES_K7HOST_PHOTO_OBSERVE
          if (p->voice_control) pthread_mutex_unlock(&g_voice_lock);
#endif
        }
    }
  *saved=r;
  p->stats.track_x=p->controller.sent_x;
  p->stats.track_y=p->controller.sent_y;
  /* A separate short box record lets calibration distinguish complete
   * faces from clipped detections. It is tied to the same sequence.
   */
  if (r.has_face)
    {
      printf("TRACK BOX q=%u x=%.1f y=%.1f w=%.1f h=%.1f edge=%u\n",
             sequence,r.box.x,r.box.y,r.box.width,r.box.height,r.edges);
      struct timespec pace={0,1000000};
      nanosleep(&pace,NULL);
    }
  /* One short record per inference, useful for direction/calibration evidence. */
  printf("TRACK q=%u s=%d dx=%.1f dy=%.1f x=%d y=%d tx=%u age=%u\n",
         sequence, r.state, r.dx, r.dy, p->stats.track_x, p->stats.track_y,
         p->stats.track_tx, (unsigned int)(now-sample));
}
#endif

/* A slot admitted under lock remains private to this callback until return.
 * finish may cancel pending slots but cannot free/reuse an admitted slot. */
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
static bool stage_cancelled(int ret)
{
  return ret==K7DS_STALE_ERROR || ret==-ECANCELED || ret==-ENODEV;
}
static void *stage_sink_worker(void *arg)
{
  struct k7_pipeline_s *p=arg;
  uint64_t next_present_us=0;
  for (;;)
    {
      struct k7ds_ticket ticket={0};uint64_t published=0;
      pthread_mutex_lock(&p->lock);
      while (!p->stopping && !p->stage_pending)
        pthread_cond_wait(&p->ready,&p->lock);
      if (p->stopping)
        {
          if (p->stage_pending)
            {ticket=p->stage_ticket;p->stage_pending=false;p->stats.sink_dropped++;}
          pthread_mutex_unlock(&p->lock);
          if(ticket.lease)(void)p->sink.stage_discard(p->sink.arg,&ticket);
          break;
        }
      uint64_t now=monotonic_us();
      if(now<next_present_us)
        {
          struct timespec pace={0,(long)(next_present_us-now)*1000};
          pthread_mutex_unlock(&p->lock);
          while(nanosleep(&pace,&pace)<0 && errno==EINTR){}
          continue;
        }
      ticket=p->stage_ticket;published=p->stage_published_us;
      p->stage_pending=false;p->stage_admitted=true;
      next_present_us=now+K7_SINK_PERIOD_US;
      pthread_mutex_unlock(&p->lock);
      uint64_t started=monotonic_us();
      int ret=p->sink.stage_commit(p->sink.arg,&ticket);
      uint64_t ended=monotonic_us();
      if(ret>0)ret=-EIO;
      /* Adapter STOP may reject before native commit was called. Retire only
       * this sink-owned, still-unsubmitted ticket. Native rejects stale or
       * frozen/pending hardware images, so this cannot free an uncertain flip. */
      if(stage_cancelled(ret))(void)p->sink.stage_discard(p->sink.arg,&ticket);
      pthread_mutex_lock(&p->lock);
      p->stage_admitted=false;p->stats.sink_calls++;
      p->stats.sink_us+=ended-started;
      if(ended-started>p->stats.max_sink_us)p->stats.max_sink_us=ended-started;
      if(ended-published>p->stats.max_sink_age_us)p->stats.max_sink_age_us=ended-published;
      if(!ret)p->stats.sink_presented++;
      else if(stage_cancelled(ret))p->stats.sink_dropped++;
      else
        {if(!p->stats.sink_error)p->stats.sink_error=ret;p->stopping=true;}
      if(ret==-ENODEV)p->stopping=true;
      pthread_cond_broadcast(&p->ready);pthread_mutex_unlock(&p->lock);
    }
  return NULL;
}
#endif
static void *sink_worker(void *arg)
{
  struct k7_pipeline_s *p=arg;
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
  if(p->staged)return stage_sink_worker(arg);
#endif
  uint64_t next_present_us=0;
  for (;;)
    {
      pthread_mutex_lock(&p->lock);
      int chosen=-1;
      while (!p->stopping)
        {
          for (unsigned int i=0;i<K7_RGB_SLOTS;i++)
            if (p->rgb_slots[i].state==SLOT_PENDING) chosen=(int)i;
          if (chosen>=0) break;
          pthread_cond_wait(&p->ready,&p->lock);
        }
      if (p->stopping)
        {
          for (unsigned int i=0;i<K7_RGB_SLOTS;i++)
            if (p->rgb_slots[i].state==SLOT_PENDING)
              { p->rgb_slots[i].state=SLOT_FREE; p->stats.sink_dropped++; }
          pthread_mutex_unlock(&p->lock);
          break;
        }
      uint64_t now=monotonic_us();
      if (now<next_present_us)
        {
          uint64_t wait=next_present_us-now;
          pthread_mutex_unlock(&p->lock);
          struct timespec pace={0,(long)wait*1000};
          while (nanosleep(&pace,&pace)<0 && errno==EINTR) {}
          continue;
        }
      struct rgb_slot_s *slot=&p->rgb_slots[chosen];
      slot->state=SLOT_DECODING; /* exclusively presented, not decoder-owned */
      next_present_us=now+K7_SINK_PERIOD_US;
      pthread_mutex_unlock(&p->lock);
      uint64_t started=monotonic_us();
      int ret=p->sink.present(p->sink.arg,slot->data,slot->result.width,
                              slot->result.height,(size_t)slot->result.width*3,
                              slot->result.bytes);
      uint64_t ended=monotonic_us();
      if (ret>0) ret=-EIO;
      pthread_mutex_lock(&p->lock);
      p->stats.sink_calls++;
      p->stats.sink_us+=ended-started;
      if (ended-started>p->stats.max_sink_us) p->stats.max_sink_us=ended-started;
      if (ended-slot->published_us>p->stats.max_sink_age_us)
        p->stats.max_sink_age_us=ended-slot->published_us;
      if (!ret) p->stats.sink_presented++;
      else
        {
          if (!p->stats.sink_error) p->stats.sink_error=ret;
          p->stopping=true;
        }
      slot->state=SLOT_FREE;
      pthread_cond_broadcast(&p->ready);
      pthread_mutex_unlock(&p->lock);
    }
  return NULL;
}

static void *decode_worker(void *arg)
{
  struct k7_pipeline_s *p = arg;
  uint64_t next_present_us = 0;
  for (;;)
    {
      struct k7_jpeg_result_s result;
      int chosen = -1;
      bool latest = p->sink.present != NULL;
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
      latest = latest || p->yunet != NULL;
#endif
      pthread_mutex_lock(&p->lock);
      while (!p->pending && !p->stopping)
        pthread_cond_wait(&p->ready, &p->lock);
      if (p->sink.present && p->stopping)
        {
          for (unsigned int i=0;i<K7_PIPE_SLOTS;i++)
            if (p->slots[i].state==SLOT_PENDING)
              { p->slots[i].state=SLOT_FREE; p->stats.dropped++; p->pending--; }
        }
      if (!p->pending && p->stopping)
        {
          pthread_mutex_unlock(&p->lock);
          break;
        }
      if (p->sink.present && monotonic_us() < next_present_us)
        {
          /* Wait before selecting input so a paced sink receives the latest
           * frame, not a frame held stale during the throttle interval. */
          uint64_t now=monotonic_us();
          uint64_t wait=now<next_present_us ? next_present_us-now : 0;
          pthread_mutex_unlock(&p->lock);
          struct timespec pace={0,(long)wait*1000};
          while (nanosleep(&pace,&pace)<0 && errno==EINTR) {}
          continue;
        }

      for (unsigned int i = 0; i < K7_PIPE_SLOTS; i++)
        if (p->slots[i].state == SLOT_PENDING &&
            (chosen < 0 ||
             (latest ? p->slots[i].sequence > p->slots[chosen].sequence :
                       p->slots[i].sequence < p->slots[chosen].sequence)))
          chosen = i;
      /* Tracking needs recent observations. Discard older pending frames
       * when selecting an inference input; never touch WRITING/DECODING.
       * JPEG-only mode preserves its original FIFO behavior.
       */
      if (latest)
        for (unsigned int i = 0; i < K7_PIPE_SLOTS; i++)
          if ((int)i != chosen && p->slots[i].state == SLOT_PENDING)
            {
              p->slots[i].state = SLOT_FREE;
              p->pending--;
              p->stats.dropped++;
            }
      struct slot_s *slot = &p->slots[chosen];
      slot->state = SLOT_DECODING;
      p->pending--;
      int rgb_chosen=-1;
      if (p->sink.present)
        {
          /* One display owner + one decoder leaves a free or pending slot.
           * Reclaim pending only; never write the buffer held by the sink. */
          for (unsigned int i=0;i<K7_RGB_SLOTS;i++)
            if (p->rgb_slots[i].state==SLOT_FREE) { rgb_chosen=(int)i; break; }
          if (rgb_chosen<0)
            for (unsigned int i=0;i<K7_RGB_SLOTS;i++)
              if (p->rgb_slots[i].state==SLOT_PENDING)
                { rgb_chosen=(int)i; p->stats.sink_dropped++; break; }
          if (rgb_chosen<0)
            { /* Invariant failure must stop, never reuse an owned buffer. */
              p->stats.sink_error=-EIO; p->stopping=true;
              slot->state=SLOT_FREE; p->stats.dropped++;
              pthread_cond_broadcast(&p->ready);
              pthread_mutex_unlock(&p->lock); break;
            }
          p->rgb_slots[rgb_chosen].state=SLOT_WRITING;
        }
      /* Start-to-start decode budget; presentation runs independently. */
      if (p->sink.present)
        next_present_us=monotonic_us()+K7_SINK_PERIOD_US;
      pthread_mutex_unlock(&p->lock);

      /* The slot remains exclusively owned by this worker until FREE.
       * Neither the JPEG copy nor decode runs under the queue mutex.
       * Optional delay is only for reproducible backpressure testing.
       */
      if (p->delay_ms)
        {
          struct timespec delay = {0, (long)p->delay_ms * 1000000};
          while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
        }
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
      struct k7ds_epoch stage_epoch={0};
      struct k7ds_ticket prepared_ticket={0};
      bool discard_prepared=false;
      int stage_ret=0;
      if(p->staged)
        stage_ret=p->sink.stage_snapshot(p->sink.arg,p->sink.stage_lease,&stage_epoch);
#endif
      int ret = k7_jpeg_decode(slot->data, slot->bytes, p->scale,
                               rgb_chosen<0 ? p->rgb : p->rgb_slots[rgb_chosen].data,
                               rgb_chosen<0 ? p->rgb_capacity : K7_RGB_BYTES, &result);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
      if(p->staged && !ret && !stage_ret)
        stage_ret=p->sink.stage_prepare(p->sink.arg,p->rgb_slots[rgb_chosen].data,
          result.width,result.height,(size_t)result.width*3,result.bytes,
          &stage_epoch,slot->sequence,&prepared_ticket);
      if(stage_ret>0)stage_ret=-EIO;
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
      struct k7_faces_s faces = {0};
      int face_ret = 0;
      if (ret == 0 && p->yunet)
        face_ret = k7_yunet_detect(p->yunet, p->rgb, result.bytes, &faces);
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
      if (p->tracking)
        {
          voice_mode_apply(p);
          struct k7_track_result_s observed;
          track_observation(p, ret == 0 && face_ret == 0 ? &faces : NULL,
                            slot->sequence, slot->published_us, &observed);
#ifdef K7_TRACK_BIG_VISION
          if(p->stats.track_observations%30==0)
            printf("TRACK TIMING q=%u jpeg_us=%llu infer_us=%llu\n",
              slot->sequence,(unsigned long long)result.usec,
              (unsigned long long)faces.usec);
#endif
          if(p->photo && (!p->voice_control || atomic_load(&g_voice_mode)==2))
            {
              unsigned photo_faces=observed.has_face ? k7_photo_target_count(&faces,&observed.box) : faces.count;
              if(photo_faces<faces.count && slot->sequence%30==0)
                printf("PHOTO TARGET q=%u raw_faces=%u eligible=%u background_ignored=%u\n",
                  slot->sequence,faces.count,photo_faces,faces.count-photo_faces);
              k7_photo_process(p->photo,slot->data,slot->bytes,slot->sequence,
                slot->published_us,&observed,photo_faces,atomic_load(&g_track_halt)||atomic_load(&p->motion_done));
              uint32_t completed_epoch=0;unsigned completed_views=0;
              if(p->voice_control && !k7_photo_native_status(&completed_epoch,&completed_views) && completed_views==7)
                {
                  /* Preserve the immutable RAM set; prevent future pose/target work.
                   * Applied hold is confirmed separately before upload admission. */
                  k7_pipeline_halt();
                  printf("PHOTO complete e=%u views=7 hold_requested=1 upload_started=0\n",completed_epoch);
                }
            }
          /* Submit a private RGB copy; output never runs in this worker. */
          if (p->preview && ret == 0)
            k7_preview_queue_submit(p->preview,p->rgb,result.bytes,slot->sequence);
        }
#endif
      uint64_t age = monotonic_us() - slot->published_us;

      pthread_mutex_lock(&p->lock);
      if (rgb_chosen>=0)
        {
          struct rgb_slot_s *rgb_slot=&p->rgb_slots[rgb_chosen];
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
          if(p->staged)
            {
              /* CPU RGB ownership ends after synchronous prepare. Ticket
               * publication is separate; never overwrite a pending DMA image. */
              rgb_slot->state=SLOT_FREE;
              if(!ret && !stage_ret && !p->stopping && !p->stage_pending)
                {
                  p->stage_ticket=prepared_ticket;
                  p->stage_published_us=slot->published_us;p->stage_pending=true;
                  pthread_cond_broadcast(&p->ready);
                }
              else
                {
                  if(!ret)p->stats.sink_dropped++;
                  discard_prepared=prepared_ticket.lease!=0;
                  if(!ret && stage_ret && stage_ret!=-EAGAIN && !stage_cancelled(stage_ret))
                    {if(!p->stats.sink_error)p->stats.sink_error=stage_ret;p->stopping=true;}
                  if(stage_ret==-ENODEV)p->stopping=true;
                  pthread_cond_broadcast(&p->ready);
                }
            }
          else
#endif
          if (!ret && !p->stopping)
            {
              /* Keep exactly one pending RGB, even if display is blocked. */
              for (unsigned int i=0;i<K7_RGB_SLOTS;i++)
                if (p->rgb_slots[i].state==SLOT_PENDING)
                  { p->rgb_slots[i].state=SLOT_FREE; p->stats.sink_dropped++; }
              rgb_slot->result=result;
              rgb_slot->published_us=slot->published_us;
              rgb_slot->state=SLOT_PENDING;
              pthread_cond_broadcast(&p->ready);
            }
          else
            {
              rgb_slot->state=SLOT_FREE;
              if (!ret) p->stats.sink_dropped++;
            }
        }
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
      p->stats.last_face_count = 0;
      memset(&p->stats.primary, 0, sizeof(p->stats.primary));
      if (ret == 0 && p->yunet)
        {
          if (face_ret < 0) p->stats.face_failures++;
          else
            {
              p->stats.face_inferences++;
              p->stats.inference_us += faces.usec;
              if (faces.usec > p->stats.max_inference_us)
                p->stats.max_inference_us = faces.usec;
              p->stats.last_face_count = faces.count;
              if (faces.count) p->stats.face_frames++;
              float best = -1;
              for (unsigned int i = 0; i < faces.count; i++)
                {
                  struct k7_face_s *f = &faces.face[i];
                  float dx = f->x + f->width / 2 - 80;
                  float dy = f->y + f->height / 2 - 60;
                  float distance = sqrtf(dx * dx + dy * dy) / 100;
                  float priority = f->width * f->height * f->score *
                    (1.25f - 0.25f * fminf(distance, 1));
                  if (priority > best)
                    { best = priority; p->stats.primary = *f; }
                }
            }
        }
#endif
      p->stats.decode_us += result.usec;
      if (result.usec > p->stats.max_decode_us)
        p->stats.max_decode_us = result.usec;
      if (age > p->stats.max_age_us) p->stats.max_age_us = age;
      if (ret == 0)
        {
          p->stats.decoded++;
          p->stats.last_sequence = slot->sequence;
          p->stats.width = result.width;
          p->stats.height = result.height;
          p->stats.bytes = result.bytes;
        }
      else
        {
          p->stats.failed++;
          p->stats.last_error = ret;
          /* Failed decode may have overwritten part of RGB. */
          p->stats.bytes = 0;
        }
      slot->state = SLOT_FREE;
      pthread_mutex_unlock(&p->lock);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
      if(discard_prepared)
        (void)p->sink.stage_discard(p->sink.arg,&prepared_ticket);
#endif
    }
  return NULL;
}

static int pipeline_start(struct k7_pipeline_s **out, size_t jpeg_capacity,
                      unsigned int scale, unsigned int delay_ms,
                      uint8_t *rgb, size_t rgb_capacity, bool detect_faces,
                      const struct k7_track_config_s *track,
                      const struct k7_pipeline_sink_s *sink, bool sidecar)
{
  struct k7_pipeline_s *p;
  pthread_attr_t attr;
  struct sched_param scheduling = {0};
  int ret;
  if (!out) return -EINVAL;
  *out = NULL;
  if (sink && (!sink->present || scale!=1 || detect_faces || track)) return -EINVAL;
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
  if(sink)
    {
      bool any=sink->stage_lease || sink->stage_snapshot || sink->stage_prepare ||
        sink->stage_commit || sink->stage_discard || sink->stage_quiesce;
      bool all=sink->stage_lease && sink->stage_snapshot && sink->stage_prepare &&
        sink->stage_commit && sink->stage_discard && sink->stage_quiesce;
      if(any && (!all || !sidecar))return -EINVAL;
    }
#endif
  if (!jpeg_capacity || jpeg_capacity > 4 * 1024 * 1024 ||
      (!rgb && (!sidecar || rgb_capacity)) ||
      (scale != 1 && scale != 2 && scale != 4 && scale != 8) ||
      delay_ms > 200 || (rgb && rgb_capacity < (640 / scale) * (480 / scale) * 3))
    return -EINVAL;

#ifndef CONFIG_EXAMPLES_K7HOST_TRACK
  if (track) return -ENOTSUP;
#endif
  if (track && !detect_faces) return -EINVAL;
  if (detect_faces && scale != 4) return -EINVAL;
#ifndef CONFIG_EXAMPLES_K7HOST_YUNET
  if (detect_faces) return -ENOTSUP;
#endif
  p = calloc(1, sizeof(*p));
  if (!p) return -ENOMEM;
  p->storage = malloc(jpeg_capacity * K7_PIPE_SLOTS);
  if (!p->storage) { free(p); return -ENOMEM; }
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  p->gimbal_fd = -1;
  atomic_init(&p->motion_done, false);
  if (track)
    {
      track_snapshot_clear();
      int x=0, y=0;
      p->tracking=true;
#ifdef K7_TRACK_CONTROL_CLOCK
      p->control_enabled=track->transmit;
      atomic_init(&p->control_stop,false);
      if(p->control_enabled)
        {
          ret=pthread_mutex_init(&p->control_lock,NULL);
          if(ret)goto free_storage;
          p->control_lock_ready=true;
        }
#endif
      p->voice_control=track->voice_control;
      if(track->photo || track->voice_control) {
        p->photo=track->voice_control?k7_photo_create_native():k7_photo_create();
        if(!p->photo){ret=ENOMEM;goto free_storage;}
      }
      atomic_store(&g_track_halt, track->voice_control);
      if (track->voice_control) atomic_store(&g_voice_mode, 0);
      if (track->transmit)
        {
          p->gimbal_fd=gimbal_link_open(&x, &y);
          if (p->gimbal_fd < 0) { ret=-p->gimbal_fd; goto free_storage; }
        }
      ret=k7_track_init(&p->controller, track, x, y);
      if (ret < 0) { ret=-ret; goto free_storage; }
      p->stats.track_x=x; p->stats.track_y=y;
      printf("TRACK mode=%s limit=%d sign=%d,%d initial=%d,%d\n",
             track->transmit ? "run" : "dry",track->limit,
             track->sign_x,track->sign_y,x,y);
      printf("TRACK axes=%s axis_extent_required=1\n",
             track->axes==TRACK_X ? "x" : track->axes==TRACK_Y ? "y" : "xy");
    }
#endif
  p->stride = jpeg_capacity;
  p->rgb = rgb;
  p->rgb_capacity = rgb_capacity;
  p->scale = scale;
  p->delay_ms = delay_ms;
  if (sink)
    {
      p->sink=*sink;
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
      p->staged=sink->stage_snapshot!=NULL;
#endif
      p->sink_storage=malloc(K7_RGB_BYTES*K7_RGB_SLOTS);
      if (!p->sink_storage) { ret=ENOMEM; goto free_storage; }
      for (unsigned int i=0;i<K7_RGB_SLOTS;i++)
        p->rgb_slots[i].data=p->sink_storage+i*K7_RGB_BYTES;
    }
  for (unsigned int i = 0; i < K7_PIPE_SLOTS; i++)
    p->slots[i].data = p->storage + i * jpeg_capacity;

#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
  if (detect_faces)
    {
      p->yunet = k7_yunet_create();
      if (!p->yunet) { ret = ENOMEM; goto free_storage; }
    }
#endif
  ret = pthread_mutex_init(&p->lock, NULL);
  if (ret) goto free_storage;
  ret = pthread_cond_init(&p->ready, NULL);
  if (ret) goto destroy_mutex;
  ret = pthread_attr_init(&attr);
  if (ret) goto destroy_cond;
  ret = pthread_attr_setstacksize(&attr, 16384);
  if (ret) goto destroy_attr;
#ifdef __NuttX__
  /* Provisioning runs at 85 on CPU0. A continuously ready FIFO decoder
   * must not starve it; keep a lower fallback priority even with affinity.
   * USB refill remains at 100/192, ASR capture uses CPU1. */
  scheduling.sched_priority = 80;
  ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  if (ret) goto destroy_attr;
  ret = pthread_attr_setschedparam(&attr, &scheduling);
  if (ret) goto destroy_attr;
  ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  if (ret) goto destroy_attr;
#if defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 1
  {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
#if CONFIG_SMP_NCPUS > 2
    CPU_SET(2, &cpus);
#else
    CPU_SET(1, &cpus);
#endif
#if defined(K7_TRACK_BIG_VISION) && CONFIG_SMP_NCPUS > 4
    /* The verified RK3576 logical map is A53=0..3, A72=4..7.
     * Keep audio/KWS/VAD affinity intact; move only vision to A72 CPU4.
     * Its independent full-resolution decoder stays on CPU5. */
    if(track && detect_faces) {CPU_ZERO(&cpus);CPU_SET(4,&cpus);}
#endif
    /* The independent full-resolution LCD decoder must not share the
     * existing vision/inference CPU2 when the configured SMP set permits. */
#if CONFIG_SMP_NCPUS > 5
    if (sidecar) { CPU_ZERO(&cpus); CPU_SET(5,&cpus); }
#elif CONFIG_SMP_NCPUS > 3
    if (sidecar) { CPU_ZERO(&cpus); CPU_SET(3,&cpus); }
#endif
    ret = pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    if (ret) goto destroy_attr;
  }
#endif
#else
  (void)scheduling;
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  if (track && track->preview)
    {
      ret=k7_preview_queue_start(&p->preview);
      if (ret < 0) { ret=-ret; goto destroy_attr; }
    }
#endif
  p->started_us = monotonic_us();
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK) && defined(K7_TRACK_CONTROL_CLOCK)
  if(p->control_enabled)
    {
      ret=control_start(p);
      if(ret)
        {
          int stopped=k7_preview_queue_stop(p->preview);
          if(stopped<0)return stopped;
          goto destroy_attr;
        }
    }
#endif
  ret = pthread_create(&p->thread, &attr, decode_worker, p);
  if (!ret && sink)
    {
#if defined(__NuttX__) && defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 3
      cpu_set_t cpus;
      CPU_ZERO(&cpus);
#if CONFIG_SMP_NCPUS > 4
      CPU_SET(4,&cpus);
#ifdef K7_TRACK_BIG_VISION
      /* LCD commit waits for the display engine; leave the A72 for vision.
       * This applies only to the independent LCD sidecar, not other sinks. */
      if(sidecar) {CPU_ZERO(&cpus);CPU_SET(2,&cpus);}
#endif
#else
      CPU_SET(3,&cpus);
#endif
      ret=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);
#endif
      if (!ret) ret=pthread_create(&p->sink_thread,&attr,sink_worker,p);
      if (!ret) p->sink_thread_started=true;
      else
        {
          pthread_mutex_lock(&p->lock);
          p->stopping=true;
          pthread_cond_broadcast(&p->ready);
          pthread_mutex_unlock(&p->lock);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
          if(p->staged)(void)p->sink.stage_quiesce(p->sink.arg,p->sink.stage_lease);
#endif
          int joined=pthread_join(p->thread,NULL);
          if (joined) { pthread_attr_destroy(&attr); return -joined; }
        }
    }
  pthread_attr_destroy(&attr);
  if (ret)
    {
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK) && defined(K7_TRACK_CONTROL_CLOCK)
      if(p->control_started)
        {
          atomic_store(&p->control_stop,true);
          int joined=pthread_join(p->control_thread,NULL);
          if(joined)return -joined;
          p->control_started=false;
        }
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
      int stopped=k7_preview_queue_stop(p->preview);
      if (stopped < 0) return stopped;
#endif
      goto destroy_cond;
    }
  *out = p;
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  if (p->voice_control)
    {
      pthread_mutex_lock(&g_voice_lock);
      g_voice_active = true;
      pthread_mutex_unlock(&g_voice_lock);
      puts("VOICE SESSION ready mode=0; awaiting explicit command");
    }
#endif
  return 0;

destroy_attr:
  pthread_attr_destroy(&attr);
destroy_cond:
  pthread_cond_destroy(&p->ready);
destroy_mutex:
  pthread_mutex_destroy(&p->lock);
free_storage:
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK) && defined(K7_TRACK_CONTROL_CLOCK)
  if(p->control_lock_ready)pthread_mutex_destroy(&p->control_lock);
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
  k7_yunet_destroy(p->yunet);
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  k7_photo_destroy(p->photo);
  gimbal_link_close(p->gimbal_fd);
#endif
  free(p->sink_storage);
  free(p->storage);
  free(p);
  return -ret;
}

int k7_pipeline_start(struct k7_pipeline_s **out, size_t jpeg_capacity,
                      unsigned int scale, unsigned int delay_ms,
                      uint8_t *rgb, size_t rgb_capacity, bool detect_faces,
                      const struct k7_track_config_s *track)
{
  return pipeline_start(out,jpeg_capacity,scale,delay_ms,rgb,rgb_capacity,
                        detect_faces,track,NULL,false);
}
int k7_pipeline_start_sink(struct k7_pipeline_s **out, size_t jpeg_capacity,
                          uint8_t *rgb, size_t rgb_capacity,
                          const struct k7_pipeline_sink_s *sink)
{
  if (!sink) return -EINVAL;
  return pipeline_start(out,jpeg_capacity,1,0,rgb,rgb_capacity,false,NULL,sink,false);
}
int k7_pipeline_start_lcd_preview(struct k7_pipeline_s **out,
                                 size_t jpeg_capacity,
                                 const struct k7_pipeline_sink_s *sink)
{
  if (!sink) return -EINVAL;
  return pipeline_start(out,jpeg_capacity,1,0,NULL,0,false,NULL,sink,true);
}
int k7_pipeline_sink_status(struct k7_pipeline_s *p)
{
  if (!p) return -EINVAL;
  pthread_mutex_lock(&p->lock);
  int ret=p->stats.sink_error;
  pthread_mutex_unlock(&p->lock);
  return ret;
}

void k7_pipeline_publish(struct k7_pipeline_s *p,
                         const uint8_t *jpeg, size_t bytes)
{
  int chosen = -1;
  uint64_t now = monotonic_us();
  pthread_mutex_lock(&p->lock);
  p->stats.published++;
  if (p->stopping)
    { p->stats.dropped++; pthread_mutex_unlock(&p->lock); return; }
  /* Only this capture task publishes, so there cannot be another WRITING
   * slot. One decoder owns at most one slot: a free or pending slot exists.
   */
  for (unsigned int i = 0; i < K7_PIPE_SLOTS; i++)
    if (p->slots[i].state == SLOT_FREE) { chosen = i; break; }
  if (chosen < 0)
    {
      for (unsigned int i = 0; i < K7_PIPE_SLOTS; i++)
        if (p->slots[i].state == SLOT_PENDING &&
            (chosen < 0 ||
             p->slots[i].sequence < p->slots[chosen].sequence))
          chosen = i;
    }
  if (!jpeg || bytes > p->stride || bytes < 4 || chosen < 0)
    {
      p->stats.dropped++;
      pthread_mutex_unlock(&p->lock);
      return;
    }
  struct slot_s *slot = &p->slots[chosen];
  if (slot->state == SLOT_PENDING)
    {
      p->pending--;
      p->stats.dropped++;
    }
  slot->state = SLOT_WRITING;
  slot->sequence = p->stats.published;
  pthread_mutex_unlock(&p->lock);

  memcpy(slot->data, jpeg, bytes);
  slot->bytes = bytes;
  slot->published_us = now;

  pthread_mutex_lock(&p->lock);
  if (p->stopping)
    {
      slot->state=SLOT_FREE;
      p->stats.dropped++;
      pthread_mutex_unlock(&p->lock);
      return;
    }
  slot->state = SLOT_PENDING;
  p->pending++;
  if (p->pending > p->stats.peak_pending)
    p->stats.peak_pending = p->pending;
  pthread_cond_broadcast(&p->ready);
  pthread_mutex_unlock(&p->lock);
}

int k7_pipeline_finish(struct k7_pipeline_s *p,
                       struct k7_pipeline_stats_s *stats)
{
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  if (p->voice_control)
    {
      pthread_mutex_lock(&g_voice_lock);
      g_voice_active = false;
      atomic_store(&g_voice_mode, 0);
      atomic_store(&g_track_halt, true);
      pthread_mutex_unlock(&g_voice_lock);
    }
#endif
  pthread_mutex_lock(&p->lock);
  p->stopping = true;
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  atomic_store(&p->motion_done, true);
#ifdef K7_TRACK_CONTROL_CLOCK
  if(p->control_enabled)atomic_store(&p->control_stop,true);
#endif
#endif
  pthread_cond_broadcast(&p->ready);
  pthread_mutex_unlock(&p->lock);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
  if(p->staged)
    {
      int closed=p->sink.stage_quiesce(p->sink.arg,p->sink.stage_lease);
      if(closed && !stage_cancelled(closed))
        {
          pthread_mutex_lock(&p->lock);
          if(!p->stats.sink_error)p->stats.sink_error=closed;
          pthread_mutex_unlock(&p->lock);
        }
    }
#endif
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK) && defined(K7_TRACK_CONTROL_CLOCK)
  if(p->control_started)
    {
      int joined=pthread_join(p->control_thread,NULL);
      if(joined)return -joined;
      p->control_started=false;
    }
#endif
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
  int ret=0;
  if(!p->staged || !p->decoder_joined)ret=pthread_join(p->thread,NULL);
  if(!ret && p->staged)p->decoder_joined=true;
#else
  int ret = pthread_join(p->thread, NULL);
#endif
  /* A failed join must never free memory a live decoder could access. */
  if (ret) return -ret;
  if (p->sink_thread_started)
    {
      ret=pthread_join(p->sink_thread,NULL);
      if (ret) return -ret;
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
      if(p->staged)p->sink_thread_started=false;
#endif
    }
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  if (p->tracking) track_snapshot_clear();
  ret=k7_preview_queue_stop(p->preview);
  if (ret < 0) return ret;
#endif
  p->stats.elapsed_us = monotonic_us() - p->started_us;
  *stats = p->stats;
  pthread_cond_destroy(&p->ready);
  pthread_mutex_destroy(&p->lock);
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK) && defined(K7_TRACK_CONTROL_CLOCK)
  if(p->control_lock_ready)pthread_mutex_destroy(&p->control_lock);
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
  k7_yunet_destroy(p->yunet);
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  k7_photo_destroy(p->photo);
  gimbal_link_close(p->gimbal_fd);
#endif
  free(p->sink_storage);
  free(p->storage);
  free(p);
  return 0;
}

