#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "vv_k7_platform_factory.h"
#include "vv_cloud_entropy.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
#include "vv_k7_trng_development.h"
#endif
#include "vv_cloud_trust.h"
#include "vv_cloud_tcp.h"
#include "vv_dns_nuttx.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
int vv_k7_platform_factory_login(struct vv_native_platform*p,vv_k7_secure_load_fn load,
 void*arg,const struct vv_https_cancel*cancel){
 if(!p||!load)return -EINVAL;
 struct vv_k7_secure_login secrets={{0},{0},{0}};
 int rc=load(arg,&secrets);
 if(!rc){
  if(!memchr(secrets.credential,0,sizeof(secrets.credential))||
     !memchr(secrets.version,0,sizeof(secrets.version))||
     !memchr(secrets.proof,0,sizeof(secrets.proof))||!secrets.credential[0])rc=-EINVAL;
  else rc=vv_native_platform_login(p,secrets.credential,secrets.version,secrets.proof,cancel);
 }
 vv_https_secure_zero(&secrets,sizeof(secrets));return rc;
}
#if defined(__NuttX__) && defined(CONFIG_VELAVISION_K7_PLATFORM_FACTORY)
#include "k7_radio_service.h"
#include <pthread.h>
#include <time.h>
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
#include "vv_roughtime_k7.h"
#include "vv_rtc_provision.h"
#endif
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
 bool attempted,prepared;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 bool entropy_fatal,reject_recorded;
 int time_source_observed; /* diagnostic only; -1 when predicate not queried */
 uint64_t entropy_generation,entropy_last_mono;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 bool renewal_fatal,lease_expired,refresh_busy;
 uint64_t lease_mono,lease_utc,last_mono,last_utc,next_refresh;
 unsigned refresh_failures;
