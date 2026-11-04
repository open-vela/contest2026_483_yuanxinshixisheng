#include "vv_cloud_platform_diag.h"
#include "vv_rtc_i2c2.h"
#include "vv_rtc_provision_i2c2.h"
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#include <unistd.h>
#endif
#ifdef CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT
#include "vv_k7_trng_development.h"
#endif
/* Proxy preserves the hardware owner/argument while adding boot cancellation. */
struct time_ops {struct vv_rtc_provision_ops original;const struct vv_trusted_time_sample *sample;};
static int sample_acquire(void*p){struct time_ops*x=p;return x->original.reader.acquire(x->original.reader.arg);}
static int sample_read(void*p,uint8_t reg,uint8_t*v,uint64_t d){struct time_ops*x=p;return x->original.reader.read_register(x->original.reader.arg,reg,v,d);}
static int sample_release(void*p,uint64_t d){struct time_ops*x=p;return x->original.reader.release(x->original.reader.arg,d);}
static uint64_t sample_now(void*p){struct time_ops*x=p;return x->original.reader.now_ms(x->original.reader.arg);}
static bool sample_cancel(void*p){struct time_ops*x=p;return
 (x->sample->cancelled&&x->sample->cancelled(x->sample->cancel_arg))||
 (x->original.reader.cancelled&&x->original.reader.cancelled(x->original.reader.arg));}
