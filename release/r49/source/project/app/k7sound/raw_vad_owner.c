/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "raw_vad_owner.h"
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "k7_apm21_nominal_identity.h"
#include "k7_apm21_product_startup.h"
#endif
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
#include "k7_apm21_prepared_lifecycle.h"
#if !defined(K7_ECHO_RESIDUAL_EVIDENCE) || !K7_ECHO_RESIDUAL_EVIDENCE || !defined(K7_ECHO_RAW_VAD_OBSERVER) || !K7_ECHO_RAW_VAD_OBSERVER
#error prepared lifecycle requires the exclusive existing residual model owner
#endif
#endif
#line 3
#include "vad_probe_asset.h"
#include "k7_vad_ort.h"
#include "vv_native_service.h"
#include "vv_voice_session.h"
#include "k7boot_aliyun_sequence.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Opt-in diagnostic factory, one model, no worker until existing manager open.
 * Try-gate never enters the sink. It also serializes destroy versus new voice
 * admission. No network, threshold, cancellation or control action here. */
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
static atomic_uintptr_t product_policy_token;
static uint64_t product_arm_serial,product_boot_claim;
#endif
static atomic_flag control=ATOMIC_FLAG_INIT;
static unsigned phase;
static int last_error;
static bool admission_fault;
static uint64_t model_generation,loan_sequence,voice_owner;
static struct k7_vad_probe_lease asset;
static struct k7_vad_ort *backend;
static struct k7_vad *pipeline;
static struct k7_raw_vad_loan *borrow_ticket;
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
static struct k7_apm21_live_binding diagnostic_binding;
static bool diagnostic_armed;
static atomic_uint_fast64_t diagnostic_claimed_owner;
#endif
#line 27
static struct k7_echo_runtime **borrow_slot;
static atomic_uint_fast64_t borrow_owner,windows,last_window_end,invalid_reports;
static atomic_uint peak_bits;
static int enter(void){return atomic_flag_test_and_set_explicit(&control,memory_order_acquire)?-EBUSY:0;}
static void leave(void){atomic_flag_clear_explicit(&control,memory_order_release);}
static int idle(void){
 struct vv_native_service_status s;int rc=vv_native_service_status(&s);
 /* Atomic pointer comparison only: no borrowed audio dereference. The voice
  * marker under control is the creation/teardown exclusion, before audio. */
 return rc?rc:(voice_owner||s.running||s.stopping||vv_voice_session_audio())?-EBUSY:0;
}
static int retire(void){
 k7_vad_destroy(pipeline);pipeline=NULL;
 k7_vad_ort_destroy(backend);backend=NULL;
 int rc=asset.id?k7_vad_probe_asset_release(&asset):0;
 phase=rc||admission_fault?K7_RAW_OWNER_RETAINED:K7_RAW_OWNER_EMPTY;
 return rc;
}
int k7_raw_vad_owner_prepare(void){
 int rc=enter();if(rc)return rc;
 bool admitted=false;
 if(phase!=K7_RAW_OWNER_EMPTY){rc=phase==K7_RAW_OWNER_READY?-EALREADY:-EBUSY;goto out;}
 rc=idle();if(rc)goto out;
 if(model_generation==UINT64_MAX){rc=-EOVERFLOW;goto out;}
 rc=k7boot_aliyun_vad_probe_acquire();if(rc)goto out;
 admitted=true;phase=K7_RAW_OWNER_PREPARING;
 rc=idle();
 if(!rc)rc=k7_vad_probe_asset_acquire(&asset);
 if(!rc){
  struct k7_vad_model m={asset.data,asset.bytes,k7_vad_probe_hash,NULL};
  rc=k7_vad_ort_create(OrtGetApiBase(),&m,&backend);
 }
 if(!rc)rc=k7_vad_create(k7_vad_ort_ops(),backend,&pipeline);
 if(!rc)rc=idle();
 if(rc){int cleanup=retire();if(cleanup)rc=cleanup;}
 else {model_generation++;phase=K7_RAW_OWNER_READY;}
 if(admitted){
  int released=k7boot_aliyun_vad_probe_release();
  /* A broken admission release retains the model/asset until explicit clear.
   * Do not advertise READY or silently let a voice owner through. */
  if(released){admission_fault=true;phase=K7_RAW_OWNER_RETAINED;rc=released;}
 }
out:last_error=rc;leave();return rc;
}
int k7_raw_vad_owner_clear(void){
 int rc=enter();if(rc)return rc;
 #if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(product_boot_claim){leave();return -EBUSY;}
#endif
 if(phase==K7_RAW_OWNER_EMPTY)rc=idle();
 else if(phase!=K7_RAW_OWNER_READY&&phase!=K7_RAW_OWNER_RETAINED)rc=-EBUSY;
 else if(!(rc=idle()))rc=retire();
 #if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 if(!rc&&phase==K7_RAW_OWNER_EMPTY){diagnostic_armed=false;atomic_store(&diagnostic_claimed_owner,0);memset(&diagnostic_binding,0,sizeof(diagnostic_binding));}
 #endif
 last_error=rc;leave();return rc;
}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
int k7_raw_vad_owner_product_startup_ready(uint64_t *token){
 if(!token)return -EINVAL;
 *token=0;int rc=enter();if(rc)return rc;
 if(product_boot_claim||voice_owner||!diagnostic_armed||!diagnostic_binding.product_experiment||
    (phase!=K7_RAW_OWNER_EMPTY&&phase!=K7_RAW_OWNER_READY))rc=-EAGAIN;
 else *token=product_arm_serial;
 leave();return rc;
}
int k7_raw_vad_owner_product_startup_claim(uint64_t token){
 if(!token)return -EINVAL;
 int rc=enter();if(rc)return rc;
 if(product_boot_claim||voice_owner||!diagnostic_armed||!diagnostic_binding.product_experiment||
    token!=product_arm_serial||(phase!=K7_RAW_OWNER_EMPTY&&phase!=K7_RAW_OWNER_READY))rc=-ESTALE;
 else if(!(rc=idle()))product_boot_claim=token;
 leave();return rc;
}
int k7_raw_vad_owner_product_startup_release(uint64_t token){
 if(!token)return -EINVAL;
 int rc=enter();if(rc)return rc;
 if(product_boot_claim!=token)rc=-ESTALE;
 else if(voice_owner||atomic_load(&product_policy_token))rc=-EBUSY;
 else {product_boot_claim=0;diagnostic_armed=false;atomic_store(&diagnostic_claimed_owner,0);memset(&diagnostic_binding,0,sizeof(diagnostic_binding));}
 leave();return rc;
}
int k7_raw_vad_owner_product_policy_register(const void *token){
 if(!token)return -EINVAL;
 int rc=enter();if(rc)return rc;
 if(atomic_load_explicit(&product_policy_token,memory_order_acquire))rc=-EBUSY;
 else if(phase!=K7_RAW_OWNER_READY||!product_boot_claim||product_boot_claim!=product_arm_serial||!diagnostic_armed||!diagnostic_binding.product_experiment||voice_owner||atomic_load(&diagnostic_claimed_owner))rc=-EACCES;
 else if(!(rc=idle()))atomic_store_explicit(&product_policy_token,(uintptr_t)token,memory_order_release);
 leave();return rc;
}
void k7_raw_vad_owner_product_policy_unregister(const void *token){
 if(!token)return;
 uintptr_t expected=(uintptr_t)token;
 (void)atomic_compare_exchange_strong_explicit(&product_policy_token,&expected,0,memory_order_acq_rel,memory_order_acquire);
}
#endif
int k7_raw_vad_owner_voice_begin(uint64_t owner){
 if(!owner)return -EINVAL;
 int rc=enter();if(rc)return rc;
 if(voice_owner||(phase!=K7_RAW_OWNER_EMPTY&&phase!=K7_RAW_OWNER_READY))rc=-EBUSY;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 /* A boot claim excludes unrelated voice startup before policy registration. */
 else if(product_boot_claim&&!atomic_load_explicit(&product_policy_token,memory_order_acquire))rc=-EACCES;
 /* Recheck at actual voice admission: a prepare-time query alone races with
  * disarm or a failed prior run that consumed its one-shot arm. */
 else if(atomic_load_explicit(&product_policy_token,memory_order_acquire)&&
         (!diagnostic_armed||!diagnostic_binding.product_experiment))rc=-EACCES;
#endif
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 else {voice_owner=owner;if(diagnostic_armed){diagnostic_armed=false;atomic_store_explicit(&diagnostic_claimed_owner,owner,memory_order_release);}}
#else
 else voice_owner=owner;
#endif
#line 83
 leave();return rc;
}
int k7_raw_vad_owner_voice_end(uint64_t owner){
 int rc=enter();if(rc)return rc;
 if(!owner||owner!=voice_owner)rc=-ESTALE;
 else if(phase==K7_RAW_OWNER_BORROWED||borrow_slot||borrow_ticket)rc=-EBUSY;
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 else {voice_owner=0;atomic_store(&diagnostic_claimed_owner,0);memset(&diagnostic_binding,0,sizeof(diagnostic_binding));}
#else
 else voice_owner=0;
#endif
#line 90
 leave();return rc;
}
int k7_raw_vad_owner_probe_begin(void){
 int rc=enter();if(rc)return rc;
 if(phase!=K7_RAW_OWNER_EMPTY||voice_owner)rc=-EBUSY;
 else if(!(rc=idle()))phase=K7_RAW_OWNER_PROBE;
 leave();return rc;
}
int k7_raw_vad_owner_probe_end(void){
 int rc=enter();if(rc)return rc;
 if(phase!=K7_RAW_OWNER_PROBE)rc=-ESTALE;else phase=K7_RAW_OWNER_EMPTY;
 leave();return rc;
}
/* Sole diagnostic worker writes scalar atomics; query is an approximate
 * per-field snapshot. Never lock, allocate, print, or call a manager here. */
