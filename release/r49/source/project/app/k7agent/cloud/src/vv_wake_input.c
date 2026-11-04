#include "vv_wake_input.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define CANDIDATE_MS 2000u
#define CAPTURE_MS 12000u
#define CAPTURE_SAMPLES 160000u
struct vv_wake_input {
  pthread_mutex_t lock;
  struct k7_voice_frame frames[VV_WAKE_STAGING_FRAMES];
  struct vv_confirmed_wake_context context;
  struct vv_wake_request request;
  unsigned head,count;
  uint64_t next_sample,last_ms,taken_next;
  bool have_frame,clock_seen,pending,active;
  int failure;
};
static atomic_uint_fast64_t request_sequence=ATOMIC_VAR_INIT(0);
static bool same(const struct vv_confirmed_wake_context *a,
                 const struct vv_confirmed_wake_context *b) {
  return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&
    a->model_epoch==b->model_epoch&&a->mode_epoch==b->mode_epoch&&
    a->clean_epoch==b->clean_epoch&&a->alignment_epoch==b->alignment_epoch&&
    a->asr_epoch==b->asr_epoch&&a->scope_incarnation==b->scope_incarnation&&
    a->lease_serial==b->lease_serial&&a->reply_id==b->reply_id&&
    a->tx_generation==b->tx_generation;
}
static void clear(struct vv_wake_input *q) {
  q->head=q->count=0;q->have_frame=q->pending=q->active=false;
  q->failure=0;memset(&q->request,0,sizeof(q->request));
}
static int fail(struct vv_wake_input *q,int rc) {
  if(q->active){if(!q->failure)q->failure=rc;}
  else clear(q);
  return rc;
}
static int clock_check(struct vv_wake_input *q,uint64_t now) {
  if(now==UINT64_MAX||(q->clock_seen&&now<q->last_ms))return fail(q,-ETIMEDOUT);
  q->last_ms=now;q->clock_seen=true;
  if((q->pending&&now-q->request.created_ms>CANDIDATE_MS)||
     (q->active&&now-q->request.created_ms>CAPTURE_MS))return fail(q,-ETIMEDOUT);
  return q->failure;
}
int vv_wake_input_create(struct vv_wake_input **out) {
  if(!out)return -EINVAL;
  *out=calloc(1,sizeof(**out));if(!*out)return -ENOMEM;
  int rc=pthread_mutex_init(&(*out)->lock,NULL);
  if(rc){free(*out);*out=NULL;return -rc;}return 0;
}
void vv_wake_input_destroy(struct vv_wake_input *q) {
  if(q){pthread_mutex_destroy(&q->lock);memset(q,0,sizeof(*q));free(q);}
}
void vv_wake_input_reset(struct vv_wake_input *q) {
  if(q){pthread_mutex_lock(&q->lock);
    if(q->active)fail(q,-ECANCELED);else clear(q);
    pthread_mutex_unlock(&q->lock);}
}
int vv_wake_input_frame(struct vv_wake_input *q,
 const struct vv_confirmed_wake_context *c,enum vv_wake_activity activity,
 const struct k7_voice_frame *f,uint64_t now) {
  if(!q||!c||!f)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=clock_check(q,now);if(rc)goto done;
  if(activity!=VV_WAKE_IDLE||c->reply_id||c->tx_generation){rc=fail(q,-EBUSY);goto done;}
  if(!c->owner_epoch||!c->input_epoch||!c->model_epoch||!c->mode_epoch||
     f->epoch!=c->input_epoch||f->samples!=320||f->flags||
     f->first_sample>UINT64_MAX-320){rc=fail(q,-EPROTO);goto done;}
  if(q->have_frame&&(!same(c,&q->context)||f->first_sample!=q->next_sample)) {
    if(q->active||q->pending){rc=fail(q,-EPIPE);goto done;}
    clear(q); /* A new idle history, never stitch the preceding segment. */
  }
  q->context=*c;q->have_frame=true;q->next_sample=f->first_sample+320;
  if(!q->pending&&!q->active&&q->count==VV_WAKE_HISTORY_FRAMES){
    q->head=(q->head+1)%VV_WAKE_STAGING_FRAMES;q->count--;
  }
  if(q->count==VV_WAKE_STAGING_FRAMES){rc=fail(q,-ENOSPC);goto done;}
  q->frames[(q->head+q->count)%VV_WAKE_STAGING_FRAMES]=*f;q->count++;rc=0;
done:pthread_mutex_unlock(&q->lock);return rc;
}
int vv_wake_input_candidate(struct vv_wake_input *q,
 const struct vv_confirmed_wake_context *c,uint64_t end,uint64_t now) {
  if(!q||!c)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=clock_check(q,now);if(rc)goto done;
  if(q->pending||q->active){rc=0;goto done;}
  if(!q->have_frame||!q->count||!same(c,&q->context)||!end||end!=q->next_sample||
     end<=q->frames[q->head].first_sample){rc=0;goto done;}
  uint_fast64_t id=atomic_load(&request_sequence);
  do {if(id==UINT64_MAX){rc=-EOVERFLOW;goto done;}}
  while(!atomic_compare_exchange_weak(&request_sequence,&id,id+1));
  q->request=(struct vv_wake_request){.id=id+1,
    .sample_first=q->frames[q->head].first_sample,.keyword_end=end,
    .created_ms=now,.context=q->context};
  q->pending=true;rc=1;
done:pthread_mutex_unlock(&q->lock);return rc;
}
int vv_wake_input_claim(struct vv_wake_input *q,
 const struct vv_confirmed_wake_context *c,enum vv_wake_activity activity,
 uint64_t now,struct vv_wake_request *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!q||!c||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=clock_check(q,now);if(rc)goto done;
  if(!q->pending){rc=q->active?-EBUSY:0;goto done;}
  if(activity!=VV_WAKE_IDLE||!same(c,&q->context)){rc=fail(q,VV_WAKE_STALE);goto done;}
  q->pending=false;q->active=true;q->taken_next=q->request.sample_first;
  *out=q->request;rc=1;
done:pthread_mutex_unlock(&q->lock);return rc;
}
static bool ticket(struct vv_wake_input *q,const struct vv_wake_request *r) {
  return q->active&&r->id&&r->id==q->request.id&&
    r->sample_first==q->request.sample_first&&r->keyword_end==q->request.keyword_end&&
    r->created_ms==q->request.created_ms&&same(&r->context,&q->request.context);
}
int vv_wake_input_health(struct vv_wake_input *q,const struct vv_wake_request *r,
 uint64_t now) {
  if(!q||!r)return -EINVAL;
  pthread_mutex_lock(&q->lock);
  int rc=ticket(q,r)?clock_check(q,now):VV_WAKE_STALE;
  pthread_mutex_unlock(&q->lock);return rc;
}
int vv_wake_input_take(struct vv_wake_input *q,const struct vv_wake_request *r,
 uint64_t now,struct k7_voice_frame *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!q||!r||!out)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=0;
  if(!ticket(q,r)){rc=VV_WAKE_STALE;goto done;}
  rc=clock_check(q,now);if(rc)goto done;
  if(!q->count)goto done;
  struct k7_voice_frame *f=&q->frames[q->head];
  if(f->first_sample!=q->taken_next){rc=fail(q,-EPIPE);goto done;}
  if(q->taken_next-r->sample_first>=CAPTURE_SAMPLES){rc=fail(q,-EFBIG);goto done;}
  *out=*f;q->head=(q->head+1)%VV_WAKE_STAGING_FRAMES;q->count--;
  q->taken_next=f->first_sample+320;rc=1;
done:pthread_mutex_unlock(&q->lock);return rc;
}
int vv_wake_input_finish(struct vv_wake_input *q,const struct vv_wake_request *r,
 uint64_t now,uint64_t *first,uint64_t *next) {
  if(first)*first=0;
  if(next)*next=0;
  if(!q||!r||!first||!next)return -EINVAL;
  pthread_mutex_lock(&q->lock);int rc=0;
  if(!ticket(q,r)){rc=VV_WAKE_STALE;goto done;}
  rc=clock_check(q,now);
  if(!rc&&(q->taken_next<=r->sample_first||q->taken_next<r->keyword_end))rc=-ENODATA;
  if(!rc){*first=r->sample_first;*next=q->taken_next;}
  clear(q);
done:pthread_mutex_unlock(&q->lock);return rc;
}
