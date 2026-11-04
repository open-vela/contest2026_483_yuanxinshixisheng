#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "vv_barge_input.h"
#include "vv_dialogue.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define CAPTURE_SAMPLES 160000u
enum state { HISTORY, PENDING, CANCELLED, CLAIMED, FAILED };
struct vv_barge_input {
  pthread_mutex_t lock;
  struct vv_barge_authority auth;
  struct k7_voice_frame frames[VV_BARGE_STAGING_FRAMES];
  struct vv_confirmed_wake_context context;
  struct vv_barge_ticket ticket;
  struct vv_wake_request request;
  enum state state;
  enum vv_barge_material material;
  unsigned head,count;
  uint64_t next_sample,taken_next,last_ms;
  bool have_frame,clock_seen,source_live,tail_deferred;
  int failure;
};
static atomic_uint_fast64_t ticket_sequence;
static bool same(const struct vv_confirmed_wake_context*a,
                 const struct vv_confirmed_wake_context*b) {
  return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&
    a->model_epoch==b->model_epoch&&a->mode_epoch==b->mode_epoch&&
    a->clean_epoch==b->clean_epoch&&a->alignment_epoch==b->alignment_epoch&&
    a->asr_epoch==b->asr_epoch&&a->scope_incarnation==b->scope_incarnation&&
    a->lease_serial==b->lease_serial&&a->reply_id==b->reply_id&&
    a->tx_generation==b->tx_generation;
}
static bool valid(const struct vv_confirmed_wake_context*c) {
  return c->owner_epoch&&c->input_epoch&&c->model_epoch&&c->mode_epoch&&
    c->clean_epoch&&c->alignment_epoch&&c->asr_epoch&&c->scope_incarnation&&
    c->lease_serial&&c->reply_id&&c->tx_generation;
}
static int fail(struct vv_barge_input*q,int rc) {
  if(!q->failure)q->failure=rc;
  q->state=FAILED;return q->failure;
}
static void clear(struct vv_barge_input*q) {
  q->head=q->count=0;q->have_frame=q->source_live=q->tail_deferred=false;
  q->state=HISTORY;q->failure=0;q->material=VV_BARGE_RAW_UNKNOWN;
  memset(&q->context,0,sizeof(q->context));
  memset(&q->ticket,0,sizeof(q->ticket));memset(&q->request,0,sizeof(q->request));
  /* Clock watermark deliberately survives reset. */
}
static int clock_check(struct vv_barge_input*q,uint64_t now) {
  if(now==UINT64_MAX||(q->clock_seen&&now<q->last_ms))return fail(q,-ETIMEDOUT);
  q->clock_seen=true;q->last_ms=now;
  unsigned limit=q->state==PENDING?VV_BARGE_TICKET_MS:
                 q->state==CANCELLED?VV_BARGE_RETIRE_MS:VV_BARGE_CAPTURE_MS;
  if(q->ticket.id&&now-q->ticket.created_ms>limit)return fail(q,-ETIMEDOUT);
  return q->failure;
}
/* Successful return always holds the independent authority pin. */
static int enter(struct vv_barge_input*q,const struct vv_confirmed_wake_context*c,
 enum vv_barge_check why,enum vv_barge_material material,uint64_t first,uint64_t next,
 struct vv_barge_observation*o) {
  if(q->failure)return q->failure;
  if(!q->auth.enter||!q->auth.leave)return fail(q,-ENOTSUP);
  if(!valid(c)||material==VV_BARGE_RAW_UNKNOWN||
     (!vv_barge_material_clean(material)&&!vv_barge_material_tail(material)))
    return fail(q,-EPROTO);
  memset(o,0,sizeof(*o));
  int rc=q->auth.enter(q->auth.arg,c,why,material,first,next,o);
  if(rc)return fail(q,rc<0?rc:-EPROTO);
  rc=clock_check(q,o->now_ms);
  if(!rc&&(!o->rx_live||o->photo_busy||o->owner_epoch!=c->owner_epoch||
     o->input_epoch!=c->input_epoch||o->mode_epoch!=c->mode_epoch||
     next>o->microphone_next))rc=fail(q,VV_SCOPE_STALE);
  if(!rc&&o->tx_active&&o->live_tx_generation!=c->tx_generation)
    rc=fail(q,VV_SCOPE_STALE);
  /* Event creation time cannot make an old, backlogged acoustic onset fresh.
   * These are physical 16 kHz MIC indices, independently observed under pin. */
  if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL)&&
     o->microphone_next-next>UINT64_C(16)*VV_BARGE_TICKET_MS)
    rc=fail(q,VV_SCOPE_STALE);
  if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL||q->state==HISTORY)&&
     (!o->tx_active||o->live_tx_generation!=c->tx_generation||
      !vv_barge_material_clean(material)))rc=fail(q,VV_SCOPE_STALE);
  if(!rc&&(why==VV_BARGE_CHECK_CLAIM||why==VV_BARGE_CHECK_CAPTURE)&&
     (o->tx_active||o->live_tx_generation!=c->tx_generation))rc=fail(q,VV_SCOPE_STALE);
  if(rc)q->auth.leave(q->auth.arg);
  return rc;
}
int vv_barge_input_create(const struct vv_barge_authority*a,struct vv_barge_input**out) {
  if(!out)return -EINVAL;
  *out=NULL;if(a&&((a->enter==NULL)!=(a->leave==NULL)))return -EINVAL;
  struct vv_barge_input*q=calloc(1,sizeof(*q));if(!q)return -ENOMEM;
  int rc=pthread_mutex_init(&q->lock,NULL);if(rc){free(q);return -rc;}
  if(a)q->auth=*a;
  *out=q;return 0;
}
int vv_barge_input_destroy(struct vv_barge_input*q) {
  if(!q)return -EINVAL;
  pthread_mutex_lock(&q->lock);bool busy=q->source_live;pthread_mutex_unlock(&q->lock);
  if(busy)return -EBUSY;
  int rc=pthread_mutex_destroy(&q->lock);if(rc)return -rc;
  memset(q,0,sizeof(*q));free(q);return 0;
}
void vv_barge_input_abort(struct vv_barge_input*q) {
  if(q){pthread_mutex_lock(&q->lock);fail(q,-ECANCELED);pthread_mutex_unlock(&q->lock);}
}
int vv_barge_input_reset(struct vv_barge_input*q) {
  if(!q)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->source_live?-EBUSY:0;
  if(!rc)clear(q);
  pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_frame(struct vv_barge_input*q,const struct vv_confirmed_wake_context*c,
 enum vv_barge_material material,const struct k7_voice_frame*f) {
  if(!q||!c||!f)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;struct vv_barge_observation o;
  if(rc)goto done;
  if(f->epoch!=c->input_epoch||f->samples!=320||f->flags||f->first_sample>UINT64_MAX-320){
    rc=fail(q,-EPROTO);goto done;}
  if(q->have_frame&&(!same(c,&q->context)||f->first_sample!=q->next_sample)){
    rc=fail(q,-EPIPE);goto done;}
  rc=enter(q,c,VV_BARGE_CHECK_FRAME,material,f->first_sample,f->first_sample+320,&o);
  if(rc)goto done;
  /* Explicit qualified transition only; once muted, clean old-TX input cannot
   * resurrect the retired generation. The authority proves tail continuity. */
  if(vv_barge_material_tail(q->material)&&material!=q->material)
    rc=fail(q,-EPIPE);
  if(!rc&&q->state==HISTORY&&q->count==VV_BARGE_HISTORY_FRAMES){
    q->head=(q->head+1)%VV_BARGE_STAGING_FRAMES;q->count--;}
  if(!rc&&q->count==VV_BARGE_STAGING_FRAMES)rc=fail(q,-ENOSPC);
  if(!rc){q->frames[(q->head+q->count)%VV_BARGE_STAGING_FRAMES]=*f;q->count++;
    q->context=*c;q->have_frame=true;q->next_sample=f->first_sample+320;q->material=material;}
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_onset(struct vv_barge_input*q,uint64_t first,uint64_t next,
 struct vv_barge_ticket*out) {
  if(out)memset(out,0,sizeof(*out));
  if(!q||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;struct vv_barge_observation o;
  if(rc)goto done;
  if(q->state!=HISTORY){rc=-EBUSY;goto done;}
  if(!q->count||first>=next||next!=q->next_sample||first<VV_BARGE_PRE_ROLL_SAMPLES||
     (first<q->frames[q->head].first_sample||(first-q->frames[q->head].first_sample)%320)||first-VV_BARGE_PRE_ROLL_SAMPLES<q->frames[q->head].first_sample){
    rc=-EINVAL;goto done;}
  rc=enter(q,&q->context,VV_BARGE_CHECK_ONSET,q->material,first,next,&o);if(rc)goto done;
  uint_fast64_t id=atomic_load(&ticket_sequence);
  do {if(id==UINT64_MAX){rc=fail(q,-EOVERFLOW);break;}}
  while(!atomic_compare_exchange_weak(&ticket_sequence,&id,id+1));
  if(!rc){
    uint64_t begin=first-VV_BARGE_PRE_ROLL_SAMPLES;
    while(q->count&&q->frames[q->head].first_sample<begin){q->head=(q->head+1)%VV_BARGE_STAGING_FRAMES;q->count--;}
    q->ticket=(struct vv_barge_ticket){id+1,first,next,o.now_ms,q->context};
    q->state=PENDING;*out=q->ticket;rc=1;}
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}
static bool event_same(const struct vv_barge_ticket*a,const struct vv_barge_ticket*b) {
  return a->id&&a->id==b->id&&a->onset_first==b->onset_first&&a->onset_next==b->onset_next&&
    a->created_ms==b->created_ms&&same(&a->context,&b->context);
}
int vv_barge_input_tail_defer(struct vv_barge_input*q,const struct vv_barge_ticket*e) {
  if(!q||!e)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;
  if(!rc&&(!event_same(e,&q->ticket)||(q->state!=PENDING&&q->state!=CANCELLED)))rc=VV_SCOPE_STALE;
  if(!rc&&q->tail_deferred)rc=-EALREADY;
  if(!rc)q->tail_deferred=true;
  pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_tail_resume(struct vv_barge_input*q,const struct vv_barge_ticket*e,uint64_t next) {
  if(!q||!e)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;
  if(!rc&&(!event_same(e,&q->ticket)||q->state!=CANCELLED||!q->tail_deferred))rc=VV_SCOPE_STALE;
  if(!rc&&(next!=q->next_sample||next<=e->onset_next))rc=-ENODATA;
  if(!rc)q->tail_deferred=false;
  pthread_mutex_unlock(&q->lock);return rc;
}
static bool scope_same(const struct vv_reply_scope_status*s,const struct vv_confirmed_wake_context*c) {
  return s->token.incarnation==c->scope_incarnation&&s->token.serial==c->lease_serial&&
    s->token.owner_epoch==c->owner_epoch&&s->token.reply_id==c->reply_id&&
    s->token.asr_epoch==c->asr_epoch&&s->tx_generation==c->tx_generation;
}
int vv_barge_input_cancel(struct vv_barge_input*q,const struct vv_barge_ticket*e,
 struct vv_reply_scope*scope,struct vv_dialogue*d) {
  if(!q||!e||!scope||!d)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=0;struct vv_barge_observation o;
  if(q->state!=PENDING||!event_same(e,&q->ticket))goto done;
  rc=enter(q,&q->context,VV_BARGE_CHECK_CANCEL,q->material,e->onset_first,e->onset_next,&o);
  if(rc)goto done;
  struct vv_reply_scope_status ss;struct vv_dialogue_reply_status ds;
  rc=vv_reply_scope_snapshot(scope,&ss);if(!rc)rc=vv_dialogue_reply_snapshot(d,&ds);
  if(!rc&&(!scope_same(&ss,&q->context)||ss.phase!=VV_SCOPE_BOUND||
      ds.id!=e->context.reply_id||ds.phase!=VV_DIALOGUE_REPLY_SPEAKING))rc=VV_SCOPE_STALE;
  /* One-shot before CAS. Cancellation and dialogue completion remain the
   * production atomic decision; this module cannot cancel a successor ID. */
  q->state=CANCELLED;
  if(!rc)rc=vv_dialogue_cancel_reply_id(d,e->context.reply_id);
  if(rc!=1)fail(q,rc<0?rc:VV_SCOPE_STALE);
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}
static bool request_same(struct vv_barge_input*q,const struct vv_wake_request*r) {
  return q->source_live&&r->id&&r->id==q->request.id&&r->sample_first==q->request.sample_first&&
    r->keyword_end==q->request.keyword_end&&r->created_ms==q->request.created_ms&&
    same(&r->context,&q->request.context);
}
static int capture_gate(struct vv_barge_input*q) {
  struct vv_barge_observation o;
  int rc=enter(q,&q->context,VV_BARGE_CHECK_CAPTURE,q->material,
               q->request.sample_first,q->next_sample,&o);
  if(!rc)q->auth.leave(q->auth.arg);
  return rc;
}
static int health(void*arg,const struct vv_wake_request*r) {
  struct vv_barge_input*q=arg;if(!q||!r)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=request_same(q,r)?capture_gate(q):VV_SCOPE_STALE;
  pthread_mutex_unlock(&q->lock);return rc;
}
static int take(void*arg,const struct vv_wake_request*r,struct k7_voice_frame*out) {
  if(out)memset(out,0,sizeof(*out));
  struct vv_barge_input*q=arg;if(!q||!r||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=request_same(q,r)?capture_gate(q):VV_SCOPE_STALE;
  if(rc||!q->count)goto done;
  struct k7_voice_frame*f=&q->frames[q->head];
  if(f->first_sample!=q->taken_next){rc=fail(q,-EPIPE);goto done;}
  if(q->taken_next-r->sample_first>=CAPTURE_SAMPLES){rc=fail(q,-EFBIG);goto done;}
  *out=*f;q->taken_next=f->first_sample+320;q->head=(q->head+1)%VV_BARGE_STAGING_FRAMES;
  q->count--;rc=1;
done:pthread_mutex_unlock(&q->lock);return rc;
}
static int finish(void*arg,const struct vv_wake_request*r,bool cancel,uint64_t*first,uint64_t*next) {
  if(first)*first=0;
  if(next)*next=0;
  struct vv_barge_input*q=arg;if(!q||!r||!first||!next)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=0;
  if(!request_same(q,r)){rc=VV_SCOPE_STALE;goto done;}
  rc=cancel?-ECANCELED:capture_gate(q);
  if(!rc&&(q->taken_next<=r->sample_first||q->taken_next<r->keyword_end))rc=-ENODATA;
  if(!rc){*first=r->sample_first;*next=q->taken_next;}
  q->source_live=false;clear(q);
done:pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_claim(struct vv_barge_input*q,struct vv_reply_scope*scope,struct vv_pf_wake_source*out) {
  if(out)memset(out,0,sizeof(*out));
  if(!q||!scope||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;
  if(rc)goto done;
  if(q->state!=CANCELLED){rc=-EAGAIN;goto done;}
  struct vv_reply_scope_status ss;rc=vv_reply_scope_snapshot(scope,&ss);if(rc)goto done;
  if(!scope_same(&ss,&q->context)){rc=fail(q,VV_SCOPE_STALE);goto done;}
  if(ss.phase==VV_SCOPE_QUARANTINED){rc=fail(q,VV_SCOPE_OWNER_UNSAFE);goto done;}
  if(ss.phase!=VV_SCOPE_CLOSED||q->tail_deferred){
    /* Still wait, but do not let the retirement budget become unbounded. */
    struct vv_barge_observation o;
    rc=enter(q,&q->context,VV_BARGE_CHECK_FRAME,q->material,q->ticket.onset_first,q->next_sample,&o);
    if(!rc){q->auth.leave(q->auth.arg);rc=-EAGAIN;}goto done;}
  if(!ss.reply_retired||!ss.speaker_joined||!ss.tx_retired||!ss.rx_live||!ss.handoff_eligible){
    rc=fail(q,VV_SCOPE_OWNER_UNSAFE);goto done;}
  struct vv_barge_observation o;
  rc=enter(q,&q->context,VV_BARGE_CHECK_CLAIM,q->material,q->ticket.onset_first,q->next_sample,&o);
  if(rc)goto done;
  /* Re-read under the owner/mode/qualification pin. The serial caller is
   * forbidden to start another reply during this handoff; nevertheless a
   * changed scope must never reuse the earlier CLOSED snapshot. */
  rc=vv_reply_scope_snapshot(scope,&ss);
  if(!rc&&(!scope_same(&ss,&q->context)||ss.phase!=VV_SCOPE_CLOSED||
      !ss.reply_retired||!ss.speaker_joined||!ss.tx_retired||!ss.rx_live||!ss.handoff_eligible))
    rc=VV_SCOPE_OWNER_UNSAFE;
  if(rc){fail(q,rc);q->auth.leave(q->auth.arg);goto done;}
  q->request=(struct vv_wake_request){.id=q->ticket.id,.sample_first=q->frames[q->head].first_sample,
    .keyword_end=q->ticket.onset_next,.created_ms=q->ticket.created_ms,.context=q->context};
  q->taken_next=q->request.sample_first;q->source_live=true;q->state=CLAIMED;
  *out=(struct vv_pf_wake_source){q,q->request,health,take,finish};rc=1;
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}

#else
#include "vv_barge_input.h"
#include "vv_dialogue.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define CAPTURE_SAMPLES 160000u
enum state { HISTORY, PENDING, CANCELLED, CLAIMED, FAILED };
struct vv_barge_input {
  pthread_mutex_t lock;
  struct vv_barge_authority auth;
  struct k7_voice_frame frames[VV_BARGE_STAGING_FRAMES];
  struct vv_confirmed_wake_context context;
  struct vv_barge_ticket ticket;
  struct vv_wake_request request;
  enum state state;
  enum vv_barge_material material;
  unsigned head,count;
  uint64_t next_sample,taken_next,last_ms;
  bool have_frame,clock_seen,source_live,tail_deferred;
  int failure;
};
static atomic_uint_fast64_t ticket_sequence;
static bool same(const struct vv_confirmed_wake_context*a,
                 const struct vv_confirmed_wake_context*b) {
  return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&
    a->model_epoch==b->model_epoch&&a->mode_epoch==b->mode_epoch&&
    a->clean_epoch==b->clean_epoch&&a->alignment_epoch==b->alignment_epoch&&
    a->asr_epoch==b->asr_epoch&&a->scope_incarnation==b->scope_incarnation&&
    a->lease_serial==b->lease_serial&&a->reply_id==b->reply_id&&
    a->tx_generation==b->tx_generation;
}
static bool valid(const struct vv_confirmed_wake_context*c) {
  return c->owner_epoch&&c->input_epoch&&c->model_epoch&&c->mode_epoch&&
    c->clean_epoch&&c->alignment_epoch&&c->asr_epoch&&c->scope_incarnation&&
    c->lease_serial&&c->reply_id&&c->tx_generation;
}
static int fail(struct vv_barge_input*q,int rc) {
  if(!q->failure)q->failure=rc;
  q->state=FAILED;return q->failure;
}
static void clear(struct vv_barge_input*q) {
  q->head=q->count=0;q->have_frame=q->source_live=q->tail_deferred=false;
  q->state=HISTORY;q->failure=0;q->material=VV_BARGE_RAW_UNKNOWN;
  memset(&q->context,0,sizeof(q->context));
  memset(&q->ticket,0,sizeof(q->ticket));memset(&q->request,0,sizeof(q->request));
  /* Clock watermark deliberately survives reset. */
}
static int clock_check(struct vv_barge_input*q,uint64_t now) {
  if(now==UINT64_MAX||(q->clock_seen&&now<q->last_ms))return fail(q,-ETIMEDOUT);
  q->clock_seen=true;q->last_ms=now;
  unsigned limit=q->state==PENDING?VV_BARGE_TICKET_MS:
                 q->state==CANCELLED?VV_BARGE_RETIRE_MS:VV_BARGE_CAPTURE_MS;
  if(q->ticket.id&&now-q->ticket.created_ms>limit)return fail(q,-ETIMEDOUT);
  return q->failure;
}
/* Successful return always holds the independent authority pin. */
static int enter(struct vv_barge_input*q,const struct vv_confirmed_wake_context*c,
 enum vv_barge_check why,enum vv_barge_material material,uint64_t first,uint64_t next,
 struct vv_barge_observation*o) {
  if(q->failure)return q->failure;
  if(!q->auth.enter||!q->auth.leave)return fail(q,-ENOTSUP);
  if(!valid(c)||material==VV_BARGE_RAW_UNKNOWN||
     (material!=VV_BARGE_QUALIFIED_CLEAN&&material!=VV_BARGE_QUALIFIED_MUTED_CONTINUOUS))
    return fail(q,-EPROTO);
  memset(o,0,sizeof(*o));
  int rc=q->auth.enter(q->auth.arg,c,why,material,first,next,o);
  if(rc)return fail(q,rc<0?rc:-EPROTO);
  rc=clock_check(q,o->now_ms);
  if(!rc&&(!o->rx_live||o->photo_busy||o->owner_epoch!=c->owner_epoch||
     o->input_epoch!=c->input_epoch||o->mode_epoch!=c->mode_epoch||
     next>o->microphone_next))rc=fail(q,VV_SCOPE_STALE);
  if(!rc&&o->tx_active&&o->live_tx_generation!=c->tx_generation)
    rc=fail(q,VV_SCOPE_STALE);
  /* Event creation time cannot make an old, backlogged acoustic onset fresh.
   * These are physical 16 kHz MIC indices, independently observed under pin. */
  if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL)&&
     o->microphone_next-next>UINT64_C(16)*VV_BARGE_TICKET_MS)
    rc=fail(q,VV_SCOPE_STALE);
  if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL||q->state==HISTORY)&&
     (!o->tx_active||o->live_tx_generation!=c->tx_generation||
      material!=VV_BARGE_QUALIFIED_CLEAN))rc=fail(q,VV_SCOPE_STALE);
  if(!rc&&(why==VV_BARGE_CHECK_CLAIM||why==VV_BARGE_CHECK_CAPTURE)&&
     (o->tx_active||o->live_tx_generation!=c->tx_generation))rc=fail(q,VV_SCOPE_STALE);
  if(rc)q->auth.leave(q->auth.arg);
  return rc;
}
int vv_barge_input_create(const struct vv_barge_authority*a,struct vv_barge_input**out) {
  if(!out)return -EINVAL;
  *out=NULL;if(a&&((a->enter==NULL)!=(a->leave==NULL)))return -EINVAL;
  struct vv_barge_input*q=calloc(1,sizeof(*q));if(!q)return -ENOMEM;
  int rc=pthread_mutex_init(&q->lock,NULL);if(rc){free(q);return -rc;}
  if(a)q->auth=*a;
  *out=q;return 0;
}
int vv_barge_input_destroy(struct vv_barge_input*q) {
  if(!q)return -EINVAL;
  pthread_mutex_lock(&q->lock);bool busy=q->source_live;pthread_mutex_unlock(&q->lock);
  if(busy)return -EBUSY;
  int rc=pthread_mutex_destroy(&q->lock);if(rc)return -rc;
  memset(q,0,sizeof(*q));free(q);return 0;
}
void vv_barge_input_abort(struct vv_barge_input*q) {
  if(q){pthread_mutex_lock(&q->lock);fail(q,-ECANCELED);pthread_mutex_unlock(&q->lock);}
}
int vv_barge_input_reset(struct vv_barge_input*q) {
  if(!q)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->source_live?-EBUSY:0;
  if(!rc)clear(q);
  pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_frame(struct vv_barge_input*q,const struct vv_confirmed_wake_context*c,
 enum vv_barge_material material,const struct k7_voice_frame*f) {
  if(!q||!c||!f)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;struct vv_barge_observation o;
  if(rc)goto done;
  if(f->epoch!=c->input_epoch||f->samples!=320||f->flags||f->first_sample>UINT64_MAX-320){
    rc=fail(q,-EPROTO);goto done;}
  if(q->have_frame&&(!same(c,&q->context)||f->first_sample!=q->next_sample)){
    rc=fail(q,-EPIPE);goto done;}
  rc=enter(q,c,VV_BARGE_CHECK_FRAME,material,f->first_sample,f->first_sample+320,&o);
  if(rc)goto done;
  /* Explicit qualified transition only; once muted, clean old-TX input cannot
   * resurrect the retired generation. The authority proves tail continuity. */
  if(q->material==VV_BARGE_QUALIFIED_MUTED_CONTINUOUS&&material!=q->material)
    rc=fail(q,-EPIPE);
  if(!rc&&q->state==HISTORY&&q->count==VV_BARGE_HISTORY_FRAMES){
    q->head=(q->head+1)%VV_BARGE_STAGING_FRAMES;q->count--;}
  if(!rc&&q->count==VV_BARGE_STAGING_FRAMES)rc=fail(q,-ENOSPC);
  if(!rc){q->frames[(q->head+q->count)%VV_BARGE_STAGING_FRAMES]=*f;q->count++;
    q->context=*c;q->have_frame=true;q->next_sample=f->first_sample+320;q->material=material;}
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_onset(struct vv_barge_input*q,uint64_t first,uint64_t next,
 struct vv_barge_ticket*out) {
  if(out)memset(out,0,sizeof(*out));
  if(!q||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;struct vv_barge_observation o;
  if(rc)goto done;
  if(q->state!=HISTORY){rc=-EBUSY;goto done;}
  if(!q->count||first>=next||next!=q->next_sample||first<VV_BARGE_PRE_ROLL_SAMPLES||
     first%320||first-VV_BARGE_PRE_ROLL_SAMPLES<q->frames[q->head].first_sample){
    rc=-EINVAL;goto done;}
  rc=enter(q,&q->context,VV_BARGE_CHECK_ONSET,q->material,first,next,&o);if(rc)goto done;
  uint_fast64_t id=atomic_load(&ticket_sequence);
  do {if(id==UINT64_MAX){rc=fail(q,-EOVERFLOW);break;}}
  while(!atomic_compare_exchange_weak(&ticket_sequence,&id,id+1));
  if(!rc){
    uint64_t begin=first-VV_BARGE_PRE_ROLL_SAMPLES;
    while(q->count&&q->frames[q->head].first_sample<begin){q->head=(q->head+1)%VV_BARGE_STAGING_FRAMES;q->count--;}
    q->ticket=(struct vv_barge_ticket){id+1,first,next,o.now_ms,q->context};
    q->state=PENDING;*out=q->ticket;rc=1;}
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}
static bool event_same(const struct vv_barge_ticket*a,const struct vv_barge_ticket*b) {
  return a->id&&a->id==b->id&&a->onset_first==b->onset_first&&a->onset_next==b->onset_next&&
    a->created_ms==b->created_ms&&same(&a->context,&b->context);
}
int vv_barge_input_tail_defer(struct vv_barge_input*q,const struct vv_barge_ticket*e) {
  if(!q||!e)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;
  if(!rc&&(!event_same(e,&q->ticket)||(q->state!=PENDING&&q->state!=CANCELLED)))rc=VV_SCOPE_STALE;
  if(!rc&&q->tail_deferred)rc=-EALREADY;
  if(!rc)q->tail_deferred=true;
  pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_tail_resume(struct vv_barge_input*q,const struct vv_barge_ticket*e,uint64_t next) {
  if(!q||!e)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;
  if(!rc&&(!event_same(e,&q->ticket)||q->state!=CANCELLED||!q->tail_deferred))rc=VV_SCOPE_STALE;
  if(!rc&&(next!=q->next_sample||next<=e->onset_next))rc=-ENODATA;
  if(!rc)q->tail_deferred=false;
  pthread_mutex_unlock(&q->lock);return rc;
}
static bool scope_same(const struct vv_reply_scope_status*s,const struct vv_confirmed_wake_context*c) {
  return s->token.incarnation==c->scope_incarnation&&s->token.serial==c->lease_serial&&
    s->token.owner_epoch==c->owner_epoch&&s->token.reply_id==c->reply_id&&
    s->token.asr_epoch==c->asr_epoch&&s->tx_generation==c->tx_generation;
}
int vv_barge_input_cancel(struct vv_barge_input*q,const struct vv_barge_ticket*e,
 struct vv_reply_scope*scope,struct vv_dialogue*d) {
  if(!q||!e||!scope||!d)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=0;struct vv_barge_observation o;
  if(q->state!=PENDING||!event_same(e,&q->ticket))goto done;
  rc=enter(q,&q->context,VV_BARGE_CHECK_CANCEL,q->material,e->onset_first,e->onset_next,&o);
  if(rc)goto done;
  struct vv_reply_scope_status ss;struct vv_dialogue_reply_status ds;
  rc=vv_reply_scope_snapshot(scope,&ss);if(!rc)rc=vv_dialogue_reply_snapshot(d,&ds);
  if(!rc&&(!scope_same(&ss,&q->context)||ss.phase!=VV_SCOPE_BOUND||
      ds.id!=e->context.reply_id||ds.phase!=VV_DIALOGUE_REPLY_SPEAKING))rc=VV_SCOPE_STALE;
  /* One-shot before CAS. Cancellation and dialogue completion remain the
   * production atomic decision; this module cannot cancel a successor ID. */
  q->state=CANCELLED;
  if(!rc)rc=vv_dialogue_cancel_reply_id(d,e->context.reply_id);
  if(rc!=1)fail(q,rc<0?rc:VV_SCOPE_STALE);
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}
static bool request_same(struct vv_barge_input*q,const struct vv_wake_request*r) {
  return q->source_live&&r->id&&r->id==q->request.id&&r->sample_first==q->request.sample_first&&
    r->keyword_end==q->request.keyword_end&&r->created_ms==q->request.created_ms&&
    same(&r->context,&q->request.context);
}
static int capture_gate(struct vv_barge_input*q) {
  struct vv_barge_observation o;
  int rc=enter(q,&q->context,VV_BARGE_CHECK_CAPTURE,q->material,
               q->request.sample_first,q->next_sample,&o);
  if(!rc)q->auth.leave(q->auth.arg);
  return rc;
}
static int health(void*arg,const struct vv_wake_request*r) {
  struct vv_barge_input*q=arg;if(!q||!r)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=request_same(q,r)?capture_gate(q):VV_SCOPE_STALE;
  pthread_mutex_unlock(&q->lock);return rc;
}
static int take(void*arg,const struct vv_wake_request*r,struct k7_voice_frame*out) {
  if(out)memset(out,0,sizeof(*out));
  struct vv_barge_input*q=arg;if(!q||!r||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=request_same(q,r)?capture_gate(q):VV_SCOPE_STALE;
  if(rc||!q->count)goto done;
  struct k7_voice_frame*f=&q->frames[q->head];
  if(f->first_sample!=q->taken_next){rc=fail(q,-EPIPE);goto done;}
  if(q->taken_next-r->sample_first>=CAPTURE_SAMPLES){rc=fail(q,-EFBIG);goto done;}
  *out=*f;q->taken_next=f->first_sample+320;q->head=(q->head+1)%VV_BARGE_STAGING_FRAMES;
  q->count--;rc=1;
done:pthread_mutex_unlock(&q->lock);return rc;
}
static int finish(void*arg,const struct vv_wake_request*r,bool cancel,uint64_t*first,uint64_t*next) {
  if(first)*first=0;
  if(next)*next=0;
  struct vv_barge_input*q=arg;if(!q||!r||!first||!next)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=0;
  if(!request_same(q,r)){rc=VV_SCOPE_STALE;goto done;}
  rc=cancel?-ECANCELED:capture_gate(q);
  if(!rc&&(q->taken_next<=r->sample_first||q->taken_next<r->keyword_end))rc=-ENODATA;
  if(!rc){*first=r->sample_first;*next=q->taken_next;}
  q->source_live=false;clear(q);
done:pthread_mutex_unlock(&q->lock);return rc;
}
int vv_barge_input_claim(struct vv_barge_input*q,struct vv_reply_scope*scope,struct vv_pf_wake_source*out) {
  if(out)memset(out,0,sizeof(*out));
  if(!q||!scope||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=q->failure;
  if(rc)goto done;
  if(q->state!=CANCELLED){rc=-EAGAIN;goto done;}
  struct vv_reply_scope_status ss;rc=vv_reply_scope_snapshot(scope,&ss);if(rc)goto done;
  if(!scope_same(&ss,&q->context)){rc=fail(q,VV_SCOPE_STALE);goto done;}
  if(ss.phase==VV_SCOPE_QUARANTINED){rc=fail(q,VV_SCOPE_OWNER_UNSAFE);goto done;}
  if(ss.phase!=VV_SCOPE_CLOSED||q->tail_deferred){
    /* Still wait, but do not let the retirement budget become unbounded. */
    struct vv_barge_observation o;
    rc=enter(q,&q->context,VV_BARGE_CHECK_FRAME,q->material,q->ticket.onset_first,q->next_sample,&o);
    if(!rc){q->auth.leave(q->auth.arg);rc=-EAGAIN;}goto done;}
  if(!ss.reply_retired||!ss.speaker_joined||!ss.tx_retired||!ss.rx_live||!ss.handoff_eligible){
    rc=fail(q,VV_SCOPE_OWNER_UNSAFE);goto done;}
  struct vv_barge_observation o;
  rc=enter(q,&q->context,VV_BARGE_CHECK_CLAIM,q->material,q->ticket.onset_first,q->next_sample,&o);
  if(rc)goto done;
  /* Re-read under the owner/mode/qualification pin. The serial caller is
   * forbidden to start another reply during this handoff; nevertheless a
   * changed scope must never reuse the earlier CLOSED snapshot. */
  rc=vv_reply_scope_snapshot(scope,&ss);
  if(!rc&&(!scope_same(&ss,&q->context)||ss.phase!=VV_SCOPE_CLOSED||
      !ss.reply_retired||!ss.speaker_joined||!ss.tx_retired||!ss.rx_live||!ss.handoff_eligible))
    rc=VV_SCOPE_OWNER_UNSAFE;
  if(rc){fail(q,rc);q->auth.leave(q->auth.arg);goto done;}
  q->request=(struct vv_wake_request){.id=q->ticket.id,.sample_first=q->frames[q->head].first_sample,
    .keyword_end=q->ticket.onset_next,.created_ms=q->ticket.created_ms,.context=q->context};
  q->taken_next=q->request.sample_first;q->source_live=true;q->state=CLAIMED;
  *out=(struct vv_pf_wake_source){q,q->request,health,take,finish};rc=1;
  q->auth.leave(q->auth.arg);
done:pthread_mutex_unlock(&q->lock);return rc;
}

#endif