#if !defined(K7_ECHO_RESIDUAL_EVIDENCE) || !K7_ECHO_RESIDUAL_EVIDENCE
static void observe(void *context,const struct k7_raw_vad_report *r){
 (void)context;
 if(!r||r->owner!=atomic_load(&borrow_owner)||r->raw!=1||r->clean||r->qualified||
    r->reply_binding_verified||!isfinite(r->model.probability)||
    r->model.probability<0||r->model.probability>1){atomic_fetch_add(&invalid_reports,1);return;}
 unsigned bits;memcpy(&bits,&r->model.probability,sizeof(bits));
 /* Nonnegative finite IEEE binary32 probabilities have ordered bit patterns. */
 if(bits>atomic_load(&peak_bits))atomic_store(&peak_bits,bits);
 atomic_store(&last_window_end,r->model.input_end_sample);
 atomic_fetch_add_explicit(&windows,1,memory_order_release);
}
#endif
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
int k7_raw_vad_owner_preopen_residual(uint64_t owner,const struct k7_apm21_live_binding *binding,
 struct k7_raw_vad_loan *ticket,struct k7_echo_runtime **out)
{
 if(!owner||!ticket||!out||*out||ticket->owner||ticket->generation)return -EINVAL;
 /* UNKNOWN is not an error in product startup and takes no shared ownership. */
 if(!k7_apm21_live_binding_valid(binding))return K7_APM21_STREAM_UNVERIFIED;
 int rc=enter();if(rc)return rc;
 if(owner!=voice_owner)rc=-ESTALE;
 else if(phase!=K7_RAW_OWNER_READY)rc=-EBUSY;
 else if(loan_sequence==UINT64_MAX)rc=-EOVERFLOW;
 else{
  *ticket=(struct k7_raw_vad_loan){owner,++loan_sequence};
  borrow_ticket=ticket;borrow_slot=out;atomic_store(&borrow_owner,owner);
  atomic_store(&windows,0);atomic_store(&last_window_end,0);
  atomic_store(&invalid_reports,0);atomic_store(&peak_bits,0);phase=K7_RAW_OWNER_BORROWED;
  rc=k7_echo_runtime_preopen_residual(owner,pipeline,binding,out);
  if(!rc&&!*out)rc=-EPROTO;
  if(rc&&!*out){memset(ticket,0,sizeof(*ticket));borrow_ticket=NULL;borrow_slot=NULL;
   atomic_store(&borrow_owner,0);phase=K7_RAW_OWNER_READY;}
 }
 last_error=rc;leave();return rc;
}
#endif
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
static int diagnostic_hash(const char *p,unsigned char out[32]){
 if(!p||strlen(p)!=64)return -EINVAL;
 unsigned nonzero=0;
 for(unsigned i=0;i<32;i++){
  unsigned v=0;
  for(unsigned j=0;j<2;j++){unsigned char c=(unsigned char)p[2*i+j];
   unsigned n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;
   if(n>15)return -EINVAL;v=(v<<4)|n;
  }
  out[i]=(unsigned char)v;nonzero|=v;
 }
 return nonzero?0:-EINVAL;
}
/* State-only explicit command. No audio/model creation, no actions. The two
 * digests identify reviewed evidence; they do NOT verify or grant calibration. */
