#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "vv_barge_route.h"
#include "vv_dialogue.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct vv_barge_route {
 pthread_mutex_t lock;
 struct vv_barge_route_config c;
 struct vv_barge_input *input;
 struct vv_cancel_tail *tail;
 struct vv_reply_scope *scope;
 struct vv_dialogue *dialogue;
 struct vv_barge_ticket ticket;
 struct vv_barge_onset_support support;
 struct vv_pf_wake_source source;
 bool window,closing,pending,armed,claimed,tail_done,failed_utterance;
 int failure;
};
static int fail(struct vv_barge_route*r,int rc){
 if(r->pending||r->claimed)r->failed_utterance=true;
 if(!r->failure)r->failure=rc<0?rc:-EPROTO;
 vv_barge_input_abort(r->input);return r->failure;
}
static int observe(struct vv_barge_route*r,struct vv_barge_observation*o){
 if(r->closing)return -ECANCELED;
 if(!r->c.observe)return -ENOTSUP;
 memset(o,0,sizeof(*o));return r->c.observe(r->c.arg,o);
}
/* Independent provider holds its qualification pin. Live identity comes from
 * the actual manager/platform, never from fields fabricated by that provider. */
static int authority_enter(void*a,const struct vv_confirmed_wake_context*c,
 enum vv_barge_check why,enum vv_barge_material m,uint64_t first,uint64_t next,
 struct vv_barge_observation*out){
 struct vv_barge_route*r=a;
 if(!r->c.authority.enter||!r->c.authority.leave)return -ENOTSUP;
 struct vv_barge_observation before,ignored;int rc=observe(r,&before);
 if(rc)return rc;
 rc=r->c.authority.enter(r->c.authority.arg,c,why,m,first,next,&ignored);
 if(rc)return rc<0?rc:-EPROTO;
 rc=observe(r,out);
 if(!rc&&(before.owner_epoch!=out->owner_epoch||before.input_epoch!=out->input_epoch||
    before.mode_epoch!=out->mode_epoch||before.live_tx_generation!=out->live_tx_generation||
    !ignored.rx_live||ignored.photo_busy||ignored.owner_epoch!=out->owner_epoch||
    ignored.input_epoch!=out->input_epoch||ignored.mode_epoch!=out->mode_epoch||
    ignored.live_tx_generation!=out->live_tx_generation||ignored.tx_active!=out->tx_active))rc=VV_SCOPE_STALE;
 if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL)){
  const struct vv_barge_onset_support *v=&r->support;
  if(!r->c.onset_support)rc=-ENOTSUP;
  else if(!v->evidence_id||v->input_first>=v->input_next||v->advance_first<v->input_first||
    v->advance_first>=v->advance_next||v->advance_next!=v->input_next||
    v->completing_frame_next!=next||v->completing_frame_first>UINT64_MAX-320||
    v->completing_frame_first+320!=v->completing_frame_next||
    v->input_next<=v->completing_frame_first||v->input_next>v->completing_frame_next||
    first>v->advance_first||v->input_next>out->microphone_next||
    out->microphone_next-v->input_next>8000)rc=VV_SCOPE_STALE;
  else rc=r->c.onset_support(r->c.authority.arg,c,v);
 }
 if(rc)r->c.authority.leave(r->c.authority.arg);
 return rc;
}
static void authority_leave(void*a){struct vv_barge_route*r=a;r->c.authority.leave(r->c.authority.arg);}
int vv_barge_route_create(const struct vv_barge_route_config*c,struct vv_barge_route**out){
 if(!c||!out||!c->observe||((c->authority.enter==NULL)!=(c->authority.leave==NULL)))return -EINVAL;
 *out=NULL;struct vv_barge_route*r=calloc(1,sizeof(*r));if(!r)return -ENOMEM;
 r->c=*c;int rc=pthread_mutex_init(&r->lock,NULL);if(rc){free(r);return -rc;}
 struct vv_barge_authority a={r,authority_enter,authority_leave};
 rc=vv_barge_input_create(&a,&r->input);if(rc){pthread_mutex_destroy(&r->lock);free(r);return rc;}
 *out=r;return 0;
}
int vv_barge_route_begin(struct vv_barge_route*r,struct vv_reply_scope*s,struct vv_dialogue*d){
 if(!r||!s||!d)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failed_utterance?r->failure:r->pending||r->claimed?-EBUSY:0;
 if(!rc){rc=vv_barge_input_reset(r->input);if(!rc){r->scope=s;r->dialogue=d;r->window=true;r->failure=0;}}
 pthread_mutex_unlock(&r->lock);return rc;
}
void vv_barge_route_end(struct vv_barge_route*r){if(r){pthread_mutex_lock(&r->lock);r->window=false;pthread_mutex_unlock(&r->lock);}}
int vv_barge_route_context(struct vv_barge_route*r,struct vv_confirmed_wake_context*out){
 if(!r||!out)return -EINVAL;
 memset(out,0,sizeof(*out));pthread_mutex_lock(&r->lock);
 int rc=!r->c.authority.enter?-ENOTSUP:!r->window||r->pending?-EAGAIN:0;
 struct vv_barge_observation o;struct vv_reply_scope_status s;struct vv_dialogue_reply_status d;
 if(!rc)rc=observe(r,&o);
 if(!rc)rc=vv_reply_scope_snapshot(r->scope,&s);
 if(!rc)rc=vv_dialogue_reply_snapshot(r->dialogue,&d);
 if(!rc&&(!o.rx_live||o.photo_busy||!o.tx_active||s.phase!=VV_SCOPE_BOUND||
    d.phase!=VV_DIALOGUE_REPLY_SPEAKING||!d.id||d.id!=s.token.reply_id||
    o.owner_epoch!=s.token.owner_epoch||o.live_tx_generation!=s.tx_generation))rc=VV_SCOPE_STALE;
 if(!rc)*out=(struct vv_confirmed_wake_context){.owner_epoch=o.owner_epoch,.input_epoch=o.input_epoch,
  .mode_epoch=o.mode_epoch,.scope_incarnation=s.token.incarnation,.lease_serial=s.token.serial,
  .asr_epoch=s.token.asr_epoch,.reply_id=d.id,.tx_generation=s.tx_generation};
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_clean(struct vv_barge_route*r,const struct vv_confirmed_wake_context*c,
 enum vv_barge_material m,const struct k7_voice_frame*f){
 if(!r||!c||!f)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failure;
 if(!rc&&(!r->window&&!r->pending))rc=-EAGAIN;
 if(!rc&&r->pending&&!r->tail_done)rc=-EAGAIN;
 if(!rc)rc=vv_barge_input_frame(r->input,c,m,f);
 if(rc&&r->pending)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_onset(struct vv_barge_route*r,uint64_t first,uint64_t next,const struct vv_barge_onset_support*v){
 if(!r||!v)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:!r->window?-EAGAIN:r->pending?-EBUSY:r->failure;
 if(!rc){r->support=*v;rc=vv_barge_input_onset(r->input,first,next,&r->ticket);}
 if(rc==1){
  /* Preserve a post-cancel staging barrier BEFORE cancellation's CAS. */
  rc=vv_cancel_tail_create(r->input,&r->ticket,r->ticket.created_ms,&r->tail);
  if(!rc)rc=vv_barge_input_cancel(r->input,&r->ticket,r->scope,r->dialogue);
  if(rc==1){r->pending=true;r->tail_done=false;printf("NEAR_EXPERIMENT cancel=1 reply=%llu first=%llu next=%llu calibrated=0\n",(unsigned long long)r->ticket.context.reply_id,(unsigned long long)r->ticket.onset_first,(unsigned long long)r->ticket.onset_next);}
  else {if(r->tail){vv_cancel_tail_destroy(r->tail);r->tail=NULL;}fail(r,rc?rc:VV_SCOPE_STALE);}
 }
 pthread_mutex_unlock(&r->lock);return rc;
}
static int pump(struct vv_barge_route*r,uint64_t now){
 int rc=vv_cancel_tail_forward(r->tail,&r->c.tail,now);
 if(rc==-EAGAIN)return 0;
 if(rc<0)return fail(r,rc);
 if(rc>0)now=r->c.tail.now_ms(r->c.tail.arg);
 uint64_t first=0,next=0;rc=vv_cancel_tail_finish(r->tail,now,&first,&next);
 if(rc==-EAGAIN)return 0;
 if(rc)return fail(r,rc);
 vv_cancel_tail_destroy(r->tail);r->tail=NULL;r->tail_done=true;return 1;
}
int vv_barge_route_tail(struct vv_barge_route*r,const struct vv_confirmed_wake_context*c,
 const struct k7_voice_frame*f,uint64_t now){
 if(!r||!c||!f)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failure;
 if(!rc&&(!r->pending||!r->tail||r->tail_done))rc=-EAGAIN;
 if(!rc)rc=vv_cancel_tail_append(r->tail,c,f,now);
 if(!rc)rc=pump(r,now);
 if(rc<0&&rc!=-EAGAIN)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_tail_pump(struct vv_barge_route*r,uint64_t now){
 if(!r)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failure;
 if(!rc)rc=r->tail?pump(r,now):r->tail_done?1:-EAGAIN;
 pthread_mutex_unlock(&r->lock);return rc;
}
static bool same_request(const struct vv_wake_request*a,const struct vv_wake_request*b){
 const struct vv_confirmed_wake_context*x=&a->context,*y=&b->context;
 return a->id&&a->id==b->id&&a->sample_first==b->sample_first&&a->keyword_end==b->keyword_end&&
  a->created_ms==b->created_ms&&x->owner_epoch==y->owner_epoch&&x->input_epoch==y->input_epoch&&
  x->model_epoch==y->model_epoch&&x->mode_epoch==y->mode_epoch&&x->clean_epoch==y->clean_epoch&&
  x->alignment_epoch==y->alignment_epoch&&x->asr_epoch==y->asr_epoch&&
  x->scope_incarnation==y->scope_incarnation&&x->lease_serial==y->lease_serial&&
  x->reply_id==y->reply_id&&x->tx_generation==y->tx_generation;
}
static int source_health(void*a,const struct vv_wake_request*q){
 struct vv_barge_route*r=a;if(!q)return -EINVAL;pthread_mutex_lock(&r->lock);
 bool current=r->claimed&&same_request(q,&r->source.request);
 int rc=!current?VV_SCOPE_STALE:r->closing?-ECANCELED:r->failure;
 if(!rc)rc=r->source.health(r->source.arg,q);
 if(current&&rc)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
static int source_take(void*a,const struct vv_wake_request*q,struct k7_voice_frame*f){
 struct vv_barge_route*r=a;if(!q||!f)return -EINVAL;memset(f,0,sizeof(*f));pthread_mutex_lock(&r->lock);
 bool current=r->claimed&&same_request(q,&r->source.request);
 int rc=!current?VV_SCOPE_STALE:r->closing?-ECANCELED:r->failure;
 if(!rc)rc=r->source.take(r->source.arg,q,f);
 if(current&&rc<0)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
static int source_finish(void*a,const struct vv_wake_request*q,bool cancel,uint64_t*first,uint64_t*next){
 struct vv_barge_route*r=a;if(!q||!first||!next)return -EINVAL;
 *first=*next=0;pthread_mutex_lock(&r->lock);
 if(!r->claimed||!same_request(q,&r->source.request)){
  pthread_mutex_unlock(&r->lock);return VV_SCOPE_STALE;
 }
 int rc=r->source.finish(r->source.arg,q,cancel||r->closing||r->failure,first,next);
 /* The entry check identified this exact live request under the route lock.
  * The lower matching finish always retires it, even if its final authority
  * check returns STALE. Only the earlier request mismatch preserves the lease.
  * Keep input loss sticky so a network retry cannot switch this utterance RAW. */
 if(rc&&rc!=-ECANCELED)fail(r,rc);
 r->claimed=r->armed=r->pending=false;r->window=false;
 if(r->tail){vv_cancel_tail_destroy(r->tail);r->tail=NULL;}
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_admit(struct vv_barge_route*r){
 if(!r)return -EINVAL;
 struct vv_pf_wake_source source={0};pthread_mutex_lock(&r->lock);
 int rc=r->closing?-ECANCELED:r->failed_utterance?r->failure:r->pending?r->failure:2;
 if(!rc){
  if(r->claimed)rc=-EBUSY;
  else {rc=vv_barge_input_claim(r->input,r->scope,&r->source);
   if(rc==1){r->claimed=true;source=(struct vv_pf_wake_source){r,r->source.request,source_health,source_take,source_finish};}
   else if(rc==-EAGAIN)rc=0;
  }
 }
 pthread_mutex_unlock(&r->lock);
 if(rc!=1)return rc;
 rc=vv_paraformer_k7_wake_arm(&source);
 if(rc){uint64_t first=0,next=0;(void)source.finish(r,&source.request,true,&first,&next);return rc;}
 pthread_mutex_lock(&r->lock);r->armed=true;pthread_mutex_unlock(&r->lock);printf("NEAR_EXPERIMENT pf_handoff=1 first=%llu onset_end=%llu calibrated=0\n",(unsigned long long)source.request.sample_first,(unsigned long long)source.request.keyword_end);return 1;
}
int vv_barge_route_release(struct vv_barge_route*r){
 if(!r)return -EINVAL;
 /* Serial owner only. Disarm may call source_finish, never hold route lock. */
 pthread_mutex_lock(&r->lock);bool armed=r->armed;pthread_mutex_unlock(&r->lock);
 return armed?vv_paraformer_k7_wake_disarm():0;
}
void vv_barge_route_close(struct vv_barge_route*r){if(r){pthread_mutex_lock(&r->lock);r->closing=true;vv_barge_input_abort(r->input);pthread_mutex_unlock(&r->lock);}}
int vv_barge_route_destroy(struct vv_barge_route*r){
 if(!r)return 0;
 pthread_mutex_lock(&r->lock);bool busy=r->claimed||r->armed;pthread_mutex_unlock(&r->lock);
 if(busy)return -EBUSY;
 if(r->tail)vv_cancel_tail_destroy(r->tail);
 int rc=vv_barge_input_destroy(r->input);if(rc)return rc;
 pthread_mutex_destroy(&r->lock);memset(r,0,sizeof(*r));free(r);return 0;
}

#else
#include "vv_barge_route.h"
#include "vv_dialogue.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
struct vv_barge_route {
 pthread_mutex_t lock;
 struct vv_barge_route_config c;
 struct vv_barge_input *input;
 struct vv_cancel_tail *tail;
 struct vv_reply_scope *scope;
 struct vv_dialogue *dialogue;
 struct vv_barge_ticket ticket;
 struct vv_barge_onset_support support;
 struct vv_pf_wake_source source;
 bool window,closing,pending,armed,claimed,tail_done,failed_utterance;
 int failure;
};
static int fail(struct vv_barge_route*r,int rc){
 if(r->pending||r->claimed)r->failed_utterance=true;
 if(!r->failure)r->failure=rc<0?rc:-EPROTO;
 vv_barge_input_abort(r->input);return r->failure;
}
static int observe(struct vv_barge_route*r,struct vv_barge_observation*o){
 if(r->closing)return -ECANCELED;
 if(!r->c.observe)return -ENOTSUP;
 memset(o,0,sizeof(*o));return r->c.observe(r->c.arg,o);
}
/* Independent provider holds its qualification pin. Live identity comes from
 * the actual manager/platform, never from fields fabricated by that provider. */
static int authority_enter(void*a,const struct vv_confirmed_wake_context*c,
 enum vv_barge_check why,enum vv_barge_material m,uint64_t first,uint64_t next,
 struct vv_barge_observation*out){
 struct vv_barge_route*r=a;
 if(!r->c.authority.enter||!r->c.authority.leave)return -ENOTSUP;
 struct vv_barge_observation before,ignored;int rc=observe(r,&before);
 if(rc)return rc;
 rc=r->c.authority.enter(r->c.authority.arg,c,why,m,first,next,&ignored);
 if(rc)return rc<0?rc:-EPROTO;
 rc=observe(r,out);
 if(!rc&&(before.owner_epoch!=out->owner_epoch||before.input_epoch!=out->input_epoch||
    before.mode_epoch!=out->mode_epoch||before.live_tx_generation!=out->live_tx_generation||
    !ignored.rx_live||ignored.photo_busy||ignored.owner_epoch!=out->owner_epoch||
    ignored.input_epoch!=out->input_epoch||ignored.mode_epoch!=out->mode_epoch||
    ignored.live_tx_generation!=out->live_tx_generation||ignored.tx_active!=out->tx_active))rc=VV_SCOPE_STALE;
 if(!rc&&(why==VV_BARGE_CHECK_ONSET||why==VV_BARGE_CHECK_CANCEL)){
  const struct vv_barge_onset_support *v=&r->support;
  if(!r->c.onset_support)rc=-ENOTSUP;
  else if(!v->evidence_id||v->input_first>=v->input_next||v->advance_first<v->input_first||
    v->advance_first>=v->advance_next||v->advance_next!=v->input_next||
    v->completing_frame_next!=next||v->completing_frame_first>UINT64_MAX-320||
    v->completing_frame_first+320!=v->completing_frame_next||
    v->input_next<=v->completing_frame_first||v->input_next>v->completing_frame_next||
    first>v->advance_first||v->input_next>out->microphone_next||
    out->microphone_next-v->input_next>8000)rc=VV_SCOPE_STALE;
  else rc=r->c.onset_support(r->c.authority.arg,c,v);
 }
 if(rc)r->c.authority.leave(r->c.authority.arg);
 return rc;
}
static void authority_leave(void*a){struct vv_barge_route*r=a;r->c.authority.leave(r->c.authority.arg);}
int vv_barge_route_create(const struct vv_barge_route_config*c,struct vv_barge_route**out){
 if(!c||!out||!c->observe||((c->authority.enter==NULL)!=(c->authority.leave==NULL)))return -EINVAL;
 *out=NULL;struct vv_barge_route*r=calloc(1,sizeof(*r));if(!r)return -ENOMEM;
 r->c=*c;int rc=pthread_mutex_init(&r->lock,NULL);if(rc){free(r);return -rc;}
 struct vv_barge_authority a={r,authority_enter,authority_leave};
 rc=vv_barge_input_create(&a,&r->input);if(rc){pthread_mutex_destroy(&r->lock);free(r);return rc;}
 *out=r;return 0;
}
int vv_barge_route_begin(struct vv_barge_route*r,struct vv_reply_scope*s,struct vv_dialogue*d){
 if(!r||!s||!d)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failed_utterance?r->failure:r->pending||r->claimed?-EBUSY:0;
 if(!rc){rc=vv_barge_input_reset(r->input);if(!rc){r->scope=s;r->dialogue=d;r->window=true;r->failure=0;}}
 pthread_mutex_unlock(&r->lock);return rc;
}
void vv_barge_route_end(struct vv_barge_route*r){if(r){pthread_mutex_lock(&r->lock);r->window=false;pthread_mutex_unlock(&r->lock);}}
int vv_barge_route_context(struct vv_barge_route*r,struct vv_confirmed_wake_context*out){
 if(!r||!out)return -EINVAL;
 memset(out,0,sizeof(*out));pthread_mutex_lock(&r->lock);
 int rc=!r->c.authority.enter?-ENOTSUP:!r->window||r->pending?-EAGAIN:0;
 struct vv_barge_observation o;struct vv_reply_scope_status s;struct vv_dialogue_reply_status d;
 if(!rc)rc=observe(r,&o);
 if(!rc)rc=vv_reply_scope_snapshot(r->scope,&s);
 if(!rc)rc=vv_dialogue_reply_snapshot(r->dialogue,&d);
 if(!rc&&(!o.rx_live||o.photo_busy||!o.tx_active||s.phase!=VV_SCOPE_BOUND||
    d.phase!=VV_DIALOGUE_REPLY_SPEAKING||!d.id||d.id!=s.token.reply_id||
    o.owner_epoch!=s.token.owner_epoch||o.live_tx_generation!=s.tx_generation))rc=VV_SCOPE_STALE;
 if(!rc)*out=(struct vv_confirmed_wake_context){.owner_epoch=o.owner_epoch,.input_epoch=o.input_epoch,
  .mode_epoch=o.mode_epoch,.scope_incarnation=s.token.incarnation,.lease_serial=s.token.serial,
  .asr_epoch=s.token.asr_epoch,.reply_id=d.id,.tx_generation=s.tx_generation};
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_clean(struct vv_barge_route*r,const struct vv_confirmed_wake_context*c,
 enum vv_barge_material m,const struct k7_voice_frame*f){
 if(!r||!c||!f)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failure;
 if(!rc&&(!r->window&&!r->pending))rc=-EAGAIN;
 if(!rc&&r->pending&&!r->tail_done)rc=-EAGAIN;
 if(!rc)rc=vv_barge_input_frame(r->input,c,m,f);
 if(rc&&r->pending)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_onset(struct vv_barge_route*r,uint64_t first,uint64_t next,const struct vv_barge_onset_support*v){
 if(!r||!v)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:!r->window?-EAGAIN:r->pending?-EBUSY:r->failure;
 if(!rc){r->support=*v;rc=vv_barge_input_onset(r->input,first,next,&r->ticket);}
 if(rc==1){
  /* Preserve a post-cancel staging barrier BEFORE cancellation's CAS. */
  rc=vv_cancel_tail_create(r->input,&r->ticket,r->ticket.created_ms,&r->tail);
  if(!rc)rc=vv_barge_input_cancel(r->input,&r->ticket,r->scope,r->dialogue);
  if(rc==1){r->pending=true;r->tail_done=false;}
  else {if(r->tail){vv_cancel_tail_destroy(r->tail);r->tail=NULL;}fail(r,rc?rc:VV_SCOPE_STALE);}
 }
 pthread_mutex_unlock(&r->lock);return rc;
}
static int pump(struct vv_barge_route*r,uint64_t now){
 int rc=vv_cancel_tail_forward(r->tail,&r->c.tail,now);
 if(rc==-EAGAIN)return 0;
 if(rc<0)return fail(r,rc);
 uint64_t first=0,next=0;rc=vv_cancel_tail_finish(r->tail,now,&first,&next);
 if(rc==-EAGAIN)return 0;
 if(rc)return fail(r,rc);
 vv_cancel_tail_destroy(r->tail);r->tail=NULL;r->tail_done=true;return 1;
}
int vv_barge_route_tail(struct vv_barge_route*r,const struct vv_confirmed_wake_context*c,
 const struct k7_voice_frame*f,uint64_t now){
 if(!r||!c||!f)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failure;
 if(!rc&&(!r->pending||!r->tail||r->tail_done))rc=-EAGAIN;
 if(!rc)rc=vv_cancel_tail_append(r->tail,c,f,now);
 if(!rc)rc=pump(r,now);
 if(rc<0&&rc!=-EAGAIN)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_tail_pump(struct vv_barge_route*r,uint64_t now){
 if(!r)return -EINVAL;
 pthread_mutex_lock(&r->lock);int rc=r->closing?-ECANCELED:r->failure;
 if(!rc)rc=r->tail?pump(r,now):r->tail_done?1:-EAGAIN;
 pthread_mutex_unlock(&r->lock);return rc;
}
static bool same_request(const struct vv_wake_request*a,const struct vv_wake_request*b){
 const struct vv_confirmed_wake_context*x=&a->context,*y=&b->context;
 return a->id&&a->id==b->id&&a->sample_first==b->sample_first&&a->keyword_end==b->keyword_end&&
  a->created_ms==b->created_ms&&x->owner_epoch==y->owner_epoch&&x->input_epoch==y->input_epoch&&
  x->model_epoch==y->model_epoch&&x->mode_epoch==y->mode_epoch&&x->clean_epoch==y->clean_epoch&&
  x->alignment_epoch==y->alignment_epoch&&x->asr_epoch==y->asr_epoch&&
  x->scope_incarnation==y->scope_incarnation&&x->lease_serial==y->lease_serial&&
  x->reply_id==y->reply_id&&x->tx_generation==y->tx_generation;
}
static int source_health(void*a,const struct vv_wake_request*q){
 struct vv_barge_route*r=a;if(!q)return -EINVAL;pthread_mutex_lock(&r->lock);
 bool current=r->claimed&&same_request(q,&r->source.request);
 int rc=!current?VV_SCOPE_STALE:r->closing?-ECANCELED:r->failure;
 if(!rc)rc=r->source.health(r->source.arg,q);
 if(current&&rc)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
static int source_take(void*a,const struct vv_wake_request*q,struct k7_voice_frame*f){
 struct vv_barge_route*r=a;if(!q||!f)return -EINVAL;memset(f,0,sizeof(*f));pthread_mutex_lock(&r->lock);
 bool current=r->claimed&&same_request(q,&r->source.request);
 int rc=!current?VV_SCOPE_STALE:r->closing?-ECANCELED:r->failure;
 if(!rc)rc=r->source.take(r->source.arg,q,f);
 if(current&&rc<0)fail(r,rc);
 pthread_mutex_unlock(&r->lock);return rc;
}
static int source_finish(void*a,const struct vv_wake_request*q,bool cancel,uint64_t*first,uint64_t*next){
 struct vv_barge_route*r=a;if(!q||!first||!next)return -EINVAL;
 *first=*next=0;pthread_mutex_lock(&r->lock);
 if(!r->claimed||!same_request(q,&r->source.request)){
  pthread_mutex_unlock(&r->lock);return VV_SCOPE_STALE;
 }
 int rc=r->source.finish(r->source.arg,q,cancel||r->closing||r->failure,first,next);
 /* The entry check identified this exact live request under the route lock.
  * The lower matching finish always retires it, even if its final authority
  * check returns STALE. Only the earlier request mismatch preserves the lease.
  * Keep input loss sticky so a network retry cannot switch this utterance RAW. */
 if(rc&&rc!=-ECANCELED)fail(r,rc);
 r->claimed=r->armed=r->pending=false;r->window=false;
 if(r->tail){vv_cancel_tail_destroy(r->tail);r->tail=NULL;}
 pthread_mutex_unlock(&r->lock);return rc;
}
int vv_barge_route_admit(struct vv_barge_route*r){
 if(!r)return -EINVAL;
 struct vv_pf_wake_source source={0};pthread_mutex_lock(&r->lock);
 int rc=r->closing?-ECANCELED:r->failed_utterance?r->failure:r->pending?r->failure:2;
 if(!rc){
  if(r->claimed)rc=-EBUSY;
  else {rc=vv_barge_input_claim(r->input,r->scope,&r->source);
   if(rc==1){r->claimed=true;source=(struct vv_pf_wake_source){r,r->source.request,source_health,source_take,source_finish};}
   else if(rc==-EAGAIN)rc=0;
  }
 }
 pthread_mutex_unlock(&r->lock);
 if(rc!=1)return rc;
 rc=vv_paraformer_k7_wake_arm(&source);
 if(rc){uint64_t first=0,next=0;(void)source.finish(r,&source.request,true,&first,&next);return rc;}
 pthread_mutex_lock(&r->lock);r->armed=true;pthread_mutex_unlock(&r->lock);return 1;
}
int vv_barge_route_release(struct vv_barge_route*r){
 if(!r)return -EINVAL;
 /* Serial owner only. Disarm may call source_finish, never hold route lock. */
 pthread_mutex_lock(&r->lock);bool armed=r->armed;pthread_mutex_unlock(&r->lock);
 return armed?vv_paraformer_k7_wake_disarm():0;
}
void vv_barge_route_close(struct vv_barge_route*r){if(r){pthread_mutex_lock(&r->lock);r->closing=true;vv_barge_input_abort(r->input);pthread_mutex_unlock(&r->lock);}}
int vv_barge_route_destroy(struct vv_barge_route*r){
 if(!r)return 0;
 pthread_mutex_lock(&r->lock);bool busy=r->claimed||r->armed;pthread_mutex_unlock(&r->lock);
 if(busy)return -EBUSY;
 if(r->tail)vv_cancel_tail_destroy(r->tail);
 int rc=vv_barge_input_destroy(r->input);if(rc)return rc;
 pthread_mutex_destroy(&r->lock);memset(r,0,sizeof(*r));free(r);return 0;
}

#endif

#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
int vv_barge_route_rearm_ready(struct vv_barge_route*r){
 if(!r)return -EINVAL;
 pthread_mutex_lock(&r->lock);struct vv_barge_observation o;
 int rc=r->closing?-ECANCELED:r->window||r->pending||r->claimed||r->armed?-EBUSY:r->failed_utterance?r->failure:observe(r,&o);
 if(!rc&&(o.tx_active||!o.rx_live))rc=-EBUSY;
 pthread_mutex_unlock(&r->lock);return rc;
}
#endif
