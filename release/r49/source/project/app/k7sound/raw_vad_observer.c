/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "raw_vad_observer.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Owner and segment disambiguate reset model window IDs. RAW source tag is
 * deliberately distinct from any future calibrated/qualified transform. */
#define RAW_SOURCE UINT32_C(0x52415731)
struct k7_raw_vad {
  struct k7_voice_session *session;
  struct k7_vad *model;
  uint64_t owner, next_frame, next_sample, previous_after_us;
  uint32_t epoch;
  struct k7_raw_vad_stats stats;
};
static int monotonic_us(uint64_t *out)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
      t.tv_nsec >= 1000000000L || (uint64_t)t.tv_sec > UINT64_MAX/1000000u)
    return -EIO;
  uint64_t seconds=(uint64_t)t.tv_sec*1000000u;
  if (seconds > UINT64_MAX-(uint64_t)t.tv_nsec/1000u) return -EOVERFLOW;
  *out=seconds+(uint64_t)t.tv_nsec/1000u;return 0;
}
static bool live(const struct k7_voice_session_status *v, uint32_t epoch)
{
  return v->running && v->ready && !v->stop_requested && !v->finished &&
         epoch && v->input_epoch==epoch;
}
static bool fresh(uint64_t current, uint64_t end)
{ return end<=current && current-end<=K7_RAW_VAD_MAX_AGE; }
void k7_raw_vad_invalidate(struct k7_raw_vad *s,enum k7_raw_vad_failure reason)
{
  if (!s) return;
  k7_vad_invalidate(s->model);s->stats.armed=false;s->stats.started=false;
  s->stats.last_failure=reason;
}
static int reject(struct k7_raw_vad *s,enum k7_raw_vad_failure reason,int rc)
{
  k7_raw_vad_invalidate(s,reason);
  if (s->stats.rejected!=UINT64_MAX) s->stats.rejected++;
  return rc;
}
int k7_raw_vad_create(struct k7_voice_session *session,uint64_t owner,
                     struct k7_vad *model,struct k7_raw_vad **out)
{
  if (!out) return -EINVAL;
  *out=NULL;if (!session || !owner || !model) return -EINVAL;
  struct k7_raw_vad *s=calloc(1,sizeof(*s));if (!s) return -ENOMEM;
  s->session=session;s->owner=owner;s->model=model;
  k7_vad_invalidate(model);s->stats.armed=true;*out=s;return 0;
}
int k7_raw_vad_rearm(struct k7_raw_vad *s)
{
  if (!s) return -EINVAL;
  if (s->stats.segments==UINT64_MAX) return -EOVERFLOW;
  k7_vad_invalidate(s->model);s->stats.armed=true;s->stats.started=false;
  return 0;
}
int k7_raw_vad_feed(struct k7_raw_vad *s,const struct k7_voice_frame *f,
                    struct k7_raw_vad_report *out)
{
  if (!s || !f || !out) return -EINVAL;
  if (!s->stats.armed) return -EPIPE;
  if (f->samples!=K7_VOICE_FRAME_SAMPLES || f->flags || !f->epoch ||
      f->first_sample>UINT64_MAX-K7_VOICE_FRAME_SAMPLES)
    return reject(s,K7_RAW_VAD_FRAME,-EINVAL);
  uint64_t end=f->first_sample+K7_VOICE_FRAME_SAMPLES,before_us;
  if (monotonic_us(&before_us) || before_us<s->previous_after_us)
    return reject(s,K7_RAW_VAD_CLOCK,-EIO);
  struct k7_voice_session_status before,after;
  k7_voice_session_status(s->session,&before);
  if (!live(&before,f->epoch)) return reject(s,K7_RAW_VAD_OWNER,-EPIPE);
  if (!fresh(before.microphone_samples,end))
    return reject(s,K7_RAW_VAD_STALE,K7_VOICE_SESSION_STALE);
  if (s->stats.started && (s->epoch!=f->epoch || s->next_sample!=f->first_sample))
    return reject(s,K7_RAW_VAD_CONTINUITY,-EPIPE);
  if (!s->stats.started) {
    if (s->stats.segments==UINT64_MAX) return reject(s,K7_RAW_VAD_EXHAUSTED,-EOVERFLOW);
    struct k7_vad_identity id={s->owner,f->epoch,RAW_SOURCE};
    int rc=k7_vad_reset(s->model,id,0,f->first_sample);
    if (rc) return reject(s,K7_RAW_VAD_MODEL,rc);
    s->epoch=f->epoch;s->next_frame=0;s->next_sample=f->first_sample;
    s->stats.segments++;s->stats.started=true;
  }
  if (s->next_frame==UINT64_MAX || s->stats.frames==UINT64_MAX ||
      s->stats.observations==UINT64_MAX)
    return reject(s,K7_RAW_VAD_EXHAUSTED,-EOVERFLOW);
  struct k7_vad_frame input={{s->owner,s->epoch,RAW_SOURCE},
                            s->next_frame,f->first_sample,f->pcm,f->samples};
  struct k7_vad_observation observation;
  int rc=k7_vad_feed(s->model,&input,&observation);
  if (rc<0) return reject(s,K7_RAW_VAD_MODEL,rc);
  uint64_t after_us;
  if (monotonic_us(&after_us) || after_us<before_us)
    return reject(s,K7_RAW_VAD_CLOCK,-EIO);
  k7_voice_session_status(s->session,&after);
  if (!live(&after,s->epoch) || after.microphone_samples<before.microphone_samples)
    return reject(s,K7_RAW_VAD_OWNER,-EPIPE);
  if (!fresh(after.microphone_samples,end) ||
      (rc && !fresh(after.microphone_samples,observation.input_end_sample)))
    return reject(s,K7_RAW_VAD_STALE,K7_VOICE_SESSION_STALE);
  s->previous_after_us=after_us;s->next_frame++;s->next_sample=end;
  s->stats.frames++;s->stats.last_failure=K7_RAW_VAD_OK;
  if (!rc) return 0;
  s->stats.observations++;s->stats.last_window_end=observation.input_end_sample;
  s->stats.last_completion_end=end;
  *out=(struct k7_raw_vad_report){
    .model=observation,.owner=s->owner,.segment=s->stats.segments,
    .local_frame_ordinal=input.frame_id,
    .completion_frame_first=f->first_sample,.completion_frame_end=end,
    .microphone_before=before.microphone_samples,.microphone_after=after.microphone_samples,
    .monotonic_before_us=before_us,.monotonic_after_us=after_us,
    .input_epoch=s->epoch,.tx_generation_before=before.tx_generation,
    .tx_generation_after=after.tx_generation,.tx_state_before=(unsigned)before.tx_state,
    .tx_state_after=(unsigned)after.tx_state,.raw=1,.clean=0,.qualified=0,
    .reply_binding_verified=0};
  return 1;
}
int k7_raw_vad_get_stats(const struct k7_raw_vad *s,struct k7_raw_vad_stats *out)
{ if (!s || !out) return -EINVAL;*out=s->stats;return 0; }
void k7_raw_vad_destroy(struct k7_raw_vad *s)
{ if (s) { k7_vad_invalidate(s->model);memset(s,0,sizeof(*s));free(s); } }