static int diagnostic_arm(int argc,char **argv){
 if(argc!=5||!argv[2]||!argv[2][0]||strlen(argv[2])>3||
    (argv[2][0]=='0'&&argv[2][1]))return -EINVAL;
 unsigned delay=0;for(const char *p=argv[2];*p;p++){if(*p<'0'||*p>'9')return -EINVAL;delay=delay*10+(unsigned)(*p-'0');}
 if(delay>500)return -EINVAL;
 struct k7_apm21_live_binding b={.profile={.kind=K7_APM21_STREAM_PROFILE_TARGET_D128,
  .measured_algorithmic_delay_samples=128},.timing={.valid=1,.supplied_buffer_delay_ms=(int32_t)delay,
  .source=K7_APM21_DELAY_OFFLINE_HYPOTHESIS},.diagnostic_hypothesis=1};
 int rc=diagnostic_hash(argv[3],b.profile.target_receipt_sha256);
 if(!rc)rc=diagnostic_hash(argv[4],b.source_mapping_receipt_sha256);if(rc)return rc;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(!strcmp(argv[1],"vad-raw-apm21-product-arm")){
  struct k7_apm21_nominal_identity e;k7_apm21_nominal_identity_expected(&e);
  if(delay||memcmp(b.profile.target_receipt_sha256,e.transform_sha256,32)||memcmp(b.source_mapping_receipt_sha256,e.mapping_sha256,32))return -EACCES;
  b.product_experiment=1;
 }
#endif
 rc=enter();if(rc)return rc;
 #if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(b.product_experiment&&product_arm_serial==UINT64_MAX)rc=-EOVERFLOW;
 else if(product_boot_claim||(phase!=K7_RAW_OWNER_READY&&!(b.product_experiment&&phase==K7_RAW_OWNER_EMPTY))||diagnostic_armed||atomic_load(&diagnostic_claimed_owner))rc=-EBUSY;
#else
 if(phase!=K7_RAW_OWNER_READY||diagnostic_armed||atomic_load(&diagnostic_claimed_owner))rc=-EBUSY;
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 else if(!(rc=idle())){diagnostic_binding=b;diagnostic_armed=true;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(b.product_experiment)product_arm_serial++;
#endif
 }
#else
 else if(!(rc=idle())){diagnostic_binding=b;diagnostic_armed=true;}
#endif
 last_error=rc;leave();
 printf("APM21_EVIDENCE_ARM result=%d diagnostic=1 delay_ms=%u delay_source=%u qualified=0 actions=0\n",rc,delay,K7_APM21_DELAY_OFFLINE_HYPOTHESIS);
 return rc;
}
#if defined(K7_APM21_STANDALONE_STARTUP) && K7_APM21_STANDALONE_STARTUP
/* Explicit standalone product build: install exactly the reviewed manual-arm
 * binding. This does not claim the token, prepare a model or start audio. */
