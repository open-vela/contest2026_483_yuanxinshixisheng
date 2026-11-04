#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "vv_cancel_tail.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct vv_cancel_tail {
  struct vv_barge_input *queue; /* caller-pinned until destroy */
  struct vv_barge_ticket ticket;
  struct k7_voice_frame raw[VV_CANCEL_TAIL_FRAMES];
  uint64_t started,last_ms,next,committed;
  unsigned head,count,forwarded;
  int failure;
  bool finished;
};
size_t vv_cancel_tail_storage_bytes(void) {return sizeof(struct vv_cancel_tail);}
static bool same(const struct vv_confirmed_wake_context*a,const struct vv_confirmed_wake_context*b) {
  return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&a->model_epoch==b->model_epoch&&
    a->mode_epoch==b->mode_epoch&&a->clean_epoch==b->clean_epoch&&a->alignment_epoch==b->alignment_epoch&&
    a->scope_incarnation==b->scope_incarnation&&a->lease_serial==b->lease_serial&&a->reply_id==b->reply_id&&
    a->asr_epoch==b->asr_epoch&&a->tx_generation==b->tx_generation;
}
static int fail(struct vv_cancel_tail*b,int rc) {
  if(!b->failure)b->failure=rc;
  vv_barge_input_abort(b->queue);return b->failure;
}
static int check(struct vv_cancel_tail*b,uint64_t now) {
  if(!b)return -EINVAL;
  if(b->failure)return b->failure;
  if(b->finished)return -EALREADY;
  if(now==UINT64_MAX||now<b->last_ms||now-b->started>VV_CANCEL_TAIL_WAIT_MS)
    return fail(b,-ETIMEDOUT);
  b->last_ms=now;return 0;
}
int vv_cancel_tail_create(struct vv_barge_input*q,const struct vv_barge_ticket*t,
 uint64_t now,struct vv_cancel_tail**out) {
  if(out)*out=NULL;
  if(!q||!t||!out||!t->id||now==UINT64_MAX||now<t->created_ms||now-t->created_ms>VV_BARGE_TICKET_MS)
    return -EINVAL;
  struct vv_cancel_tail*b=calloc(1,sizeof(*b));if(!b)return -ENOMEM;
  int rc=vv_barge_input_tail_defer(q,t);if(rc){free(b);return rc;}
  b->queue=q;b->ticket=*t;b->started=b->last_ms=now;b->next=b->committed=t->onset_next;
  *out=b;return 0;
}
int vv_cancel_tail_append(struct vv_cancel_tail*b,const struct vv_confirmed_wake_context*c,
 const struct k7_voice_frame*f,uint64_t now) {
  int rc=check(b,now);if(rc)return rc;
  if(!c||!f||!same(c,&b->ticket.context)||f->epoch!=c->input_epoch||f->samples!=320||f->flags||
     f->first_sample!=b->next||b->next>UINT64_MAX-320)return fail(b,-EPIPE);
  if(b->count==VV_CANCEL_TAIL_FRAMES)return fail(b,-ENOSPC);
  b->raw[(b->head+b->count)%VV_CANCEL_TAIL_FRAMES]=*f;
  b->count++;b->next+=320;return 0;
}
int vv_cancel_tail_forward(struct vv_cancel_tail*b,const struct vv_cancel_tail_transform*t,uint64_t now) {
  int rc=check(b,now);if(rc)return rc;
  if(!b->count)return 0;
  if(!t||!t->frame)return -EAGAIN;
  if(!t->now_ms)return fail(b,-ENOTSUP);
  rc=check(b,t->now_ms(t->arg));if(rc)return rc;
  const struct k7_voice_frame*raw=&b->raw[b->head];
  struct k7_voice_frame clean={0};enum vv_barge_material material=VV_BARGE_RAW_UNKNOWN;
  rc=t->frame(t->arg,&b->ticket,raw,&clean,&material);
  int timed=check(b,t->now_ms(t->arg));if(timed)return timed;
  if(rc==-EAGAIN)return rc;
  if(rc)return fail(b,rc<0?rc:-EPROTO);
  if(clean.first_sample!=raw->first_sample||clean.epoch!=raw->epoch||clean.samples!=320||clean.flags||
     (!vv_barge_material_clean(material)&&!vv_barge_material_tail(material)))
    return fail(b,-EPROTO);
  rc=vv_barge_input_frame(b->queue,&b->ticket.context,material,&clean);
  if(rc)return fail(b,rc);
  b->committed=clean.first_sample+320;b->head=(b->head+1)%VV_CANCEL_TAIL_FRAMES;
  b->count--;b->forwarded++;return 1;
}
int vv_cancel_tail_finish(struct vv_cancel_tail*b,uint64_t now,uint64_t*first,uint64_t*next) {
  if(first)*first=0;
  if(next)*next=0;
  if(!first||!next)return -EINVAL;
  int rc=check(b,now);if(rc)return rc;
  if(b->count||!b->forwarded||b->committed!=b->next)return -EAGAIN;
  rc=vv_barge_input_tail_resume(b->queue,&b->ticket,b->next);if(rc)return fail(b,rc);
  *first=b->ticket.onset_next;*next=b->next;b->finished=true;return 0;
}
void vv_cancel_tail_destroy(struct vv_cancel_tail*b) {
  if(!b)return;
  if(!b->finished)vv_barge_input_abort(b->queue);
  memset(b,0,sizeof(*b));free(b);
}

