#include "vv_trng_development_policy.h"
#include <errno.h>
#include <string.h>
#define VV_TD_MAX_LIFETIME_MS 10800000u
static void wipe(uint8_t *p,size_t n){if(p){volatile uint8_t*q=p;while(n--)*q++=0;}}
static void fail(struct vv_trng_development_policy*p){
 int state=atomic_load(&p->state);
 while(state!=VV_TD_REVOKED&&state!=VV_TD_FAILED&&
       !atomic_compare_exchange_weak(&p->state,&state,VV_TD_FAILED)){}
}
static bool admitted(void *arg){
 struct vv_trng_development_policy*p=arg;
 int state=atomic_load(&p->state);
 if(state!=VV_TD_PROBING&&state!=VV_TD_APPROVED)return false;
 uint64_t now=p->ops.now_ms(p->ops.arg);
 if(now==UINT64_MAX||now<p->started_ms||now>=atomic_load(&p->expires_ms)||
    !p->ops.context_valid(p->ops.arg,p->generation)){fail(p);return false;}
 return atomic_load(&p->state)==state;
}
bool vv_trng_development_approved(void *arg){
 struct vv_trng_development_policy*p=arg;
 return p&&atomic_load(&p->state)==VV_TD_APPROVED&&admitted(p);
}
void vv_trng_development_revoke(struct vv_trng_development_policy*p){
 if(p)atomic_store(&p->state,VV_TD_REVOKED);
}
int vv_trng_development_probe(struct vv_trng_development_policy*p,
 const struct vv_trng_development_ops*ops,uint64_t generation,uint64_t deadline){
 if(!p||!ops||!ops->context_valid||!ops->now_ms||!ops->begin||!ops->read_exact||!generation)return -EINVAL;
 bool idle=false;
 if(!atomic_compare_exchange_strong(&p->busy,&idle,true))return -EBUSY;
 int rc=-EALREADY;uint8_t discard[64]={0};
 if(atomic_load(&p->state)!=VV_TD_UNTRIED)goto done;
 p->ops=*ops;p->generation=generation;
 uint64_t now=ops->now_ms(ops->arg);
 if(now==UINT64_MAX||now>UINT64_MAX-VV_TD_MAX_LIFETIME_MS||deadline<=now||deadline-now>100){rc=-EINVAL;fail(p);goto done;}
 p->started_ms=now;atomic_store(&p->expires_ms,now+VV_TD_MAX_LIFETIME_MS);
 atomic_store(&p->last_validation_ms,now);
 int expected=VV_TD_UNTRIED;
 if(!atomic_compare_exchange_strong(&p->state,&expected,VV_TD_PROBING)){rc=-ECANCELED;goto done;}
 if(!admitted(p)){rc=-EACCES;goto done;}
 rc=ops->begin(ops->arg,admitted,p);if(rc){if(rc>0)rc=-EIO;fail(p);goto done;}
 ssize_t n=ops->read_exact(ops->arg,discard,sizeof(discard),deadline);
 now=ops->now_ms(ops->arg);
 if(n!=(ssize_t)sizeof(discard)||now>=deadline||!admitted(p)||
    !memcmp(discard,discard+32,32)){rc=n<0?(int)n:-EIO;fail(p);goto done;}
 expected=VV_TD_PROBING;
 rc=atomic_compare_exchange_strong(&p->state,&expected,VV_TD_APPROVED)?0:-ECANCELED;
done:wipe(discard,sizeof(discard));atomic_store(&p->busy,false);return rc;
}
ssize_t vv_trng_development_read(void*arg,uint8_t*out,size_t n,uint64_t deadline){
 struct vv_trng_development_policy*p=arg;
 if(!p||!out||!n){wipe(out,n);return -EINVAL;}
 bool idle=false;
 if(!atomic_compare_exchange_strong(&p->busy,&idle,true)){wipe(out,n);return -EBUSY;}
 ssize_t rc=-EACCES;
 if(!vv_trng_development_approved(p))goto done;
 uint64_t now=p->ops.now_ms(p->ops.arg);
 if(deadline<=now||deadline-now>100){rc=-EINVAL;fail(p);goto done;}
 rc=p->ops.read_exact(p->ops.arg,out,n,deadline);
 if(rc!=(ssize_t)n||p->ops.now_ms(p->ops.arg)>=deadline||!vv_trng_development_approved(p)){
  if(rc>=0)rc=-EIO;
  fail(p);
 }
done:if(rc<0)wipe(out,n);atomic_store(&p->busy,false);return rc;
}