static int sample_calendar(void*p,const uint8_t c[7],uint64_t d){struct time_ops*x=p;return x->original.write_calendar(x->original.reader.arg,c,d);}
static int sample_control(void*p,uint8_t c,uint64_t d){struct time_ops*x=p;return x->original.write_control1(x->original.reader.arg,c,d);}
static void sample_poison(void*p){struct time_ops*x=p;x->original.poison(x->original.reader.arg);}
int vv_trusted_time_initialize(struct vv_rtc_provision*s,
 const struct vv_trusted_time_sample*q,uint64_t deadline,struct vv_rtc_provision_result*r){
 if(r)memset(r,0,sizeof(*r));
 if(!s||!q||!r||!q->present||!s->ops.reader.now_ms)return -EINVAL;
 if(s->attempted)return -EALREADY;
 if(!q->authorized)return -EACCES;
 uint64_t now=s->ops.reader.now_ms(s->ops.reader.arg);
 if(now==UINT64_MAX||now<q->received_mono_ms)return -EIO;
 if(now-q->received_mono_ms>10000||now>=deadline)return -ETIMEDOUT;
 if(q->utc_seconds<VV_RTC_PROVISION_MIN_EPOCH||q->utc_seconds>VV_RTC_PROVISION_MAX_EPOCH)return -ERANGE;
 uint64_t adjusted=q->utc_seconds+(now-q->received_mono_ms)/1000;
 if(adjusted>VV_RTC_PROVISION_MAX_EPOCH)return -ERANGE;
 if(!s->ops.reader.acquire||!s->ops.reader.release||!s->ops.reader.read_register||
    !s->ops.write_calendar||!s->ops.write_control1||!s->ops.poison)return -EINVAL;
 struct time_ops proxy={s->ops,q};
 if(sample_cancel(&proxy))return -ECANCELED;
 struct vv_rtc_provision local={0};
 local.ops=(struct vv_rtc_provision_ops){
  {sample_acquire,sample_read,sample_release,sample_now,sample_cancel,&proxy},
  sample_calendar,sample_control,sample_poison};
 char text[24];snprintf(text,sizeof(text),"%" PRIu64,adjusted);
 /* The caller's instance remains sealed even if acquire/write/cleanup fails. */
 s->attempted=true;
 int rc=vv_rtc_provision_initialize_cli(&local,true,text,deadline,r);
 if(!rc&&sample_cancel(&proxy))rc=-ECANCELED;
 return rc;
}
static bool development_ready(const struct vv_platform_diag_authority*a){
#ifdef CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT
 return a->trng_development_ns_policy&&
  !a->trng_mapping_reset_parent_owner_verified&&
  a->trng_source_approved==vv_k7_trng_development_approved&&
  !a->trng_source_arg&&vv_k7_trng_development_approved(0);
#else
 (void)a;return false;
#endif
}
static int gate_check(const struct vv_platform_diag_authority*a){
 if(!a->cru_mapping_access_verified)return -EACCES;
#if defined(__NuttX__) && defined(CONFIG_VELAVISION_CLOUD_PLATFORM_DIAG)
 /* No probe can safely infer access permissions by risking an abort. The
  * caller attests that CRU mapping/security ownership is already verified. */
 uint32_t gate=*(volatile const uint32_t *)(uintptr_t)0x27200c48;
 return gate&(1u<<13)?-ENODEV:0;
#else
 return -ENOSYS;
#endif
}
int vv_cloud_platform_diag_with_trusted_time_once(struct vv_platform_diag*s,
 const struct vv_platform_diag_authority*a,const struct vv_trusted_time_sample*sample,uint64_t deadline,
 struct vv_platform_diag_result*r){
 if(!s||!a||!r)return -EINVAL;
 memset(r,0,sizeof(*r));
 if(s->attempted){r->rtc_rc=-EALREADY;r->trng_gate_rc=-EALREADY;r->missing=127u;return -EALREADY;}
 s->attempted=true;
 r->rtc_rc=-EACCES;
 if(!a->rtc_access_owner_verified)r->missing|=VV_PD_RTC_ACCESS;
 else{
  r->rtc_rc=vv_rtc_i2c2_platform_ops(false,&s->rtc.ops);
  if(!r->rtc_rc)r->rtc_rc=vv_rtc_i2c2_platform_prepare(true,deadline);
  if(!r->rtc_rc&&sample&&sample->present){
   struct vv_rtc_provision provision={0};struct vv_rtc_provision_result pr={0};
   r->rtc_rc=vv_rtc_i2c2_platform_provision_ops(&provision.ops);
   if(!r->rtc_rc)r->rtc_rc=vv_trusted_time_initialize(&provision,sample,deadline,&pr);
   r->trusted_time_initialized=!r->rtc_rc&&pr.readback_valid&&pr.running_verified;
   printf("CLOUD_TRUSTED_TIME rc=%d valid=%d running=%d write_attempted=%d\n",
          r->rtc_rc,pr.readback_valid,pr.running_verified,pr.write_attempted);
  }
  if(!r->rtc_rc)r->rtc_rc=vv_rtc_reader_read(&s->rtc,deadline,&r->rtc_utc_candidate);
  if(!r->rtc_rc&&sample&&sample->present&&sample->cancelled&&sample->cancelled(sample->cancel_arg)){
   r->rtc_rc=-ECANCELED;r->rtc_utc_candidate=0;r->trusted_time_initialized=false;
  }
 }
 if(r->rtc_rc)r->missing|=VV_PD_RTC_VALID;
 if((sample&&sample->present)?(!r->trusted_time_initialized||r->rtc_rc):!a->rtc_trusted_provisioning_verified)r->missing|=VV_PD_RTC_TRUST;
 r->trng_gate_rc=gate_check(a);
 if(!a->cru_mapping_access_verified)r->missing|=VV_PD_CRU_ACCESS;
 if(r->trng_gate_rc)r->missing|=VV_PD_TRNG_GATE;
 if(a->trng_development_ns_policy?!development_ready(a):!a->trng_mapping_reset_parent_owner_verified)r->missing|=VV_PD_TRNG_OWNERSHIP;
 if(!a->trng_source_approved||!a->trng_source_approved(a->trng_source_arg))r->missing|=VV_PD_TRNG_HEALTH;
 return r->missing?-EAGAIN:0;
}
int vv_cloud_platform_diag_once(struct vv_platform_diag*s,
 const struct vv_platform_diag_authority*a,uint64_t d,struct vv_platform_diag_result*r){
 return vv_cloud_platform_diag_with_trusted_time_once(s,a,0,d,r);
}
static void diag_pause(void){
 fflush(stdout);
#ifdef __NuttX__
 usleep(5000);
#endif
}
static void diag_line(const char *s){puts(s);diag_pause();}
void vv_cloud_platform_diag_print(const struct vv_platform_diag_result*r){
 if(!r)return;
 printf("CLOUD_PLATFORM rtc_rc=%d utc_candidate=%" PRIu64 " trng_gate_rc=%d missing=0x%x tls_armed=0 random_logged=0\n",r->rtc_rc,r->rtc_utc_candidate,r->trng_gate_rc,r->missing);diag_pause();
 if(r->missing&VV_PD_RTC_ACCESS)diag_line("CLOUD_PLATFORM missing=rtc_access_and_exclusive_owner");
 if(r->missing&VV_PD_RTC_VALID)diag_line("CLOUD_PLATFORM missing=stable_valid_rtc_read");
 if(r->missing&VV_PD_RTC_TRUST)diag_line("CLOUD_PLATFORM missing=trusted_rtc_provisioning_and_retention");
 if(r->missing&VV_PD_CRU_ACCESS)diag_line("CLOUD_PLATFORM missing=verified_cru_access");
 if(r->missing&VV_PD_TRNG_GATE)diag_line("CLOUD_PLATFORM missing=trng_leaf_clock_enabled");
 if(r->missing&VV_PD_TRNG_OWNERSHIP)diag_line("CLOUD_PLATFORM missing=trng_mapping_reset_parent_and_owner");
 if(r->missing&VV_PD_TRNG_HEALTH)diag_line("CLOUD_PLATFORM missing=approved_entropy_source_policy");
}
int vv_cloud_platform_diag_bind_entropy(const struct vv_platform_diag_authority*a,
 ssize_t (**out)(void*,uint8_t*,size_t,uint64_t)){
 if(!out)return -EINVAL;
 *out=0;
 if(a&&a->trng_development_ns_policy){
  if(!development_ready(a))return -EACCES;
  int rc=gate_check(a);if(rc)return rc;
#ifdef CONFIG_VELAVISION_TRNG_NS_DEVELOPMENT
  *out=vv_k7_trng_development_read;return 0;
#else
  return -ENOSYS;
#endif
 }
 if(!a||!a->trng_mapping_reset_parent_owner_verified||!a->trng_source_approved||
    !a->trng_source_approved(a->trng_source_arg))return -EACCES;
 int rc=gate_check(a);if(rc)return rc;
 rc=vv_trng_nuttx_configure(true,a->trng_source_approved,a->trng_source_arg);
 if(!rc)*out=vv_trng_nuttx_read_exact;
 return rc;
}