#else
#include "vv_cancel_tail.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct vv_cancel_tail {
  struct vv_barge_input *queue; /* caller-pinned until destroy */
  struct vv_barge_ticket ticket;
  struct k7_voice_frame raw[VV_CANCEL_TAIL_FRAMES];
  uint64_t started,last_ms,next,committed;
  unsigned head,count,forwarded;
  int failure;
  bool finished;
};
size_t vv_cancel_tail_storage_bytes(void) {return sizeof(struct vv_cancel_tail);}
static bool same(const struct vv_confirmed_wake_context*a,const struct vv_confirmed_wake_context*b) {
  return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&a->model_epoch==b->model_epoch&&
    a->mode_epoch==b->mode_epoch&&a->clean_epoch==b->clean_epoch&&a->alignment_epoch==b->alignment_epoch&&
    a->scope_incarnation==b->scope_incarnation&&a->lease_serial==b->lease_serial&&a->reply_id==b->reply_id&&
    a->asr_epoch==b->asr_epoch&&a->tx_generation==b->tx_generation;
}
static int fail(struct vv_cancel_tail*b,int rc) {
  if(!b->failure)b->failure=rc;
  vv_barge_input_abort(b->queue);return b->failure;
}
static int check(struct vv_cancel_tail*b,uint64_t now) {
  if(!b)return -EINVAL;
  if(b->failure)return b->failure;
  if(b->finished)return -EALREADY;
  if(now==UINT64_MAX||now<b->last_ms||now-b->started>VV_CANCEL_TAIL_WAIT_MS)
    return fail(b,-ETIMEDOUT);
  b->last_ms=now;return 0;
}
int vv_cancel_tail_create(struct vv_barge_input*q,const struct vv_barge_ticket*t,
 uint64_t now,struct vv_cancel_tail**out) {
  if(out)*out=NULL;
  if(!q||!t||!out||!t->id||now==UINT64_MAX||now<t->created_ms||now-t->created_ms>VV_BARGE_TICKET_MS)
    return -EINVAL;
  struct vv_cancel_tail*b=calloc(1,sizeof(*b));if(!b)return -ENOMEM;
  int rc=vv_barge_input_tail_defer(q,t);if(rc){free(b);return rc;}
  b->queue=q;b->ticket=*t;b->started=b->last_ms=now;b->next=b->committed=t->onset_next;
  *out=b;return 0;
}
int vv_cancel_tail_append(struct vv_cancel_tail*b,const struct vv_confirmed_wake_context*c,
 const struct k7_voice_frame*f,uint64_t now) {
  int rc=check(b,now);if(rc)return rc;
  if(!c||!f||!same(c,&b->ticket.context)||f->epoch!=c->input_epoch||f->samples!=320||f->flags||
     f->first_sample!=b->next||b->next>UINT64_MAX-320)return fail(b,-EPIPE);
  if(b->count==VV_CANCEL_TAIL_FRAMES)return fail(b,-ENOSPC);
  b->raw[(b->head+b->count)%VV_CANCEL_TAIL_FRAMES]=*f;
  b->count++;b->next+=320;return 0;
}
int vv_cancel_tail_forward(struct vv_cancel_tail*b,const struct vv_cancel_tail_transform*t,uint64_t now) {
  int rc=check(b,now);if(rc)return rc;
  if(!b->count)return 0;
  if(!t||!t->frame)return -EAGAIN;
  if(!t->now_ms)return fail(b,-ENOTSUP);
  rc=check(b,t->now_ms(t->arg));if(rc)return rc;
  const struct k7_voice_frame*raw=&b->raw[b->head];
  struct k7_voice_frame clean={0};enum vv_barge_material material=VV_BARGE_RAW_UNKNOWN;
  rc=t->frame(t->arg,&b->ticket,raw,&clean,&material);
  int timed=check(b,t->now_ms(t->arg));if(timed)return timed;
  if(rc==-EAGAIN)return rc;
  if(rc)return fail(b,rc<0?rc:-EPROTO);
  if(clean.first_sample!=raw->first_sample||clean.epoch!=raw->epoch||clean.samples!=320||clean.flags||
     (material!=VV_BARGE_QUALIFIED_CLEAN&&material!=VV_BARGE_QUALIFIED_MUTED_CONTINUOUS))
    return fail(b,-EPROTO);
  rc=vv_barge_input_frame(b->queue,&b->ticket.context,material,&clean);
  if(rc)return fail(b,rc);
  b->committed=clean.first_sample+320;b->head=(b->head+1)%VV_CANCEL_TAIL_FRAMES;
  b->count--;b->forwarded++;return 1;
}
int vv_cancel_tail_finish(struct vv_cancel_tail*b,uint64_t now,uint64_t*first,uint64_t*next) {
  if(first)*first=0;
  if(next)*next=0;
  if(!first||!next)return -EINVAL;
  int rc=check(b,now);if(rc)return rc;
  if(b->count||!b->forwarded||b->committed!=b->next)return -EAGAIN;
  rc=vv_barge_input_tail_resume(b->queue,&b->ticket,b->next);if(rc)return fail(b,rc);
  *first=b->ticket.onset_next;*next=b->next;b->finished=true;return 0;
}
void vv_cancel_tail_destroy(struct vv_cancel_tail*b) {
  if(!b)return;
  if(!b->finished)vv_barge_input_abort(b->queue);
  memset(b,0,sizeof(*b));free(b);
}

#endif
