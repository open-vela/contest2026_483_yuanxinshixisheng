#include "vv_reply_scope.h"
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include "k7_echo_reply_identity.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
struct vv_reply_scope {
  pthread_mutex_t lock;
  uint64_t incarnation,serial,owner;
  uint32_t last_asr;
  bool active;
  struct vv_reply_scope_status status;
};
static atomic_uint_fast64_t incarnations;
static int enter(struct vv_reply_scope *s){return s?-pthread_mutex_lock(&s->lock):-EINVAL;}
static int leave(struct vv_reply_scope*s,int rc){int e=pthread_mutex_unlock(&s->lock);return rc?rc:-e;}
static bool matches(struct vv_reply_scope *s,const struct vv_reply_scope_token*t){
  return s&&t&&t->scope==s&&t->incarnation==s->incarnation&&t->owner_epoch==s->owner&&
    t->serial==s->status.token.serial&&t->reply_id==s->status.token.reply_id&&
    t->asr_epoch==s->status.token.asr_epoch&&t->serial&&t->reply_id&&t->asr_epoch;
}
int vv_reply_scope_create(uint64_t owner,struct vv_reply_scope **out){
  if(out)*out=NULL;
  if(!owner||!out)return -EINVAL;
  struct vv_reply_scope*s=calloc(1,sizeof(*s));if(!s)return -ENOMEM;
  int e=pthread_mutex_init(&s->lock,NULL);if(e){free(s);return -e;}
  uint_fast64_t n=atomic_load(&incarnations);
  do{if(n==UINT64_MAX){pthread_mutex_destroy(&s->lock);free(s);return -EOVERFLOW;}}
  while(!atomic_compare_exchange_weak(&incarnations,&n,n+1));
  s->incarnation=n+1;s->owner=owner;*out=s;return 0;
}
int vv_reply_scope_destroy(struct vv_reply_scope*s){
  int rc=enter(s);if(rc)return rc;if(s->active)return leave(s,-EBUSY);
  #if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
(void)k7_echo_reply_retire(s->incarnation,s->status.token.serial);
#endif
  leave(s,0);int e=pthread_mutex_destroy(&s->lock);if(e)return -e;free(s);return 0;
}
int vv_reply_scope_set_owner(struct vv_reply_scope*s,uint64_t owner){
  int rc=enter(s);if(rc)return rc;
  if(s->active)return leave(s,-EBUSY);
  if(owner<=s->owner)return leave(s,VV_SCOPE_STALE);
  #if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
(void)k7_echo_reply_retire(s->incarnation,s->status.token.serial);
#endif
  s->owner=owner;s->last_asr=0;memset(&s->status,0,sizeof(s->status));return leave(s,0);
}
int vv_reply_scope_begin(struct vv_reply_scope*s,uint64_t owner,uint32_t asr,
                         uint64_t reply,struct vv_reply_scope_token*out){
  if(out)memset(out,0,sizeof(*out));
  if(!out||!asr||!reply)return -EINVAL;
  int rc=enter(s);if(rc)return rc;
  if(owner!=s->owner||asr<=s->last_asr)return leave(s,VV_SCOPE_STALE);
  if(s->active)return leave(s,-EBUSY);
  if(s->serial==UINT64_MAX)return leave(s,-EOVERFLOW);
  struct vv_reply_scope_token t={s,s->incarnation,++s->serial,owner,reply,asr};
  s->status=(struct vv_reply_scope_status){.token=t,.phase=VV_SCOPE_RESERVED};
  s->last_asr=asr;s->active=true;*out=t;return leave(s,0);
}
int vv_reply_scope_snapshot(struct vv_reply_scope*s,struct vv_reply_scope_status*out){
  if(!out)return -EINVAL;
  int rc=enter(s);if(rc)return rc;*out=s->status;return leave(s,0);
}
int vv_reply_scope_speaker_begin(const struct vv_reply_scope_token*t){
  struct vv_reply_scope*s=t?t->scope:NULL;int rc=enter(s);if(rc)return rc;
  if(!matches(s,t)||!s->active)return leave(s,VV_SCOPE_STALE);
  if(s->status.phase!=VV_SCOPE_RESERVED)return leave(s,-EALREADY);
  s->status.phase=VV_SCOPE_SPEAKER;return leave(s,0);
}
int vv_reply_scope_bind(const struct vv_reply_scope_token*t,uint64_t owner,uint32_t tx){
  struct vv_reply_scope*s=t?t->scope:NULL;int rc=enter(s);if(rc)return rc;
  if(!matches(s,t)||!s->active||owner!=s->owner)return leave(s,VV_SCOPE_STALE);
  if(!tx)return leave(s,-EINVAL);
  if(s->status.phase!=VV_SCOPE_SPEAKER)return leave(s,-EALREADY);
  s->status.tx_generation=tx;s->status.phase=VV_SCOPE_BOUND;return leave(s,0);
}
int vv_reply_scope_speaker_end(const struct vv_reply_scope_token*t,uint32_t tx,
                              bool retired,bool joined,bool rx){
  struct vv_reply_scope*s=t?t->scope:NULL;int rc=enter(s);if(rc)return rc;
  if(!matches(s,t)||!s->active)return leave(s,VV_SCOPE_STALE);
  if(s->status.phase!=VV_SCOPE_SPEAKER&&s->status.phase!=VV_SCOPE_BOUND)
    return leave(s,-EALREADY);
  #if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
(void)k7_echo_reply_retire(t->incarnation,t->serial);
#endif
  if(tx!=s->status.tx_generation||!retired||!joined){
    s->status.phase=VV_SCOPE_QUARANTINED;return leave(s,-EIO);
  }
  s->status.tx_retired=retired;s->status.speaker_joined=joined;s->status.rx_live=rx;
  s->status.phase=VV_SCOPE_TX_RETIRED;return leave(s,0);
}
int vv_reply_scope_speaker_safe(const struct vv_reply_scope_token*t){
  struct vv_reply_scope*s=t?t->scope:NULL;int rc=enter(s);if(rc)return rc;
  if(!matches(s,t)||!s->active)return leave(s,VV_SCOPE_STALE);
  return leave(s,s->status.phase==VV_SCOPE_TX_RETIRED&&s->status.tx_generation?0:-EIO);
}
int vv_reply_scope_reply_end(const struct vv_reply_scope_token*t){
  struct vv_reply_scope*s=t?t->scope:NULL;int rc=enter(s);if(rc)return rc;
  if(!matches(s,t)||!s->active)return leave(s,VV_SCOPE_STALE);
  #if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
(void)k7_echo_reply_retire(t->incarnation,t->serial);
#endif
  s->status.reply_retired=true;
  if(s->status.phase==VV_SCOPE_RESERVED){s->active=false;s->status.phase=VV_SCOPE_CLOSED;return leave(s,0);}
  if(s->status.phase!=VV_SCOPE_TX_RETIRED){s->status.phase=VV_SCOPE_QUARANTINED;return leave(s,-EIO);}
  s->status.handoff_eligible=s->status.tx_retired&&s->status.speaker_joined&&s->status.rx_live;
  s->status.phase=VV_SCOPE_CLOSED;s->active=false;return leave(s,0);
}