/* Not a TLS retry: this is an opt-in maintenance operation under the original
 * boot reservation. Raw state and its previous full block are never reset. */
int vv_trng_development_renew(struct vv_trng_development_policy*p,
 uint64_t generation,uint64_t deadline){
 if(!p||!generation)return -EINVAL;
 bool idle=false;
 if(!atomic_compare_exchange_strong(&p->busy,&idle,true))return -EBUSY;
 int rc=-EACCES;uint8_t discard[64]={0};
 if(atomic_load(&p->state)!=VV_TD_APPROVED)goto done;
 if(generation!=p->generation){rc=-ESTALE;goto done;}
 uint64_t expires=atomic_load(&p->expires_ms);
 uint64_t first=p->ops.now_ms(p->ops.arg);
 if(first==UINT64_MAX||first<p->started_ms||first<atomic_load(&p->last_validation_ms)||
    first>=expires||!p->ops.context_valid(p->ops.arg,p->generation)){fail(p);goto done;}
 if(deadline<=first||deadline-first>100||deadline>expires){rc=-EINVAL;fail(p);goto done;}
 ssize_t n=p->ops.read_exact(p->ops.arg,discard,sizeof(discard),deadline);
 uint64_t final=p->ops.now_ms(p->ops.arg);
 if(final==UINT64_MAX||final<first||final>=deadline||final>=expires||
    final>UINT64_MAX-VV_TD_MAX_LIFETIME_MS||
    !p->ops.context_valid(p->ops.arg,p->generation)){rc=-EACCES;fail(p);goto done;}
 if(atomic_load(&p->state)!=VV_TD_APPROVED){rc=-ECANCELED;goto done;}
 /* A callback -EBUSY may originate inside a raw core that has disabled
  * itself. Only the outer policy trygate proves side-effect-free contention. */
 if(n!=(ssize_t)sizeof(discard)||!memcmp(discard,discard+32,32)){
  rc=n<0?(int)n:-EIO;fail(p);goto done;
 }
 int expected=VV_TD_APPROVED;
 if(!atomic_compare_exchange_strong(&p->state,&expected,VV_TD_RENEWING)){
  rc=-ECANCELED;goto done;
 }
 /* This short publication state admits no TLS/raw users. Recheck callbacks
  * after taking it; concurrent revoke is never overwritten by the final CAS. */
 uint64_t commit=p->ops.now_ms(p->ops.arg);
 if(commit==UINT64_MAX||commit<final||commit>=deadline||commit>=expires||
    commit>UINT64_MAX-VV_TD_MAX_LIFETIME_MS||
    !p->ops.context_valid(p->ops.arg,p->generation)){rc=-EACCES;fail(p);goto done;}
 if(atomic_load(&p->state)!=VV_TD_RENEWING){rc=-ECANCELED;goto done;}
 atomic_store(&p->expires_ms,commit+VV_TD_MAX_LIFETIME_MS);
 atomic_store(&p->last_validation_ms,commit);
 expected=VV_TD_RENEWING;
 rc=atomic_compare_exchange_strong(&p->state,&expected,VV_TD_APPROVED)?0:-ECANCELED;
done:wipe(discard,sizeof(discard));atomic_store(&p->busy,false);return rc;
}

int vv_trng_development_snapshot(struct vv_trng_development_policy*p,
 struct vv_trng_development_lease*out){
 if(!p||!out)return -EINVAL;
 bool idle=false;
 if(!atomic_compare_exchange_strong(&p->busy,&idle,true))return -EBUSY;
 struct vv_trng_development_lease v={.state=atomic_load(&p->state),
  .generation=p->generation,.started_ms=p->started_ms,
  .expires_ms=atomic_load(&p->expires_ms),
  .last_validation_ms=atomic_load(&p->last_validation_ms),.now_ms=UINT64_MAX};
 if(v.generation&&p->ops.now_ms){
  v.now_ms=p->ops.now_ms(p->ops.arg);
  v.clock_valid=v.now_ms!=UINT64_MAX&&v.now_ms>=v.started_ms&&v.now_ms>=v.last_validation_ms;
  v.expired=v.clock_valid&&v.now_ms>=v.expires_ms;
  v.remaining_ms=v.clock_valid&&v.now_ms<v.expires_ms?v.expires_ms-v.now_ms:0;
 }
 v.state=atomic_load(&p->state);
 *out=v;atomic_store(&p->busy,false);return 0;
}