#endif
 struct vv_k7_platform_boot_config c;
 struct vv_platform_diag diag;
 struct vv_cloud_trust trust;
 struct vv_cloud_entropy entropy;
 struct vv_cloud_tcp tcp;
 ssize_t (*raw)(void*,uint8_t*,size_t,uint64_t);
} f;
static uint64_t mono(void*a){struct timespec t;(void)a;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||(uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static bool utc(void*a,uint64_t*out){struct timespec t;(void)a;if(!out)return false;*out=0;
 if(clock_gettime(CLOCK_REALTIME,&t)||t.tv_sec<=0||t.tv_nsec<0||t.tv_nsec>=1000000000)return false;
 *out=(uint64_t)t.tv_sec;return true;
}
static bool healthy(void*a){(void)a;return f.c.authority.trng_source_approved&&f.c.authority.trng_source_approved(f.c.authority.trng_source_arg);}
static ssize_t raw(void*a,uint8_t*b,size_t n,uint64_t d){(void)a;return f.raw?f.raw(0,b,n,d):-EACCES;}
/* Included in the reviewed factory TU after its raw adapter, not a new
 * runtime module. This file is spliced into the overlay for review/build. */
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
#define ENTROPY_RENEW_WINDOW_MS UINT64_C(600000)
#define ENTROPY_RENEW_BUDGET_MS UINT64_C(100)
enum reject_reason {
 REJ_UNPREPARED=1u,REJ_ENTROPY_OFF=2u,REJ_ENTROPY_FAILED=4u,
 REJ_SOURCE=8u,REJ_CA=16u,REJ_MONO_INVALID=32u,REJ_MONO_ROLLBACK=64u,
 REJ_UTC_INVALID=128u,REJ_UTC_ROLLBACK=256u,REJ_DRIFT=512u,
 REJ_TIME_EXPIRED=1024u,REJ_TRUST_UNARMED=2048u,
 REJ_SOURCE_LEASE=4096u,REJ_RENEW_ERROR=8192u
};
/* Factory lock held. Called only on the first rejected/fatal observation.
 * No decision is made from this diagnostic snapshot. No random bytes/logs. */
static void first_reject_values(unsigned reason,uint64_t m,uint64_t u,int source,const struct vv_cloud_trust*t){
 if(f.reject_recorded)return;
 f.reject_recorded=true;
 struct vv_trng_development_lease l={0};int q=-ENOTSUP;
 if(f.c.authority.trng_development_ns_policy&&
    f.c.authority.trng_source_approved==vv_k7_trng_development_approved&&
    !f.c.authority.trng_source_arg)q=vv_k7_trng_development_snapshot(&l);
 uint64_t am=t->anchor_mono_ms,lm=t->last_mono_ms,au=t->anchor_utc_s,lu=t->last_utc_s,age=t->max_age_ms;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(f.c.renew_trusted_time){am=f.lease_mono;lm=f.last_mono;au=f.lease_utc;lu=f.last_utc;age=86400000;}
#endif
 printf("SECURITY_REJECT reason=%u prepared=%u entropy_enabled=%u entropy_failed=%u source=%d ca=%u mono=%llu utc=%llu trust_armed=%u time_anchor_mono=%llu time_last_mono=%llu time_anchor_utc=%llu time_last_utc=%llu time_max_age=%llu lease_query_rc=%d lease_state=%u lease_remaining=%llu lease_now=%llu lease_expires=%llu\n",
 reason,(unsigned)f.prepared,(unsigned)f.entropy.enabled,(unsigned)f.entropy.failed,
 source,(unsigned)f.c.ca_approved,(unsigned long long)m,(unsigned long long)u,
 (unsigned)t->armed,(unsigned long long)am,(unsigned long long)lm,
 (unsigned long long)au,(unsigned long long)lu,(unsigned long long)age,
 q,q?0u:(unsigned)l.state,q?0:(unsigned long long)l.remaining_ms,
 q?0:(unsigned long long)l.now_ms,q?0:(unsigned long long)l.expires_ms);
}
static void first_reject(unsigned reason,uint64_t m,uint64_t u,int source){first_reject_values(reason,m,u,source,&f.trust);}
static unsigned trust_reasons(uint64_t m,uint64_t u,bool source){
 unsigned b=0;
 if(!f.prepared)b|=REJ_UNPREPARED;
 if(!f.entropy.enabled)b|=REJ_ENTROPY_OFF;
 if(f.entropy.failed)b|=REJ_ENTROPY_FAILED;
 if(!source)b|=REJ_SOURCE;
 if(!f.c.ca_approved)b|=REJ_CA;
 if(m==UINT64_MAX)b|=REJ_MONO_INVALID;
 else if(m<f.trust.last_mono_ms||m<f.trust.anchor_mono_ms)b|=REJ_MONO_ROLLBACK;
 if(!u)b|=REJ_UTC_INVALID;
 else if(u<f.trust.last_utc_s)b|=REJ_UTC_ROLLBACK;
 if(!f.trust.armed)b|=REJ_TRUST_UNARMED;
 if(f.trust.armed&&m!=UINT64_MAX&&m>=f.trust.anchor_mono_ms){
  uint64_t d=(m-f.trust.anchor_mono_ms)/1000;
  if(m-f.trust.anchor_mono_ms>=f.trust.max_age_ms)b|=REJ_TIME_EXPIRED;
  if(f.trust.anchor_utc_s>UINT64_MAX-d)b|=REJ_DRIFT;
  else {uint64_t expected=f.trust.anchor_utc_s+d;
   if((u>expected?u-expected:expected-u)>f.trust.max_drift_s)b|=REJ_DRIFT;}
 }
 return b;
}
static int entropy_fatal(int rc,unsigned why,const struct vv_trng_development_lease*l){
 /* No fabricated healthy() observation on early snapshot/renew failure. */
 first_reject(why,l?l->now_ms:mono(0),0,-1);
 f.entropy_fatal=true;vv_cloud_entropy_disable(&f.entropy);
 vv_cloud_trust_revoke(&f.trust);return rc<0?rc:-EACCES;
}
/* Status is a read-only policy observation, never a source probe. Factory
 * remembers its own last observed monotonic value to reject rollback. */
static int entropy_state_locked(struct vv_trng_development_lease*l){
 if(!f.prepared||!f.c.renew_entropy_lease||f.entropy_fatal)return -EACCES;
 int rc=vv_k7_trng_development_snapshot(l);
 if(rc==-EBUSY)return rc; /* Only this outer readonly entry is proven pure. */
 if(rc)return entropy_fatal(rc,REJ_SOURCE_LEASE,NULL);
 if(l->state!=VV_TD_APPROVED||l->generation!=f.entropy_generation||
    !l->clock_valid||l->expired||!l->remaining_ms||l->now_ms<f.entropy_last_mono)
  return entropy_fatal(-EACCES,REJ_SOURCE_LEASE,l);
 if(!f.entropy.enabled||f.entropy.failed||!healthy(NULL))
  return entropy_fatal(-EACCES,REJ_SOURCE,l);
 /* Source renewal never repairs clock integrity. Observe existing independent
  * time anchors without arming/revoking/extending them or fetching new time. */
 uint64_t u=0,m=mono(NULL),am=f.trust.anchor_mono_ms,au=f.trust.anchor_utc_s;
 uint64_t lm=f.trust.last_mono_ms,lu=f.trust.last_utc_s;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(f.c.renew_trusted_time){am=f.lease_mono;au=f.lease_utc;lm=f.last_mono;lu=f.last_utc;}
#endif
 unsigned clock_reason=0;
 if(m==UINT64_MAX)clock_reason|=REJ_MONO_INVALID;
 else if(m<am||m<lm)clock_reason|=REJ_MONO_ROLLBACK;
 if(!utc(NULL,&u)||!u)clock_reason|=REJ_UTC_INVALID;
 else if(u<lu)clock_reason|=REJ_UTC_ROLLBACK;
 if(m!=UINT64_MAX&&m>=am){
  uint64_t d=(m-am)/1000;
  if(au>UINT64_MAX-d)clock_reason|=REJ_DRIFT;
  else{uint64_t expected=au+d;if((u>expected?u-expected:expected-u)>2)clock_reason|=REJ_DRIFT;}
 }
 if(clock_reason){first_reject(clock_reason,m,u,true);return entropy_fatal(-EACCES,clock_reason,l);}
 f.entropy_last_mono=l->now_ms;
 return l->remaining_ms<=ENTROPY_RENEW_WINDOW_MS?1:0;
}
static int entropy_status(void *arg){
 (void)arg;struct vv_trng_development_lease l={0};pthread_mutex_lock(&lock);
 int rc=entropy_state_locked(&l);pthread_mutex_unlock(&lock);return rc;
}
static bool entropy_cancelled(const struct vv_https_cancel*c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static int entropy_refresh(void *arg,const struct vv_https_cancel*cancel){
 (void)arg;
 /* Never call this token while holding factory lock: a caller's safe-point
  * health callback may consult security/status. Raw only sees policy/context. */
 if(entropy_cancelled(cancel))return -ECANCELED;
 struct vv_trng_development_lease before={0},after={0};pthread_mutex_lock(&lock);
 int rc=entropy_state_locked(&before);
 if(rc==-EBUSY)rc=1;
 else if(rc==1){
  uint64_t deadline=before.now_ms>UINT64_MAX-ENTROPY_RENEW_BUDGET_MS?UINT64_MAX:before.now_ms+ENTROPY_RENEW_BUDGET_MS;
  if(deadline>before.expires_ms)deadline=before.expires_ms;
  rc=vv_k7_trng_development_renew(f.entropy_generation,deadline);
  int q=vv_k7_trng_development_snapshot(&after);
  if(rc==-EBUSY&&!q&&after.state==VV_TD_APPROVED&&
     after.generation==before.generation&&after.expires_ms==before.expires_ms&&
     after.last_validation_ms==before.last_validation_ms)rc=1;
  else if(rc||q||after.state!=VV_TD_APPROVED||after.generation!=f.entropy_generation||
     !after.clock_valid||after.expired||!after.remaining_ms||!healthy(NULL))
   rc=entropy_fatal(rc<0?rc:-EACCES,REJ_RENEW_ERROR,q?NULL:&after);
  else f.entropy_last_mono=after.now_ms;
 }
 pthread_mutex_unlock(&lock);
 if(entropy_cancelled(cancel)){
  pthread_mutex_lock(&lock);f.entropy_fatal=true;vv_cloud_entropy_disable(&f.entropy);
  vv_cloud_trust_revoke(&f.trust);vv_k7_trng_development_revoke();pthread_mutex_unlock(&lock);
  return -ECANCELED;
 }
 return rc;
}
#endif

#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
/* lock held. Expiry is the only recoverable trust loss. Keep an independent
 * anchor so clearing the expired trust cannot erase rollback/error evidence. */
static int time_gate(uint64_t m,uint64_t u){
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 f.time_source_observed=-1;
#endif
 if(!f.prepared||f.renewal_fatal||!f.c.renew_trusted_time)return -EACCES;
 bool source=f.entropy.enabled&&!f.entropy.failed&&healthy(0);
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 f.time_source_observed=source;
#endif
 if(!source||!f.c.ca_approved||m==UINT64_MAX||m<f.last_mono||u<f.last_utc||
    m<f.lease_mono||!u)goto fatal;
 uint64_t elapsed=(m-f.lease_mono)/1000;
 if(f.lease_utc>UINT64_MAX-elapsed)goto fatal;
 uint64_t expected=f.lease_utc+elapsed,diff=u>expected?u-expected:expected-u;
 if(diff>2||(!f.trust.armed&&!f.lease_expired))goto fatal;
 f.last_mono=m;f.last_utc=u;
 if(m-f.lease_mono>=UINT64_C(86400000)){
  f.lease_expired=true;vv_cloud_trust_revoke(&f.trust);return 2;
 }
 if(!vv_cloud_trust_check(&f.trust,m,u,source,f.c.ca_approved))goto fatal;
 return m-f.lease_mono>=UINT64_C(82800000)?1:0;
fatal:
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 first_reject(trust_reasons(m,u,source),m,u,source);
#endif
 f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);return -EACCES;
}
static int time_status(void *arg){
 (void)arg;uint64_t u=0;pthread_mutex_lock(&lock);
 int rc=utc(0,&u)?time_gate(mono(0),u):-EACCES;
 if(rc<0){
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
  first_reject(u?REJ_TRUST_UNARMED:REJ_UTC_INVALID,mono(NULL),u,-1);
#endif
  f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);
 }
 pthread_mutex_unlock(&lock);return rc;
}
static bool time_cancelled(const struct vv_https_cancel *c){
 return c&&c->is_cancelled&&c->is_cancelled(c->arg);
}
static int time_refresh(void *arg,const struct vv_https_cancel *cancel){
 (void)arg;
 if(time_cancelled(cancel))return -ECANCELED;
 pthread_mutex_lock(&lock);uint64_t u=0,m=mono(0);
 int state=utc(0,&u)?time_gate(m,u):-EACCES;
 if(state<0){
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
  first_reject(u?REJ_TRUST_UNARMED:REJ_UTC_INVALID,m,u,-1);
#endif
  f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);pthread_mutex_unlock(&lock);return state;
 }
 if(!state||m<f.next_refresh){pthread_mutex_unlock(&lock);return state==2?1:0;}
 if(f.refresh_busy){pthread_mutex_unlock(&lock);return -EBUSY;}
 f.refresh_busy=true;pthread_mutex_unlock(&lock);
 /* No factory lock is held over network/entropy callbacks. No ordinary TLS
  * is used to authenticate time and no expired trust is provisionally armed. */
 struct vv_trusted_time_sample sample={0};
 int rc=vv_roughtime_k7_fetch_runtime(&f.c.authority,&f.tcp,cancel,&sample);
 bool cancelled=time_cancelled(cancel);
 pthread_mutex_lock(&lock);m=mono(0);u=0;
 state=utc(0,&u)?time_gate(m,u):-EACCES;
 if(state<0){f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);rc=state;}
 else if(cancelled)rc=-ECANCELED;
 else if(!rc){
  /* Authenticated new authorization, not a lease extension based on old RTC.
   * We intentionally do not step either system clock or RTC at runtime. */
  if(!sample.present||!sample.authorized||sample.received_mono_ms>m)rc=-EACCES;
  else if(m-sample.received_mono_ms>10000)rc=-ETIMEDOUT;
  else if(sample.utc_seconds<VV_RTC_PROVISION_MIN_EPOCH||sample.utc_seconds>VV_RTC_PROVISION_MAX_EPOCH)rc=-ERANGE;
  else{
   uint64_t age=(m-sample.received_mono_ms)/1000;
   if(sample.utc_seconds>UINT64_MAX-age)rc=-ERANGE;
   else{uint64_t verified=sample.utc_seconds+age,diff=u>verified?u-verified:verified-u;
    if(diff>2)rc=-ERANGE;
   }
  }
  if(!rc){
   rc=vv_cloud_trust_arm(&f.trust,m,u,86400000,2,true,true,true);
   if(!rc){f.lease_mono=f.last_mono=m;f.lease_utc=f.last_utc=u;f.lease_expired=false;state=0;}
  }
 }
 if(!rc){f.refresh_failures=0;f.next_refresh=0;}
 else if(state>=0&&rc!=-ECANCELED){
  uint64_t delay=1000u<<(f.refresh_failures<6?f.refresh_failures:6);
  if(delay>60000)delay=60000;
  if(f.refresh_failures<6)++f.refresh_failures;
  f.next_refresh=m>UINT64_MAX-delay?UINT64_MAX:m+delay;
 }
 f.refresh_busy=false;pthread_mutex_unlock(&lock);
 /* Cancellation racing a newly armed anchor must not publish it as usable. */
 if(!rc&&time_cancelled(cancel)){
  pthread_mutex_lock(&lock);f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);pthread_mutex_unlock(&lock);rc=-ECANCELED;
 }
 printf("TRUST_RENEW rc=%d expired=%u retry=%u\n",rc,state==2,f.refresh_failures);
 if(state<0||rc==-ECANCELED)return rc;
 return state==2?1:0;
}
#endif
static bool security(void*a){(void)a;uint64_t u=0;bool ok;
 pthread_mutex_lock(&lock);
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(f.c.renew_trusted_time){
  int state=utc(0,&u)?time_gate(mono(0),u):-EACCES;
  if(state<0){f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);}
  ok=state==0||state==1;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
  if(!ok)first_reject(state==2?REJ_TIME_EXPIRED:(u?REJ_TRUST_UNARMED:REJ_UTC_INVALID),mono(0),u,u?f.time_source_observed:-1);
#endif
 }else
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 {
  bool source=false;uint64_t m=0;
  if(f.prepared&&utc(0,&u)){
   m=mono(0);source=f.entropy.enabled&&!f.entropy.failed&&healthy(0);
   unsigned reason=trust_reasons(m,u,source);
   struct vv_cloud_trust previous=f.trust;
   ok=vv_cloud_trust_check(&f.trust,m,u,source,f.c.ca_approved);
   if(!ok)first_reject_values(reason,m,u,source,&previous);
  }else{ok=false;first_reject(trust_reasons(m,u,source),m,u,-1);}
 }
