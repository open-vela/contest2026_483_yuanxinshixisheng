#include "vv_confirmed_wake.h"
#include "device_voice_intent.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define EVENT_MAX_AGE_MS 800u
struct vv_confirmed_wake_gate {
  pthread_mutex_t lock;
  struct vv_confirmed_wake_context context;
  struct vv_confirmed_wake_event pending;
  enum vv_wake_activity activity;
  uint64_t last_ms;
  bool bound, clean, clock_seen;
};
static atomic_uint_fast64_t sequence=ATOMIC_VAR_INIT(0);
static bool same(const struct vv_confirmed_wake_context *a,
                 const struct vv_confirmed_wake_context *b) {
  return a->owner_epoch==b->owner_epoch&&a->model_epoch==b->model_epoch&&
    a->clean_epoch==b->clean_epoch&&a->alignment_epoch==b->alignment_epoch&&
    a->scope_incarnation==b->scope_incarnation&&a->lease_serial==b->lease_serial&&
    a->reply_id==b->reply_id&&a->input_epoch==b->input_epoch&&
    a->mode_epoch==b->mode_epoch&&a->asr_epoch==b->asr_epoch&&
    a->tx_generation==b->tx_generation;
}
static bool valid(const struct vv_confirmed_wake_context *c) {
  return c&&c->owner_epoch&&c->input_epoch&&c->model_epoch&&c->mode_epoch;
}
static void clear(struct vv_confirmed_wake_gate *g) {
  memset(&g->pending,0,sizeof(g->pending));
}
int vv_confirmed_wake_create(struct vv_confirmed_wake_gate **out) {
  if(!out)return -EINVAL;
  *out=calloc(1,sizeof(**out));if(!*out)return -ENOMEM;
  int rc=pthread_mutex_init(&(*out)->lock,NULL);
  if(rc){free(*out);*out=NULL;return -rc;}return 0;
}
void vv_confirmed_wake_destroy(struct vv_confirmed_wake_gate *g) {
  /* Owner has unregistered and joined every observer before destroy. */
  if(g){pthread_mutex_destroy(&g->lock);memset(g,0,sizeof(*g));free(g);}
}
int vv_confirmed_wake_bind(struct vv_confirmed_wake_gate *g,
  const struct vv_confirmed_wake_context *c,enum vv_wake_activity a,bool clean) {
  if(!g)return -EINVAL;
  pthread_mutex_lock(&g->lock);
  if(!valid(c)||a<VV_WAKE_IDLE||a>VV_WAKE_REPLY||(clean&&(!c->clean_epoch||!c->alignment_epoch))) {
    clear(g);g->bound=false;pthread_mutex_unlock(&g->lock);return -EINVAL;
  }
  if(!g->bound||!same(c,&g->context)||g->activity!=a||g->clean!=clean)clear(g);
  g->context=*c;g->activity=a;g->clean=clean;g->bound=true;
  pthread_mutex_unlock(&g->lock);return 0;
}
void vv_confirmed_wake_invalidate(struct vv_confirmed_wake_gate *g) {
  if(g){pthread_mutex_lock(&g->lock);clear(g);g->bound=false;pthread_mutex_unlock(&g->lock);}
}
static bool clock_ok(struct vv_confirmed_wake_gate *g,uint64_t now) {
  if(now==UINT64_MAX||(g->clock_seen&&now<g->last_ms)){clear(g);g->bound=false;return false;}
  g->clock_seen=true;g->last_ms=now;return true;
}
static bool whole(const char *s,size_t n) {
  char v[64];size_t used=0;
  for(size_t i=0;i<n;) {
    unsigned char c=(unsigned char)s[i];
    if(c==' '||c=='\t'||c=='\r'||c=='\n'||c==','||c=='.'||c=='!'||c=='?'){i++;continue;}
    const char *marks[]={"，","。","！","？","　"};bool skipped=false;
    for(unsigned k=0;k<sizeof(marks)/sizeof(marks[0]);k++) {
      size_t m=strlen(marks[k]);
      if(n-i>=m&&!memcmp(s+i,marks[k],m)){i+=m;skipped=true;break;}
    }
    if(skipped)continue;
    if(used+1>=sizeof(v))return false;
    v[used++]=(c>='A'&&c<='Z')?(char)(c-'A'+'a'):(char)c;i++;
  }
  v[used]=0;return !strcmp(v,"你好openvela");
}
int vv_confirmed_wake_final(struct vv_confirmed_wake_gate *g,
 const struct vv_confirmed_wake_context *c,enum vv_confirmed_wake_source source,
 uint64_t first,uint64_t next,uint64_t now,const char *text,size_t bytes,
 bool verified,bool complete,struct vv_confirmed_wake_event *out) {
  if(out)memset(out,0,sizeof(*out));
  if(!g||!c||!out)return -EINVAL;
  /* Existing parser keeps stop and negation ahead of fuzzy greeting. */
  enum k7_voice_intent intent=k7_voice_completed_text(text,bytes,verified,complete);
  pthread_mutex_lock(&g->lock);int rc=0;
  if(!clock_ok(g,now)){rc=-ETIMEDOUT;goto done;}
  if(!g->bound||!same(c,&g->context)||g->activity==VV_WAKE_PHOTO||
     first>=next||next-first>160000)goto done;
  if(complete&&intent!=K7_VOICE_TRACK){clear(g);goto done;}
  if(intent!=K7_VOICE_TRACK)goto done;
  if(source==VV_WAKE_IDLE_ASR_FINAL) {
    if(g->activity!=VV_WAKE_IDLE||c->reply_id||c->tx_generation)goto done;
  }else if(source==VV_WAKE_CLEAN_FULL_KWS) {
    if(g->activity!=VV_WAKE_REPLY||!g->clean||!c->clean_epoch||!c->alignment_epoch||
       !c->scope_incarnation||!c->lease_serial||!c->asr_epoch||!c->reply_id||
       !c->tx_generation||!whole(text,bytes))goto done;
  }else goto done;
  if(g->pending.event_id) {
    if(now-g->pending.confirmed_ms<=EVENT_MAX_AGE_MS)goto done;
    clear(g);
  }
  uint_fast64_t id=atomic_load(&sequence);
  do {if(id==UINT64_MAX){rc=-EOVERFLOW;goto done;}}
  while(!atomic_compare_exchange_weak(&sequence,&id,id+1));
  g->pending=(struct vv_confirmed_wake_event){.event_id=id+1,.sample_first=first,
    .sample_next=next,.confirmed_ms=now,.context=*c,.source=source,.full_wake=true};
  *out=g->pending;rc=1;
done:pthread_mutex_unlock(&g->lock);return rc;
}
int vv_confirmed_wake_consume(void *arg,const struct vv_confirmed_wake_event *e,
 const struct vv_confirmed_wake_context *current,uint64_t now) {
  struct vv_confirmed_wake_gate *g=arg;
  if(!g||!e||!current)return -EINVAL;
  pthread_mutex_lock(&g->lock);int rc=0;
  if(!clock_ok(g,now))goto done;
  struct vv_confirmed_wake_event *p=&g->pending;
  if(p->event_id&&now-p->confirmed_ms>EVENT_MAX_AGE_MS)clear(g);
  if(!g->bound||g->activity==VV_WAKE_PHOTO||!p->event_id||
     !same(&g->context,current)||!same(&p->context,&e->context)||
     p->event_id!=e->event_id||p->source!=e->source||
     p->sample_first!=e->sample_first||p->sample_next!=e->sample_next||
     p->confirmed_ms!=e->confirmed_ms||p->full_wake!=e->full_wake||
     p->negated!=e->negated||!e->full_wake||e->negated)goto done;
  clear(g);rc=1;
done:pthread_mutex_unlock(&g->lock);return rc;
}