int k7_raw_vad_owner_product_startup_arm(void){
 struct k7_apm21_nominal_identity e;k7_apm21_nominal_identity_expected(&e);
 static const char hex[]="0123456789abcdef";
 char transform[65],mapping[65];
 for(unsigned i=0;i<32;i++){
  transform[2*i]=hex[e.transform_sha256[i]>>4];transform[2*i+1]=hex[e.transform_sha256[i]&15];
  mapping[2*i]=hex[e.mapping_sha256[i]>>4];mapping[2*i+1]=hex[e.mapping_sha256[i]&15];
 }
 transform[64]=0;mapping[64]=0;
 char *args[]={"k7sound","vad-raw-apm21-product-arm","0",transform,mapping};
 return diagnostic_arm(5,args);
}
#endif
static int diagnostic_disarm(void){
 int rc=enter();if(rc)return rc;
 #if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(product_boot_claim)rc=-EBUSY;
 else
#endif
 if(atomic_load(&diagnostic_claimed_owner))rc=-EBUSY;
 else if(!(rc=idle())){diagnostic_armed=false;memset(&diagnostic_binding,0,sizeof(diagnostic_binding));}
 last_error=rc;leave();return rc;
}
int k7_raw_vad_owner_take_apm21_diagnostic(uint64_t owner,struct k7_apm21_live_binding *out){
 if(!owner||!out)return -EINVAL;
 uint64_t claimed=atomic_load_explicit(&diagnostic_claimed_owner,memory_order_acquire);
 if(!claimed)return K7_APM21_STREAM_UNVERIFIED;
 int rc=enter();if(rc)return rc;
 if(claimed!=owner||voice_owner!=owner||phase!=K7_RAW_OWNER_READY)rc=-ESTALE;
 else{*out=diagnostic_binding;atomic_store(&diagnostic_claimed_owner,0);memset(&diagnostic_binding,0,sizeof(diagnostic_binding));rc=0;}
 leave();return rc;
}
#endif
int k7_raw_vad_owner_open(struct k7_voice_session *s,uint64_t owner,
#line 143
#line 119
 struct k7_raw_vad_loan *ticket,struct k7_echo_runtime **out){
 if(!s||!owner||!ticket||!out||*out||ticket->owner||ticket->generation)return -EINVAL;
 int rc=enter();if(rc)return rc;
 if(owner!=voice_owner)rc=-ESTALE;
 else if(phase==K7_RAW_OWNER_EMPTY)rc=k7_echo_runtime_open(s,owner,out);
 else if(phase!=K7_RAW_OWNER_READY)rc=-EBUSY;
 else if(loan_sequence==UINT64_MAX)rc=-EOVERFLOW;
 else {
  *ticket=(struct k7_raw_vad_loan){owner,++loan_sequence};
  borrow_ticket=ticket;borrow_slot=out;atomic_store(&borrow_owner,owner);
  atomic_store(&windows,0);atomic_store(&last_window_end,0);
  atomic_store(&invalid_reports,0);atomic_store(&peak_bits,0);
  phase=K7_RAW_OWNER_BORROWED;
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
  rc=k7_echo_runtime_open_residual_vad(s,owner,pipeline,out);
#else
  rc=k7_echo_runtime_open_vad(s,owner,pipeline,observe,NULL,out);
#endif
  /* Frozen open_vad failure guarantees *out==NULL, no running borrower. If
   * a future implementation violates this, retain the complete loan. */
  if(rc&&!*out){
   memset(ticket,0,sizeof(*ticket));borrow_ticket=NULL;borrow_slot=NULL;
   atomic_store(&borrow_owner,0);phase=K7_RAW_OWNER_READY;
  }
 }
 last_error=rc;leave();return rc;
}
int k7_raw_vad_owner_close(struct k7_raw_vad_loan *ticket,struct k7_echo_runtime **out){
 if(!ticket||!out)return -EINVAL;
 int rc=enter();if(rc)return rc;
 if(ticket->owner||ticket->generation){
  if(phase!=K7_RAW_OWNER_BORROWED||ticket!=borrow_ticket||out!=borrow_slot||
     ticket->owner!=atomic_load(&borrow_owner)||ticket->generation!=loan_sequence){rc=-ESTALE;goto done;}
  /* Caller already joined sole manager tap producer. Actual runtime close
   * joins diagnostic worker and destroys observer before returning success. */
  rc=k7_echo_runtime_close(out);
  if(!rc){
   memset(ticket,0,sizeof(*ticket));borrow_ticket=NULL;borrow_slot=NULL;
   atomic_store(&borrow_owner,0);phase=K7_RAW_OWNER_READY;
  }
 }else if(phase==K7_RAW_OWNER_BORROWED)rc=-ESTALE;
 else rc=k7_echo_runtime_close(out);
done:last_error=rc;leave();return rc;
}
int k7_raw_vad_owner_status(struct k7_raw_vad_owner_status *out){
 if(!out)return -EINVAL;
 memset(out,0,sizeof(*out));int rc=enter();if(rc)return rc;
 *out=(struct k7_raw_vad_owner_status){.phase=phase,.last_error=last_error,
 .model_generation=model_generation,.loan_generation=loan_sequence,
 .voice_owner=voice_owner,.borrow_owner=atomic_load(&borrow_owner),
 .windows=atomic_load_explicit(&windows,memory_order_acquire),
 .last_window_end=atomic_load(&last_window_end),.invalid_reports=atomic_load(&invalid_reports)};
 unsigned bits=atomic_load(&peak_bits);memcpy(&out->max_probability,&bits,sizeof(bits));
 leave();return 0;
}
int k7_raw_vad_owner_command(int argc,char **argv){
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(argc>=2&&!strcmp(argv[1],"vad-raw-apm21-product-arm"))return diagnostic_arm(argc,argv)?1:0;
#endif
 if(argc>=2&&!strcmp(argv[1],"vad-raw-apm21-arm"))return diagnostic_arm(argc,argv)?1:0;
 if(argc==2&&!strcmp(argv[1],"vad-raw-apm21-disarm"))return diagnostic_disarm()?1:0;
#endif
 if(argc!=2)return 1;
 int rc;
#line 177
 if(!strcmp(argv[1],"vad-raw-prepare"))rc=k7_raw_vad_owner_prepare();
 else if(!strcmp(argv[1],"vad-raw-clear"))rc=k7_raw_vad_owner_clear();
 else if(!strcmp(argv[1],"vad-raw-status"))rc=0;
 else return 1;
 struct k7_raw_vad_owner_status s;int status=k7_raw_vad_owner_status(&s);
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
 if(status)printf("RAW_VAD_OWNER result=%d status=%d actions=0\n",rc,status);
 else printf("RAW_VAD_OWNER result=%d phase=%u model_generation=%" PRIu64 " loan_generation=%" PRIu64 " voice=%" PRIu64 " borrowed=%" PRIu64 " windows=%" PRIu64 " last_end=%" PRIu64 " max_probability=%.8f invalid=%" PRIu64 " raw=%u residual=%u clean=0 qualified=0 reply_binding_verified=0 actions=0 target_budget_verified=0\n",
 rc,s.phase,s.model_generation,s.loan_generation,s.voice_owner,s.borrow_owner,s.windows,s.last_window_end,(double)s.max_probability,s.invalid_reports,
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
 0u,1u
#else
 1u,0u
#endif
 );
#else
 if(status)printf("RAW_VAD_OWNER result=%d status=%d actions=0\n",rc,status);
 else printf("RAW_VAD_OWNER result=%d phase=%u model_generation=%" PRIu64 " loan_generation=%" PRIu64 " voice=%" PRIu64 " borrowed=%" PRIu64 " windows=%" PRIu64 " last_end=%" PRIu64 " max_probability=%.8f invalid=%" PRIu64 " raw=1 clean=0 qualified=0 reply_binding_verified=0 actions=0 target_budget_verified=0\n",
 rc,s.phase,s.model_generation,s.loan_generation,s.voice_owner,s.borrow_owner,s.windows,s.last_window_end,(double)s.max_probability,s.invalid_reports);
#endif
 return rc||status?1:0;
}