#else
 ok=f.prepared&&utc(0,&u)&&vv_cloud_trust_check(&f.trust,mono(0),u,
   f.entropy.enabled&&!f.entropy.failed&&healthy(0),f.c.ca_approved);
#endif
 pthread_mutex_unlock(&lock);return ok;
}
static int entropy(void*a,unsigned char*b,size_t n){(void)a;pthread_mutex_lock(&lock);
 int rc=vv_cloud_entropy_read(&f.entropy,b,n);if(rc)vv_cloud_trust_revoke(&f.trust);
 pthread_mutex_unlock(&lock);return rc;
}
static unsigned missing(void*a){(void)a;unsigned bits=f.c.business.missing(f.c.business.arg);
 struct k7radio_link_snapshot s={0};
 if(k7radio_get_link_snapshot(&s)||!s.wifi_connected||!s.ipv4_ready||!(s.ipv4[0]|s.ipv4[1]|s.ipv4[2]|s.ipv4[3]))bits|=VV_NATIVE_NETWORK;
 if(!security(0))bits|=VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA;
 return bits;
}
static int claim(void*a){(void)a;return f.c.business.claim(f.c.business.arg);}
static void release(void*a){(void)a;f.c.business.release(f.c.business.arg);}
static int request(void*a,struct vv_assessment_request*q){(void)a;return f.c.business.make_request(f.c.business.arg,q);}
static void release_request(void*a,struct vv_assessment_request*q){(void)a;f.c.business.release_request(f.c.business.arg,q);}
static void sleep_ms(void*a,unsigned ms){(void)a;struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
int vv_k7_platform_factory_prepare(const struct vv_k7_platform_boot_config*c,uint64_t d,
 struct vv_native_platform_config*out,unsigned*bits){
 if(out)memset(out,0,sizeof(*out));
 if(bits)*bits=VV_NATIVE_CONFIG;
 if(!c||!out||!bits||!c->business.missing||!c->business.claim||!c->business.release||!c->business.make_request||!c->business.release_request)return -EINVAL;
#ifndef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(c->renew_entropy_lease)return -ENOTSUP;
#else
 if(c->renew_entropy_lease&&(!c->authority.trng_development_ns_policy||
    c->authority.trng_source_approved!=vv_k7_trng_development_approved||c->authority.trng_source_arg))return -EACCES;
#endif
 pthread_mutex_lock(&lock);if(f.attempted){pthread_mutex_unlock(&lock);return -EALREADY;}
 f.attempted=true;f.c=*c;int rc=-EAGAIN;*bits=0;
 const char *stage="authority";int os_errno=0;
 if(!c->ca_approved||!c->ca_pem||!c->ca_pem_length||c->ca_pem[c->ca_pem_length-1])*bits|=VV_NATIVE_CA;
 if(!c->trusted_time.present&&!c->authority.rtc_trusted_provisioning_verified)*bits|=VV_NATIVE_TIME;
 if(*bits)goto done;
 struct vv_platform_diag_result result;
 stage="platform_diag";
 rc=vv_cloud_platform_diag_with_trusted_time_once(&f.diag,&c->authority,&c->trusted_time,d,&result);
 printf("VVALIYUN factory=platform_diag rc=%d rtc_rc=%d trng_gate_rc=%d missing=0x%x\n",rc,result.rtc_rc,result.trng_gate_rc,result.missing);
 if(rc){*bits=0;if(result.missing&(VV_PD_RTC_ACCESS|VV_PD_RTC_VALID|VV_PD_RTC_TRUST))*bits|=VV_NATIVE_TIME;
 if(result.missing&~(VV_PD_RTC_ACCESS|VV_PD_RTC_VALID|VV_PD_RTC_TRUST))*bits|=VV_NATIVE_ENTROPY;
 goto done;}
 stage="entropy_bind";
 rc=vv_cloud_platform_diag_bind_entropy(&c->authority,&f.raw);if(rc){*bits=VV_NATIVE_ENTROPY;goto done;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(c->renew_entropy_lease&&f.raw!=vv_k7_trng_development_read){rc=-EACCES;*bits=VV_NATIVE_ENTROPY;goto done;}
#endif
 struct vv_cloud_entropy_config ec={raw,healthy,mono,0,100};
 stage="entropy_configure";
 rc=vv_cloud_entropy_configure(&f.entropy,&ec);if(rc){*bits=VV_NATIVE_ENTROPY;goto done;}
 stage="monotonic_deadline";
 uint64_t m=mono(0);if(m==UINT64_MAX||m>=d){rc=-ETIMEDOUT;*bits=VV_NATIVE_TIME;goto done;}
 struct timespec set={(time_t)result.rtc_utc_candidate,0};
 stage="utc_cast";
 if((uint64_t)set.tv_sec!=result.rtc_utc_candidate){rc=-EIO;*bits=VV_NATIVE_TIME;goto done;}
 stage="clock_settime";
 if(clock_settime(CLOCK_REALTIME,&set)){os_errno=errno;rc=-EIO;*bits=VV_NATIVE_TIME;goto done;}
 stage="utc_readback";
 uint64_t u=0;if(!utc(0,&u)){rc=-EIO;*bits=VV_NATIVE_TIME;goto done;}
 stage="trust_arm";
 rc=vv_cloud_trust_arm(&f.trust,mono(0),u,86400000,2,true,true,true);
 if(rc){*bits=VV_NATIVE_TIME;goto done;}
 stage="dns_init";
 rc=vv_dns_nuttx_backend_init(&f.tcp.dns,0);if(rc){*bits=VV_NATIVE_NETWORK;goto done;}
 f.tcp.ops=vv_cloud_tcp_posix_ops();f.tcp.dns_policy=(struct vv_dns_policy){10000,100,1};
 *out=c->business;out->tls=(struct vv_https_mbedtls_config){c->ca_pem,c->ca_pem_length,vv_cloud_tcp_connect,&f.tcp,security,0,mono,0,entropy,0};
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 out->onset_barge=true;out->scoped_dialogue=true;out->confirmed_barge=false;out->onset_driver=NULL;
#endif
 out->utc_now=utc;out->clock_arg=0;out->missing=missing;out->claim=claim;out->release=release;
 out->make_request=request;out->release_request=release_request;out->sleep_ms=sleep_ms;out->arg=0;
 out->entropy_status=NULL;out->entropy_refresh=NULL;out->entropy_arg=NULL;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(c->renew_entropy_lease){
  struct vv_trng_development_lease l={0};
  rc=vv_k7_trng_development_snapshot(&l);
  if(rc||l.state!=VV_TD_APPROVED||!l.clock_valid||l.expired||!l.remaining_ms||!healthy(NULL)){
   if(!rc)rc=-EACCES;
   *bits=VV_NATIVE_ENTROPY;goto done;
  }
  f.entropy_generation=l.generation;f.entropy_last_mono=l.now_ms;
  out->entropy_status=entropy_status;out->entropy_refresh=entropy_refresh;out->entropy_arg=NULL;
 }
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(c->renew_trusted_time){
  f.lease_mono=f.last_mono=f.trust.anchor_mono_ms;
  f.lease_utc=f.last_utc=f.trust.anchor_utc_s;
  out->trusted_time_status=time_status;out->trusted_time_refresh=time_refresh;out->trusted_time_arg=0;
 }
#else
 if(c->renew_trusted_time){rc=-ENOTSUP;*bits=VV_NATIVE_CONFIG;goto done;}
#endif
 f.prepared=true;rc=0;stage="ready";
 done:printf("VVALIYUN factory=%s rc=%d missing=0x%x os_errno=%d\n",stage,rc,*bits,os_errno);
 if(rc){vv_cloud_trust_revoke(&f.trust);vv_cloud_entropy_disable(&f.entropy);memset(out,0,sizeof(*out));}
 pthread_mutex_unlock(&lock);return rc;
}
#else
int vv_k7_platform_factory_prepare(const struct vv_k7_platform_boot_config*c,uint64_t d,struct vv_native_platform_config*out,unsigned*missing){
 (void)c;(void)d;if(out)memset(out,0,sizeof(*out));if(missing)*missing=VV_NATIVE_CONFIG;return -ENOSYS;
}
#endif

#else
#include "vv_k7_platform_factory.h"
#include "vv_cloud_entropy.h"
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
#include "vv_k7_trng_development.h"
#endif
#include "vv_cloud_trust.h"
#include "vv_cloud_tcp.h"
#include "vv_dns_nuttx.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
int vv_k7_platform_factory_login(struct vv_native_platform*p,vv_k7_secure_load_fn load,
 void*arg,const struct vv_https_cancel*cancel){
 if(!p||!load)return -EINVAL;
 struct vv_k7_secure_login secrets={{0},{0},{0}};
 int rc=load(arg,&secrets);
 if(!rc){
  if(!memchr(secrets.credential,0,sizeof(secrets.credential))||
     !memchr(secrets.version,0,sizeof(secrets.version))||
     !memchr(secrets.proof,0,sizeof(secrets.proof))||!secrets.credential[0])rc=-EINVAL;
  else rc=vv_native_platform_login(p,secrets.credential,secrets.version,secrets.proof,cancel);
 }
 vv_https_secure_zero(&secrets,sizeof(secrets));return rc;
}
#if defined(__NuttX__) && defined(CONFIG_VELAVISION_K7_PLATFORM_FACTORY)
#include "k7_radio_service.h"
#include <pthread.h>
#include <time.h>
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
#include "vv_roughtime_k7.h"
#include "vv_rtc_provision.h"
#endif
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
 bool attempted,prepared;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 bool entropy_fatal,reject_recorded;
 int time_source_observed; /* diagnostic only; -1 when predicate not queried */
 uint64_t entropy_generation,entropy_last_mono;
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 bool renewal_fatal,lease_expired,refresh_busy;
 uint64_t lease_mono,lease_utc,last_mono,last_utc,next_refresh;
 unsigned refresh_failures;
#endif
 struct vv_k7_platform_boot_config c;
 struct vv_platform_diag diag;
 struct vv_cloud_trust trust;
 struct vv_cloud_entropy entropy;
 struct vv_cloud_tcp tcp;
 ssize_t (*raw)(void*,uint8_t*,size_t,uint64_t);
} f;
static uint64_t mono(void*a){struct timespec t;(void)a;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||(uint64_t)t.tv_sec>(UINT64_MAX-999)/1000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static bool utc(void*a,uint64_t*out){struct timespec t;(void)a;if(!out)return false;*out=0;
 if(clock_gettime(CLOCK_REALTIME,&t)||t.tv_sec<=0||t.tv_nsec<0||t.tv_nsec>=1000000000)return false;
 *out=(uint64_t)t.tv_sec;return true;
}
static bool healthy(void*a){(void)a;return f.c.authority.trng_source_approved&&f.c.authority.trng_source_approved(f.c.authority.trng_source_arg);}
static ssize_t raw(void*a,uint8_t*b,size_t n,uint64_t d){(void)a;return f.raw?f.raw(0,b,n,d):-EACCES;}
/* Included in the reviewed factory TU after its raw adapter, not a new
 * runtime module. This file is spliced into the overlay for review/build. */
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
#define ENTROPY_RENEW_WINDOW_MS UINT64_C(600000)
#define ENTROPY_RENEW_BUDGET_MS UINT64_C(100)
enum reject_reason {
 REJ_UNPREPARED=1u,REJ_ENTROPY_OFF=2u,REJ_ENTROPY_FAILED=4u,
 REJ_SOURCE=8u,REJ_CA=16u,REJ_MONO_INVALID=32u,REJ_MONO_ROLLBACK=64u,
 REJ_UTC_INVALID=128u,REJ_UTC_ROLLBACK=256u,REJ_DRIFT=512u,
 REJ_TIME_EXPIRED=1024u,REJ_TRUST_UNARMED=2048u,
 REJ_SOURCE_LEASE=4096u,REJ_RENEW_ERROR=8192u
};
/* Factory lock held. Called only on the first rejected/fatal observation.
 * No decision is made from this diagnostic snapshot. No random bytes/logs. */
static void first_reject_values(unsigned reason,uint64_t m,uint64_t u,int source,const struct vv_cloud_trust*t){
 if(f.reject_recorded)return;
 f.reject_recorded=true;
 struct vv_trng_development_lease l={0};int q=-ENOTSUP;
 if(f.c.authority.trng_development_ns_policy&&
    f.c.authority.trng_source_approved==vv_k7_trng_development_approved&&
    !f.c.authority.trng_source_arg)q=vv_k7_trng_development_snapshot(&l);
 uint64_t am=t->anchor_mono_ms,lm=t->last_mono_ms,au=t->anchor_utc_s,lu=t->last_utc_s,age=t->max_age_ms;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(f.c.renew_trusted_time){am=f.lease_mono;lm=f.last_mono;au=f.lease_utc;lu=f.last_utc;age=86400000;}
#endif
 printf("SECURITY_REJECT reason=%u prepared=%u entropy_enabled=%u entropy_failed=%u source=%d ca=%u mono=%llu utc=%llu trust_armed=%u time_anchor_mono=%llu time_last_mono=%llu time_anchor_utc=%llu time_last_utc=%llu time_max_age=%llu lease_query_rc=%d lease_state=%u lease_remaining=%llu lease_now=%llu lease_expires=%llu\n",
 reason,(unsigned)f.prepared,(unsigned)f.entropy.enabled,(unsigned)f.entropy.failed,
 source,(unsigned)f.c.ca_approved,(unsigned long long)m,(unsigned long long)u,
 (unsigned)t->armed,(unsigned long long)am,(unsigned long long)lm,
 (unsigned long long)au,(unsigned long long)lu,(unsigned long long)age,
 q,q?0u:(unsigned)l.state,q?0:(unsigned long long)l.remaining_ms,
 q?0:(unsigned long long)l.now_ms,q?0:(unsigned long long)l.expires_ms);
}
static void first_reject(unsigned reason,uint64_t m,uint64_t u,int source){first_reject_values(reason,m,u,source,&f.trust);}
static unsigned trust_reasons(uint64_t m,uint64_t u,bool source){
 unsigned b=0;
 if(!f.prepared)b|=REJ_UNPREPARED;
 if(!f.entropy.enabled)b|=REJ_ENTROPY_OFF;
 if(f.entropy.failed)b|=REJ_ENTROPY_FAILED;
 if(!source)b|=REJ_SOURCE;
 if(!f.c.ca_approved)b|=REJ_CA;
 if(m==UINT64_MAX)b|=REJ_MONO_INVALID;
 else if(m<f.trust.last_mono_ms||m<f.trust.anchor_mono_ms)b|=REJ_MONO_ROLLBACK;
 if(!u)b|=REJ_UTC_INVALID;
 else if(u<f.trust.last_utc_s)b|=REJ_UTC_ROLLBACK;
 if(!f.trust.armed)b|=REJ_TRUST_UNARMED;
 if(f.trust.armed&&m!=UINT64_MAX&&m>=f.trust.anchor_mono_ms){
  uint64_t d=(m-f.trust.anchor_mono_ms)/1000;
  if(m-f.trust.anchor_mono_ms>=f.trust.max_age_ms)b|=REJ_TIME_EXPIRED;
  if(f.trust.anchor_utc_s>UINT64_MAX-d)b|=REJ_DRIFT;
  else {uint64_t expected=f.trust.anchor_utc_s+d;
   if((u>expected?u-expected:expected-u)>f.trust.max_drift_s)b|=REJ_DRIFT;}
 }
 return b;
}
static int entropy_fatal(int rc,unsigned why,const struct vv_trng_development_lease*l){
 /* No fabricated healthy() observation on early snapshot/renew failure. */
 first_reject(why,l?l->now_ms:mono(0),0,-1);
 f.entropy_fatal=true;vv_cloud_entropy_disable(&f.entropy);
 vv_cloud_trust_revoke(&f.trust);return rc<0?rc:-EACCES;
}
/* Status is a read-only policy observation, never a source probe. Factory
 * remembers its own last observed monotonic value to reject rollback. */
static int entropy_state_locked(struct vv_trng_development_lease*l){
 if(!f.prepared||!f.c.renew_entropy_lease||f.entropy_fatal)return -EACCES;
 int rc=vv_k7_trng_development_snapshot(l);
 if(rc==-EBUSY)return rc; /* Only this outer readonly entry is proven pure. */
 if(rc)return entropy_fatal(rc,REJ_SOURCE_LEASE,NULL);
 if(l->state!=VV_TD_APPROVED||l->generation!=f.entropy_generation||
    !l->clock_valid||l->expired||!l->remaining_ms||l->now_ms<f.entropy_last_mono)
  return entropy_fatal(-EACCES,REJ_SOURCE_LEASE,l);
 if(!f.entropy.enabled||f.entropy.failed||!healthy(NULL))
  return entropy_fatal(-EACCES,REJ_SOURCE,l);
 /* Source renewal never repairs clock integrity. Observe existing independent
  * time anchors without arming/revoking/extending them or fetching new time. */
 uint64_t u=0,m=mono(NULL),am=f.trust.anchor_mono_ms,au=f.trust.anchor_utc_s;
 uint64_t lm=f.trust.last_mono_ms,lu=f.trust.last_utc_s;
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(f.c.renew_trusted_time){am=f.lease_mono;au=f.lease_utc;lm=f.last_mono;lu=f.last_utc;}
#endif
 unsigned clock_reason=0;
 if(m==UINT64_MAX)clock_reason|=REJ_MONO_INVALID;
 else if(m<am||m<lm)clock_reason|=REJ_MONO_ROLLBACK;
 if(!utc(NULL,&u)||!u)clock_reason|=REJ_UTC_INVALID;
 else if(u<lu)clock_reason|=REJ_UTC_ROLLBACK;
 if(m!=UINT64_MAX&&m>=am){
  uint64_t d=(m-am)/1000;
  if(au>UINT64_MAX-d)clock_reason|=REJ_DRIFT;
  else{uint64_t expected=au+d;if((u>expected?u-expected:expected-u)>2)clock_reason|=REJ_DRIFT;}
 }
 if(clock_reason){first_reject(clock_reason,m,u,true);return entropy_fatal(-EACCES,clock_reason,l);}
 f.entropy_last_mono=l->now_ms;
 return l->remaining_ms<=ENTROPY_RENEW_WINDOW_MS?1:0;
}
static int entropy_status(void *arg){
 (void)arg;struct vv_trng_development_lease l={0};pthread_mutex_lock(&lock);
 int rc=entropy_state_locked(&l);pthread_mutex_unlock(&lock);return rc;
}
static bool entropy_cancelled(const struct vv_https_cancel*c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static int entropy_refresh(void *arg,const struct vv_https_cancel*cancel){
 (void)arg;
 /* Never call this token while holding factory lock: a caller's safe-point
  * health callback may consult security/status. Raw only sees policy/context. */
 if(entropy_cancelled(cancel))return -ECANCELED;
 struct vv_trng_development_lease before={0},after={0};pthread_mutex_lock(&lock);
 int rc=entropy_state_locked(&before);
 if(rc==-EBUSY)rc=1;
 else if(rc==1){
  uint64_t deadline=before.now_ms>UINT64_MAX-ENTROPY_RENEW_BUDGET_MS?UINT64_MAX:before.now_ms+ENTROPY_RENEW_BUDGET_MS;
  if(deadline>before.expires_ms)deadline=before.expires_ms;
  rc=vv_k7_trng_development_renew(f.entropy_generation,deadline);
  int q=vv_k7_trng_development_snapshot(&after);
  if(rc==-EBUSY&&!q&&after.state==VV_TD_APPROVED&&
     after.generation==before.generation&&after.expires_ms==before.expires_ms&&
     after.last_validation_ms==before.last_validation_ms)rc=1;
  else if(rc||q||after.state!=VV_TD_APPROVED||after.generation!=f.entropy_generation||
     !after.clock_valid||after.expired||!after.remaining_ms||!healthy(NULL))
   rc=entropy_fatal(rc<0?rc:-EACCES,REJ_RENEW_ERROR,q?NULL:&after);
  else f.entropy_last_mono=after.now_ms;
 }
 pthread_mutex_unlock(&lock);
 if(entropy_cancelled(cancel)){
  pthread_mutex_lock(&lock);f.entropy_fatal=true;vv_cloud_entropy_disable(&f.entropy);
  vv_cloud_trust_revoke(&f.trust);vv_k7_trng_development_revoke();pthread_mutex_unlock(&lock);
  return -ECANCELED;
 }
 return rc;
}
#endif

#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
/* lock held. Expiry is the only recoverable trust loss. Keep an independent
 * anchor so clearing the expired trust cannot erase rollback/error evidence. */
static int time_gate(uint64_t m,uint64_t u){
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 f.time_source_observed=-1;
#endif
 if(!f.prepared||f.renewal_fatal||!f.c.renew_trusted_time)return -EACCES;
 bool source=f.entropy.enabled&&!f.entropy.failed&&healthy(0);
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 f.time_source_observed=source;
#endif
 if(!source||!f.c.ca_approved||m==UINT64_MAX||m<f.last_mono||u<f.last_utc||
    m<f.lease_mono||!u)goto fatal;
 uint64_t elapsed=(m-f.lease_mono)/1000;
 if(f.lease_utc>UINT64_MAX-elapsed)goto fatal;
 uint64_t expected=f.lease_utc+elapsed,diff=u>expected?u-expected:expected-u;
 if(diff>2||(!f.trust.armed&&!f.lease_expired))goto fatal;
 f.last_mono=m;f.last_utc=u;
 if(m-f.lease_mono>=UINT64_C(86400000)){
  f.lease_expired=true;vv_cloud_trust_revoke(&f.trust);return 2;
 }
 if(!vv_cloud_trust_check(&f.trust,m,u,source,f.c.ca_approved))goto fatal;
 return m-f.lease_mono>=UINT64_C(82800000)?1:0;
fatal:
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 first_reject(trust_reasons(m,u,source),m,u,source);
#endif
 f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);return -EACCES;
}
static int time_status(void *arg){
 (void)arg;uint64_t u=0;pthread_mutex_lock(&lock);
 int rc=utc(0,&u)?time_gate(mono(0),u):-EACCES;
 if(rc<0){
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
  first_reject(u?REJ_TRUST_UNARMED:REJ_UTC_INVALID,mono(NULL),u,-1);
#endif
  f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);
 }
 pthread_mutex_unlock(&lock);return rc;
}
static bool time_cancelled(const struct vv_https_cancel *c){
 return c&&c->is_cancelled&&c->is_cancelled(c->arg);
}
static int time_refresh(void *arg,const struct vv_https_cancel *cancel){
 (void)arg;
 if(time_cancelled(cancel))return -ECANCELED;
 pthread_mutex_lock(&lock);uint64_t u=0,m=mono(0);
 int state=utc(0,&u)?time_gate(m,u):-EACCES;
 if(state<0){
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
  first_reject(u?REJ_TRUST_UNARMED:REJ_UTC_INVALID,m,u,-1);
#endif
  f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);pthread_mutex_unlock(&lock);return state;
 }
 if(!state||m<f.next_refresh){pthread_mutex_unlock(&lock);return state==2?1:0;}
 if(f.refresh_busy){pthread_mutex_unlock(&lock);return -EBUSY;}
 f.refresh_busy=true;pthread_mutex_unlock(&lock);
 /* No factory lock is held over network/entropy callbacks. No ordinary TLS
  * is used to authenticate time and no expired trust is provisionally armed. */
 struct vv_trusted_time_sample sample={0};
 int rc=vv_roughtime_k7_fetch_runtime(&f.c.authority,&f.tcp,cancel,&sample);
 bool cancelled=time_cancelled(cancel);
 pthread_mutex_lock(&lock);m=mono(0);u=0;
 state=utc(0,&u)?time_gate(m,u):-EACCES;
 if(state<0){f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);rc=state;}
 else if(cancelled)rc=-ECANCELED;
 else if(!rc){
  /* Authenticated new authorization, not a lease extension based on old RTC.
   * We intentionally do not step either system clock or RTC at runtime. */
  if(!sample.present||!sample.authorized||sample.received_mono_ms>m)rc=-EACCES;
  else if(m-sample.received_mono_ms>10000)rc=-ETIMEDOUT;
  else if(sample.utc_seconds<VV_RTC_PROVISION_MIN_EPOCH||sample.utc_seconds>VV_RTC_PROVISION_MAX_EPOCH)rc=-ERANGE;
  else{
   uint64_t age=(m-sample.received_mono_ms)/1000;
   if(sample.utc_seconds>UINT64_MAX-age)rc=-ERANGE;
   else{uint64_t verified=sample.utc_seconds+age,diff=u>verified?u-verified:verified-u;
    if(diff>2)rc=-ERANGE;
   }
  }
  if(!rc){
   rc=vv_cloud_trust_arm(&f.trust,m,u,86400000,2,true,true,true);
   if(!rc){f.lease_mono=f.last_mono=m;f.lease_utc=f.last_utc=u;f.lease_expired=false;state=0;}
  }
 }
 if(!rc){f.refresh_failures=0;f.next_refresh=0;}
 else if(state>=0&&rc!=-ECANCELED){
  uint64_t delay=1000u<<(f.refresh_failures<6?f.refresh_failures:6);
  if(delay>60000)delay=60000;
  if(f.refresh_failures<6)++f.refresh_failures;
  f.next_refresh=m>UINT64_MAX-delay?UINT64_MAX:m+delay;
 }
 f.refresh_busy=false;pthread_mutex_unlock(&lock);
 /* Cancellation racing a newly armed anchor must not publish it as usable. */
 if(!rc&&time_cancelled(cancel)){
  pthread_mutex_lock(&lock);f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);pthread_mutex_unlock(&lock);rc=-ECANCELED;
 }
 printf("TRUST_RENEW rc=%d expired=%u retry=%u\n",rc,state==2,f.refresh_failures);
 if(state<0||rc==-ECANCELED)return rc;
 return state==2?1:0;
}
#endif
static bool security(void*a){(void)a;uint64_t u=0;bool ok;
 pthread_mutex_lock(&lock);
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(f.c.renew_trusted_time){
  int state=utc(0,&u)?time_gate(mono(0),u):-EACCES;
  if(state<0){f.renewal_fatal=true;vv_cloud_trust_revoke(&f.trust);}
  ok=state==0||state==1;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
  if(!ok)first_reject(state==2?REJ_TIME_EXPIRED:(u?REJ_TRUST_UNARMED:REJ_UTC_INVALID),mono(0),u,u?f.time_source_observed:-1);
#endif
 }else
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 {
  bool source=false;uint64_t m=0;
  if(f.prepared&&utc(0,&u)){
   m=mono(0);source=f.entropy.enabled&&!f.entropy.failed&&healthy(0);
   unsigned reason=trust_reasons(m,u,source);
   struct vv_cloud_trust previous=f.trust;
   ok=vv_cloud_trust_check(&f.trust,m,u,source,f.c.ca_approved);
   if(!ok)first_reject_values(reason,m,u,source,&previous);
  }else{ok=false;first_reject(trust_reasons(m,u,source),m,u,-1);}
 }
#else
 ok=f.prepared&&utc(0,&u)&&vv_cloud_trust_check(&f.trust,mono(0),u,
   f.entropy.enabled&&!f.entropy.failed&&healthy(0),f.c.ca_approved);
#endif
 pthread_mutex_unlock(&lock);return ok;
}
static int entropy(void*a,unsigned char*b,size_t n){(void)a;pthread_mutex_lock(&lock);
 int rc=vv_cloud_entropy_read(&f.entropy,b,n);if(rc)vv_cloud_trust_revoke(&f.trust);
 pthread_mutex_unlock(&lock);return rc;
}
static unsigned missing(void*a){(void)a;unsigned bits=f.c.business.missing(f.c.business.arg);
 struct k7radio_link_snapshot s={0};
 if(k7radio_get_link_snapshot(&s)||!s.wifi_connected||!s.ipv4_ready||!(s.ipv4[0]|s.ipv4[1]|s.ipv4[2]|s.ipv4[3]))bits|=VV_NATIVE_NETWORK;
 if(!security(0))bits|=VV_NATIVE_TIME|VV_NATIVE_ENTROPY|VV_NATIVE_CA;
 return bits;
}
static int claim(void*a){(void)a;return f.c.business.claim(f.c.business.arg);}
static void release(void*a){(void)a;f.c.business.release(f.c.business.arg);}
static int request(void*a,struct vv_assessment_request*q){(void)a;return f.c.business.make_request(f.c.business.arg,q);}
static void release_request(void*a,struct vv_assessment_request*q){(void)a;f.c.business.release_request(f.c.business.arg,q);}
static void sleep_ms(void*a,unsigned ms){(void)a;struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
int vv_k7_platform_factory_prepare(const struct vv_k7_platform_boot_config*c,uint64_t d,
 struct vv_native_platform_config*out,unsigned*bits){
 if(out)memset(out,0,sizeof(*out));
 if(bits)*bits=VV_NATIVE_CONFIG;
 if(!c||!out||!bits||!c->business.missing||!c->business.claim||!c->business.release||!c->business.make_request||!c->business.release_request)return -EINVAL;
#ifndef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(c->renew_entropy_lease)return -ENOTSUP;
#else
 if(c->renew_entropy_lease&&(!c->authority.trng_development_ns_policy||
    c->authority.trng_source_approved!=vv_k7_trng_development_approved||c->authority.trng_source_arg))return -EACCES;
#endif
 pthread_mutex_lock(&lock);if(f.attempted){pthread_mutex_unlock(&lock);return -EALREADY;}
 f.attempted=true;f.c=*c;int rc=-EAGAIN;*bits=0;
 const char *stage="authority";int os_errno=0;
 if(!c->ca_approved||!c->ca_pem||!c->ca_pem_length||c->ca_pem[c->ca_pem_length-1])*bits|=VV_NATIVE_CA;
 if(!c->trusted_time.present&&!c->authority.rtc_trusted_provisioning_verified)*bits|=VV_NATIVE_TIME;
 if(*bits)goto done;
 struct vv_platform_diag_result result;
 stage="platform_diag";
 rc=vv_cloud_platform_diag_with_trusted_time_once(&f.diag,&c->authority,&c->trusted_time,d,&result);
 printf("VVALIYUN factory=platform_diag rc=%d rtc_rc=%d trng_gate_rc=%d missing=0x%x\n",rc,result.rtc_rc,result.trng_gate_rc,result.missing);
 if(rc){*bits=0;if(result.missing&(VV_PD_RTC_ACCESS|VV_PD_RTC_VALID|VV_PD_RTC_TRUST))*bits|=VV_NATIVE_TIME;
 if(result.missing&~(VV_PD_RTC_ACCESS|VV_PD_RTC_VALID|VV_PD_RTC_TRUST))*bits|=VV_NATIVE_ENTROPY;
 goto done;}
 stage="entropy_bind";
 rc=vv_cloud_platform_diag_bind_entropy(&c->authority,&f.raw);if(rc){*bits=VV_NATIVE_ENTROPY;goto done;}
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(c->renew_entropy_lease&&f.raw!=vv_k7_trng_development_read){rc=-EACCES;*bits=VV_NATIVE_ENTROPY;goto done;}
#endif
 struct vv_cloud_entropy_config ec={raw,healthy,mono,0,100};
 stage="entropy_configure";
 rc=vv_cloud_entropy_configure(&f.entropy,&ec);if(rc){*bits=VV_NATIVE_ENTROPY;goto done;}
 stage="monotonic_deadline";
 uint64_t m=mono(0);if(m==UINT64_MAX||m>=d){rc=-ETIMEDOUT;*bits=VV_NATIVE_TIME;goto done;}
 struct timespec set={(time_t)result.rtc_utc_candidate,0};
 stage="utc_cast";
 if((uint64_t)set.tv_sec!=result.rtc_utc_candidate){rc=-EIO;*bits=VV_NATIVE_TIME;goto done;}
 stage="clock_settime";
 if(clock_settime(CLOCK_REALTIME,&set)){os_errno=errno;rc=-EIO;*bits=VV_NATIVE_TIME;goto done;}
 stage="utc_readback";
 uint64_t u=0;if(!utc(0,&u)){rc=-EIO;*bits=VV_NATIVE_TIME;goto done;}
 stage="trust_arm";
 rc=vv_cloud_trust_arm(&f.trust,mono(0),u,86400000,2,true,true,true);
 if(rc){*bits=VV_NATIVE_TIME;goto done;}
 stage="dns_init";
 rc=vv_dns_nuttx_backend_init(&f.tcp.dns,0);if(rc){*bits=VV_NATIVE_NETWORK;goto done;}
 f.tcp.ops=vv_cloud_tcp_posix_ops();f.tcp.dns_policy=(struct vv_dns_policy){10000,100,1};
 *out=c->business;out->tls=(struct vv_https_mbedtls_config){c->ca_pem,c->ca_pem_length,vv_cloud_tcp_connect,&f.tcp,security,0,mono,0,entropy,0};
 out->utc_now=utc;out->clock_arg=0;out->missing=missing;out->claim=claim;out->release=release;
 out->make_request=request;out->release_request=release_request;out->sleep_ms=sleep_ms;out->arg=0;
 out->entropy_status=NULL;out->entropy_refresh=NULL;out->entropy_arg=NULL;
#ifdef CONFIG_EXAMPLES_K7CLOUD_ENTROPY_MAINTENANCE
 if(c->renew_entropy_lease){
  struct vv_trng_development_lease l={0};
  rc=vv_k7_trng_development_snapshot(&l);
  if(rc||l.state!=VV_TD_APPROVED||!l.clock_valid||l.expired||!l.remaining_ms||!healthy(NULL)){
   if(!rc)rc=-EACCES;
   *bits=VV_NATIVE_ENTROPY;goto done;
  }
  f.entropy_generation=l.generation;f.entropy_last_mono=l.now_ms;
  out->entropy_status=entropy_status;out->entropy_refresh=entropy_refresh;out->entropy_arg=NULL;
 }
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_TRUST_RENEWAL
 if(c->renew_trusted_time){
  f.lease_mono=f.last_mono=f.trust.anchor_mono_ms;
  f.lease_utc=f.last_utc=f.trust.anchor_utc_s;
  out->trusted_time_status=time_status;out->trusted_time_refresh=time_refresh;out->trusted_time_arg=0;
 }
#else
 if(c->renew_trusted_time){rc=-ENOTSUP;*bits=VV_NATIVE_CONFIG;goto done;}
#endif
 f.prepared=true;rc=0;stage="ready";
 done:printf("VVALIYUN factory=%s rc=%d missing=0x%x os_errno=%d\n",stage,rc,*bits,os_errno);
 if(rc){vv_cloud_trust_revoke(&f.trust);vv_cloud_entropy_disable(&f.entropy);memset(out,0,sizeof(*out));}
 pthread_mutex_unlock(&lock);return rc;
}
#else
int vv_k7_platform_factory_prepare(const struct vv_k7_platform_boot_config*c,uint64_t d,struct vv_native_platform_config*out,unsigned*missing){
 (void)c;(void)d;if(out)memset(out,0,sizeof(*out));if(missing)*missing=VV_NATIVE_CONFIG;return -ENOSYS;
}
#endif

#endif
