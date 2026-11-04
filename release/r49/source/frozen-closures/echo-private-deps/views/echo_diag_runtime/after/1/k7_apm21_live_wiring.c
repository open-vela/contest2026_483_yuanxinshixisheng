#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#include "k7_apm21_live_wiring.h"
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
#include "k7_apm21_reference_stage.h"
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include "k7_apm21_nominal_identity.h"
#endif
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
enum { TAP_IDLE=0,TAP_PREFILL,TAP_ON,TAP_FAILED,TAP_DRAIN };
#else
enum { TAP_IDLE=0,TAP_PREFILL,TAP_ON,TAP_FAILED };
#endif
enum { REC_BEGIN=1,REC_MIC,REC_REFERENCE };
#define TAP_SLOTS 64u
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"PIO tap requires lock-free unsigned atomics");
struct tap_record {
 uint64_t epoch,first,other_first,before_us,after_us;
 unsigned revision,kind;
 int16_t pcm[160];
 uint8_t explicit_zero_bits[20];
};
struct k7_apm21_pio_tap {
 atomic_uint produced,consumed,revision;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 atomic_uint_fast64_t pinned_owner;
 atomic_uint cut_ready;
 struct k7_apm21_cut_descriptor terminal_cut;
 uint64_t reference_instance;
 uint32_t reference_epoch;
 unsigned cut_taken;
#endif

#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
 struct k7_apm21_reference_stage reference_stage;
#endif
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 atomic_uint physical_fault;
#endif
#line 21
 struct tap_record slots[TAP_SLOTS];
 /* Everything below is accessed by the one PIO producer only. */
 uint64_t epoch,mic_next,ref_next,mic_first,ref_first,last_us;
 unsigned phase,mic_samples,ref_samples,exhausted;
 struct tap_record mic,ref;
};
static unsigned tap_revision(const struct k7_apm21_pio_tap *t)
{return atomic_load_explicit(&t->revision,memory_order_acquire);}
static int advance_revision(struct k7_apm21_pio_tap *t)
{
 unsigned n=atomic_load_explicit(&t->revision,memory_order_relaxed);
 if(n>=UINT_MAX-1){t->exhausted=1;t->phase=TAP_FAILED;
  atomic_store_explicit(&t->revision,UINT_MAX,memory_order_release);return -EOVERFLOW;}
 atomic_store_explicit(&t->revision,n+1,memory_order_release);return 0;
}
void k7_apm21_tap_gap(struct k7_apm21_pio_tap *t,int reason)
{
 (void)reason; /* Fault is an invalidation, never an audio pump error. */
 if(!t || t->phase==TAP_IDLE || t->phase==TAP_FAILED)return;
 t->phase=TAP_FAILED;t->mic_samples=t->ref_samples=0;(void)advance_revision(t);
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 atomic_store_explicit(&t->physical_fault,1,memory_order_release);
#endif
#line 41
}
static int publish(struct k7_apm21_pio_tap *t,struct tap_record *r)
{
 unsigned p=atomic_load_explicit(&t->produced,memory_order_relaxed);
 unsigned c=atomic_load_explicit(&t->consumed,memory_order_acquire);
 if(p-c>=TAP_SLOTS){k7_apm21_tap_gap(t,-ENOSPC);return -ENOSPC;}
 r->revision=tap_revision(t);r->epoch=t->epoch;
 t->slots[p%TAP_SLOTS]=*r;
 atomic_store_explicit(&t->produced,p+1,memory_order_release);return 0;
}
static int take(struct k7_apm21_pio_tap *t,struct tap_record *r)
{
 unsigned c=atomic_load_explicit(&t->consumed,memory_order_relaxed);
 unsigned p=atomic_load_explicit(&t->produced,memory_order_acquire);
 if(c==p)return 0;
 *r=t->slots[c%TAP_SLOTS];
 atomic_store_explicit(&t->consumed,c+1,memory_order_release);return 1;
}
int k7_apm21_tap_create(struct k7_apm21_pio_tap **out)
{
 if(!out)return -EINVAL;
 *out=NULL;
 struct k7_apm21_pio_tap *t=calloc(1,sizeof(*t));if(!t)return -ENOMEM;
 #if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
 k7_apm21_reference_init(&t->reference_stage);
#endif
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 atomic_init(&t->pinned_owner,0);atomic_init(&t->cut_ready,0);
#endif
 atomic_init(&t->produced,0);atomic_init(&t->consumed,0);atomic_init(&t->revision,1);
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 atomic_init(&t->physical_fault,0);
#endif
#line 65
 *out=t;return 0;
}
void k7_apm21_tap_destroy(struct k7_apm21_pio_tap *t){free(t);}
size_t k7_apm21_tap_storage_bytes(void){return sizeof(struct k7_apm21_pio_tap);}
void k7_apm21_tap_prepare(struct k7_apm21_pio_tap *t,uint64_t mic)
{
 if(!t || t->exhausted)return;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 if(t->phase==TAP_DRAIN){k7_apm21_tap_gap(t,-EBUSY);return;}
#endif
 if(t->phase==TAP_ON || t->phase==TAP_PREFILL){k7_apm21_tap_gap(t,-EBUSY);return;}
 if(t->epoch==UINT64_MAX || mic>UINT64_MAX-160 || t->ref_next>UINT64_MAX-160){
  t->exhausted=1;(void)advance_revision(t);t->phase=TAP_FAILED;return;
 }
 if(advance_revision(t))return;
 ++t->epoch;t->phase=TAP_PREFILL;t->mic_next=t->mic_first=mic;
 t->ref_first=t->ref_next;t->last_us=0;t->mic_samples=t->ref_samples=0;
 memset(&t->mic,0,sizeof(t->mic));memset(&t->ref,0,sizeof(t->ref));
}
void k7_apm21_tap_on(struct k7_apm21_pio_tap *t,uint64_t mic,uint64_t before,uint64_t after)
{
 if(!t)return;
 if(t->phase!=TAP_PREFILL || mic!=t->mic_first || !t->ref_samples ||
    t->ref_samples>8 || after<before || before<t->last_us){
  k7_apm21_tap_gap(t,-EPIPE);return;
 }
 t->last_us=after;t->phase=TAP_ON;
 struct tap_record begin={.kind=REC_BEGIN,.first=mic,.other_first=t->ref_first,
  .before_us=before,.after_us=after};(void)publish(t,&begin);
}
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
void k7_apm21_tap_stop_pending(struct k7_apm21_pio_tap *t,int rc){
 if(!t)return;
 if((rc&&rc!=-ECANCELED)||t->phase!=TAP_ON){k7_apm21_tap_gap(t,rc?rc:-EPIPE);return;}
 t->phase=TAP_DRAIN;
}
int k7_apm21_tap_pin_owner(struct k7_apm21_pio_tap *t,uint64_t owner){
 if(!t||!owner)return -EINVAL;uint_fast64_t zero=0;
 if(!atomic_compare_exchange_strong_explicit(&t->pinned_owner,&zero,owner,memory_order_release,memory_order_relaxed)&&zero!=owner)return -ESTALE;return 0;
}
void k7_apm21_tap_reference_identity(struct k7_apm21_pio_tap *t,uint64_t instance,uint32_t epoch,int rc){
 if(!t)return;
 if(rc||!instance||!epoch||!atomic_load_explicit(&t->pinned_owner,memory_order_acquire)||t->reference_instance){
  (void)k7_apm21_reference_fault(&t->reference_stage,-ESTALE);k7_apm21_tap_gap(t,-ESTALE);return;
 }
 t->reference_instance=instance;t->reference_epoch=epoch;
}
int k7_apm21_tap_take_cut(struct k7_apm21_pio_tap *t,uint64_t owner,struct k7_apm21_cut_descriptor *out){
 if(!t||!out||!owner||atomic_load_explicit(&t->pinned_owner,memory_order_acquire)!=owner)return -ESTALE;
 if(!atomic_load_explicit(&t->cut_ready,memory_order_acquire)||t->cut_taken)return 0;
 *out=t->terminal_cut;t->cut_taken=1;return 1;
}
#endif
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
void k7_apm21_tap_reference_begin(struct k7_apm21_pio_tap *t,uint32_t gen,uint32_t input,uint32_t clock,uint64_t now){
 if(!t)return;
 int rc=k7_apm21_reference_begin(&t->reference_stage,t->epoch,gen,input,clock,t->ref_next,now);
 if(rc)k7_apm21_tap_gap(t,rc);
}
void k7_apm21_tap_reference_observe(struct k7_apm21_pio_tap *t,uint32_t gen,uint32_t input,uint32_t clock,unsigned words,int observed,uint64_t now,unsigned final){
 if(!t||!t->reference_stage.active||atomic_load_explicit(&t->reference_stage.sealed,memory_order_relaxed))return;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 int rc=k7_apm21_reference_observe(&t->reference_stage,t->epoch,gen,input,clock,words,observed,now);
 if(rc){k7_apm21_tap_gap(t,rc);return;}
 if(t->phase==TAP_ON||t->phase==TAP_DRAIN){
  struct k7_apm21_reference_block block;
  while((rc=k7_apm21_reference_take(&t->reference_stage,&block,now))>0){
   struct tap_record r={.kind=REC_REFERENCE,.first=block.first,.before_us=block.before_us,.after_us=block.after_us};
   memcpy(r.pcm,block.pcm,sizeof(r.pcm));memcpy(r.explicit_zero_bits,block.zero_bits,sizeof(r.explicit_zero_bits));
   if(publish(t,&r))return;
  }
  if(rc<0){k7_apm21_tap_gap(t,rc);return;}
 }
 if(final){rc=k7_apm21_reference_cut(&t->reference_stage,t->epoch,gen,input,clock,words,observed,now);if(rc){k7_apm21_tap_gap(t,rc);return;}}
#else
 int rc=final?k7_apm21_reference_cut(&t->reference_stage,t->epoch,gen,input,clock,words,observed,now):
  k7_apm21_reference_observe(&t->reference_stage,t->epoch,gen,input,clock,words,observed,now);
 if(rc){k7_apm21_tap_gap(t,rc);return;}
#endif
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 if(final){
  if(!t->reference_instance||!t->reference_epoch||atomic_load_explicit(&t->cut_ready,memory_order_relaxed)){
   (void)k7_apm21_reference_fault(&t->reference_stage,-ESTALE);k7_apm21_tap_gap(t,-ESTALE);return;
  }
  t->terminal_cut=(struct k7_apm21_cut_descriptor){.serial=t->epoch,
   .owner=atomic_load_explicit(&t->pinned_owner,memory_order_acquire),.tap_epoch=t->epoch,
   .reference_instance=t->reference_instance,.reference_epoch=t->reference_epoch,
   .first=t->reference_stage.first,.written=t->reference_stage.written,
   .cut=atomic_load_explicit(&t->reference_stage.final_cut,memory_order_relaxed),
   .published_upper=atomic_load_explicit(&t->reference_stage.published_next,memory_order_relaxed),
   .before_us=now,.after_us=now,.generation=gen,.input_epoch=input,.clock_epoch=clock,.remaining_words=words};
  struct k7_apm21_reference_block prefix;unsigned count;
  rc=k7_apm21_reference_prefix(&t->reference_stage,&prefix,&count);
  if(rc){k7_apm21_tap_gap(t,rc);return;}
  t->terminal_cut.prefix_samples=count;memcpy(t->terminal_cut.prefix_pcm,prefix.pcm,sizeof(prefix.pcm));
  memcpy(t->terminal_cut.prefix_zero_bits,prefix.zero_bits,sizeof(prefix.zero_bits));
  atomic_store_explicit(&t->cut_ready,1,memory_order_release);
 }
#endif
 /* A terminal cut does not restart a GAP or fabricate a tail flush. */
 if(final||t->phase!=TAP_ON)return;
 struct k7_apm21_reference_block b;
 while((rc=k7_apm21_reference_take(&t->reference_stage,&b,now))>0){
  struct tap_record r={.kind=REC_REFERENCE,.first=b.first,.before_us=b.before_us,.after_us=b.after_us};
  memcpy(r.pcm,b.pcm,sizeof(r.pcm));memcpy(r.explicit_zero_bits,b.zero_bits,sizeof(r.explicit_zero_bits));
  if(publish(t,&r))return;
 }
 if(rc<0)k7_apm21_tap_gap(t,rc);
}
void k7_apm21_tap_reference_status(struct k7_apm21_pio_tap *t,struct k7_apm21_reference_status *out){
 if(!out)return;memset(out,0,sizeof(*out));if(!t){out->fault=-EINVAL;return;}
 out->fault=atomic_load_explicit(&t->reference_stage.fault,memory_order_acquire);
 out->sealed=atomic_load_explicit(&t->reference_stage.sealed,memory_order_acquire);
 out->published_next=atomic_load_explicit(&t->reference_stage.published_next,memory_order_acquire);
 out->final_cut=atomic_load_explicit(&t->reference_stage.final_cut,memory_order_acquire);
}
#endif
void k7_apm21_tap_pair(struct k7_apm21_pio_tap *t,int16_t sample,unsigned zero,
 uint64_t before,uint64_t after)
{
 if(!t)return;
#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
 if(t->phase!=TAP_PREFILL&&t->phase!=TAP_ON)return;
 int sr=k7_apm21_reference_pair(&t->reference_stage,sample,zero,before,after);
 if(sr){k7_apm21_tap_gap(t,sr);return;}
 t->ref_next++;t->last_us=after;if(t->phase==TAP_PREFILL)t->ref_samples++;
 return;
#else
 /* No observation is invented for OFF or failed half writes. */
 if(t->phase!=TAP_PREFILL && t->phase!=TAP_ON)return;
 if(zero>1 || (zero && sample) || after<before || before<t->last_us ||
    t->ref_next==UINT64_MAX || (t->phase==TAP_PREFILL && t->ref_samples>=8)){
  k7_apm21_tap_gap(t,-EPIPE);return;
 }
 t->last_us=after;
 if(!t->ref_samples){memset(&t->ref,0,sizeof(t->ref));t->ref.kind=REC_REFERENCE;
  t->ref.first=t->ref_next;t->ref.before_us=before;}
 unsigned n=t->ref_samples++;t->ref.pcm[n]=sample;t->ref.after_us=after;
 if(zero)t->ref.explicit_zero_bits[n/8]|=(uint8_t)(1u<<(n%8));
 ++t->ref_next;
 if(t->ref_samples==160){(void)publish(t,&t->ref);t->ref_samples=0;}
#endif
}
void k7_apm21_tap_microphone(struct k7_apm21_pio_tap *t,int16_t sample,uint64_t index)
{
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 if(!t||(t->phase!=TAP_ON&&t->phase!=TAP_DRAIN))return;
#else
 if(!t || t->phase!=TAP_ON)return;
#endif
 if(index!=t->mic_next || index==UINT64_MAX){k7_apm21_tap_gap(t,-EILSEQ);return;}
 if(!t->mic_samples){memset(&t->mic,0,sizeof(t->mic));t->mic.kind=REC_MIC;t->mic.first=index;}
 t->mic.pcm[t->mic_samples++]=sample;++t->mic_next;
 if(t->mic_samples==160){(void)publish(t,&t->mic);t->mic_samples=0;}
}
static int binding_valid(const struct k7_apm21_live_binding *b)
{
 unsigned profile=0,mapping=0;
 if(!b || b->profile.kind!=K7_APM21_STREAM_PROFILE_TARGET_D128 ||
    b->profile.measured_algorithmic_delay_samples!=128 || b->timing.valid!=1 ||
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
    (b->timing.source!=K7_APM21_DELAY_MEASURED && !(b->diagnostic_hypothesis==1 && b->timing.source==K7_APM21_DELAY_OFFLINE_HYPOTHESIS)) || b->timing.supplied_buffer_delay_ms<0 ||
#else
    b->timing.source!=K7_APM21_DELAY_MEASURED || b->timing.supplied_buffer_delay_ms<0 ||
#endif
#line 124
    b->timing.supplied_buffer_delay_ms>500)return 0;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(b->product_experiment){
  struct k7_apm21_nominal_identity e;k7_apm21_nominal_identity_expected(&e);
  if(b->product_experiment!=1||b->diagnostic_hypothesis!=1||b->timing.source!=K7_APM21_DELAY_OFFLINE_HYPOTHESIS||b->timing.supplied_buffer_delay_ms||memcmp(b->profile.target_receipt_sha256,e.transform_sha256,32)||memcmp(b->source_mapping_receipt_sha256,e.mapping_sha256,32))return 0;
 }
#endif
 for(unsigned i=0;i<32;++i){profile|=b->profile.target_receipt_sha256[i];
  mapping|=b->source_mapping_receipt_sha256[i];}
 return profile && mapping;
}
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
int k7_apm21_live_binding_valid(const struct k7_apm21_live_binding *b){return binding_valid(b);}
#endif
static void close_epoch(struct k7_apm21_live_worker *w,int reason)
#line 130
{
 if(w->stream.consumer){
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  if(w->stream.state==K7_APM21_STREAM_ACTIVE || (w->prepared_lifecycle&&w->stream.state==K7_APM21_STREAM_PREPARED))
#else
  if(w->stream.state==K7_APM21_STREAM_ACTIVE)
#endif
#line 133
   (void)k7_apm21_stream_gap(&w->stream,w->owner,w->epoch,reason);
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  if(!w->prepared_lifecycle)
#endif
  (void)k7_apm21_stream_stop(&w->stream,w->owner,w->epoch);
#line 135
 }
 w->mic_count=w->ref_count=w->mic_head=w->ref_head=0;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 w->pending_pair_valid=w->output_pair_valid=0;
#endif
#line 137
 memset(&w->last_output,0,sizeof(w->last_output));w->last_error=reason;
}
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
int k7_apm21_live_worker_prepare(struct k7_apm21_live_worker *w,uint64_t owner,
 const struct k7_apm21_live_binding *binding)
{
 if(!w||!owner||w->initialized||w->owner)return -EINVAL;
 if(!binding_valid(binding))return K7_APM21_STREAM_UNVERIFIED;
 w->owner=owner;w->epoch=owner;w->binding=*binding;w->binding_valid=1;
 w->prepared_lifecycle=1;w->initialized=1;
 int32_t upstream=0;
 int rc=k7_apm21_stream_prepare(&w->stream,owner,owner,&binding->profile,&upstream);
 w->last_error=rc;return rc;
}
int k7_apm21_live_worker_attach(struct k7_apm21_live_worker *w,struct k7_apm21_pio_tap *t)
{
 if(!w||!t||!w->initialized||!w->prepared_lifecycle||w->tap||w->paused||
    w->stream.state!=K7_APM21_STREAM_PREPARED)return -EINVAL;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 int pin=k7_apm21_tap_pin_owner(t,w->owner);if(pin)return pin;
#endif
 w->tap=t;w->revision=tap_revision(t);return 0;
}
int k7_apm21_live_worker_pause(struct k7_apm21_live_worker *w)
{
 if(!w||!w->initialized||!w->prepared_lifecycle)return -EINVAL;
 close_epoch(w,-ECANCELED);w->paused=1;return 0;
}
#endif
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
int k7_apm21_live_worker_set_output_sink(struct k7_apm21_live_worker *w,
 int (*sink)(void *,const struct k7_apm21_stream_output *,const struct k7_apm21_live_block *,const struct k7_apm21_live_block *),void *arg)
{
 if(!w||!w->initialized||!w->prepared_lifecycle||w->tap||w->origin_bound||!sink||!arg||w->output_sink)return -EINVAL;
 w->output_sink=sink;w->output_sink_arg=arg;return 0;
}
#endif
int k7_apm21_live_worker_init(struct k7_apm21_live_worker *w,struct k7_apm21_pio_tap *t,
#line 164
#line 140
 uint64_t owner,const struct k7_apm21_live_binding *binding)
{
 if(!w || !t || !owner || w->initialized || w->owner)return -EINVAL;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(binding_valid(binding))return -EPERM; /* Valid profiles require PREPARED. */
#endif
#line 143
 /* Fresh storage is supplied by the parent runtime's one initial calloc. */
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 int pin=k7_apm21_tap_pin_owner(t,owner);if(pin)return pin;
#endif
 w->tap=t;w->owner=owner;w->revision=tap_revision(t);w->initialized=1;
 w->binding_valid=(unsigned)binding_valid(binding);if(binding)w->binding=*binding;
 return 0;
}
int k7_apm21_live_worker_reply(struct k7_apm21_live_worker *w,uint64_t next)
{
 if(!w || !w->initialized)return -EINVAL;
 if(next==w->reply_lease)return 0;
 if(next && next<=w->reply_high_water)return -ESTALE;
 if(w->stream.state==K7_APM21_STREAM_ACTIVE){
  int rc=k7_apm21_stream_rebind_reply(&w->stream,w->owner,w->epoch,w->stream.reply_lease,next);
  if(rc)return rc;
 }
 w->reply_lease=next;if(next)w->reply_high_water=next;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 w->output_pair_valid=0;
#endif
#line 158
 memset(&w->last_output,0,sizeof(w->last_output));return 0;
}
static int enqueue_block(struct k7_apm21_live_worker *w,const struct tap_record *r)
{
 unsigned mic=r->kind==REC_MIC;
 struct k7_apm21_live_block *blocks=mic?w->mic:w->ref;
 unsigned *count=mic?&w->mic_count:&w->ref_count;
 unsigned head=mic?w->mic_head:w->ref_head;
 uint64_t *next=mic?&w->mic_next:&w->ref_next;
 if(*count==K7_APM21_LIVE_PENDING || r->first!=*next || r->first>UINT64_MAX-160)return -EPIPE;
 struct k7_apm21_live_block *b=&blocks[(head+*count)%K7_APM21_LIVE_PENDING];
 b->first=r->first;memcpy(b->pcm,r->pcm,sizeof(b->pcm));
 memcpy(b->explicit_zero_bits,r->explicit_zero_bits,sizeof(b->explicit_zero_bits));
 ++*count;*next+=160;return 0;
}
static int process_ready(struct k7_apm21_live_worker *w)
{
 while(w->mic_count && w->ref_count){
  struct k7_apm21_live_block *m=&w->mic[w->mic_head],*r=&w->ref[w->ref_head];
  if(w->revision!=tap_revision(w->tap))return -EPIPE;
  struct k7_apm21_stream_input in={.owner=w->owner,.dsp_epoch=w->epoch,
   .ordinal=w->stream.ordinal,.mic_first=m->first,.reference_first=r->first,
   .mic=m->pcm,.reference=r->pcm,.samples=160,.reference_complete=1,.timing=&w->binding.timing};
  int rc=k7_apm21_stream_process(&w->stream,&in,&w->last_output);
  if(w->revision!=tap_revision(w->tap))return -EPIPE;
  if(rc<0)return rc;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  if(w->prepared_lifecycle){
   w->output_pair_valid=0;
   if(rc==K7_APM21_STREAM_OUTPUT){
    if(!w->pending_pair_valid||w->last_output.nominal_mic_first!=w->pending_mic.first||
       w->last_output.nominal_reference_first!=w->pending_ref.first||
       w->last_output.reply_lease!=w->pending_half_lease)return -EPROTO;
    w->output_mic=w->pending_mic;w->output_ref=w->pending_ref;w->output_pair_valid=1;
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
    if(w->output_sink){int sink_rc=w->output_sink(w->output_sink_arg,&w->last_output,&w->output_mic,&w->output_ref);if(sink_rc)return sink_rc;}
#endif
#line 192
   }
   /* Preserve original nominal raw/ref/zero and its OLD lease for the next
    * API output. No current reply snapshot can relabel this cached half. */
   w->pending_mic=*m;w->pending_ref=*r;w->pending_half_lease=w->stream.reply_lease;
   w->pending_pair_valid=1;
  }
#endif
  ++w->accepted_frames;if(rc==1)++w->output_frames;else ++w->warmup_frames;
#line 185
  w->mic_head=(w->mic_head+1)%K7_APM21_LIVE_PENDING;--w->mic_count;
  w->ref_head=(w->ref_head+1)%K7_APM21_LIVE_PENDING;--w->ref_count;
 }
 return 0;
}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
int k7_apm21_live_worker_poststop(struct k7_apm21_live_worker *w,const struct k7_apm21_cut_descriptor *d){
 if(!w||!d||w->binding.product_experiment!=1||w->owner!=d->owner||w->tap_epoch!=d->tap_epoch||!w->pending_pair_valid||w->stream.ingestion_fault||w->stream.state!=K7_APM21_STREAM_ACTIVE||w->ref_count||w->ref_next<d->published_upper)return -ESTALE;
 unsigned budget=w->mic_count;
 for(unsigned k=0;k<budget;k++){
  struct tap_record r={.kind=REC_REFERENCE,.first=w->ref_next};
  if(r.first>UINT64_MAX-160)return -EOVERFLOW;
  for(unsigned i=0;i<160;i++){
   uint64_t at=r.first+i;
   if(at<d->cut){uint64_t n=at-d->published_upper;if(n>=d->prefix_samples)return -EPIPE;r.pcm[i]=d->prefix_pcm[n];if((d->prefix_zero_bits[n/8]>>(n%8))&1u)r.explicit_zero_bits[i/8]|=(uint8_t)(1u<<(i%8));}
   else r.explicit_zero_bits[i/8]|=(uint8_t)(1u<<(i%8));
  }
  int rc=enqueue_block(w,&r);if(!rc)rc=process_ready(w);if(rc){close_epoch(w,rc);return rc;}
 }
 return 0;
}
#endif
int k7_apm21_live_worker_drain(struct k7_apm21_live_worker *w,unsigned budget)
{
 if(!w || !w->initialized || !budget || budget>TAP_SLOTS)return -EINVAL;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(w->prepared_lifecycle&&(!w->tap||w->paused))return 0;
 if(w->prepared_lifecycle&&atomic_load_explicit(&w->tap->physical_fault,memory_order_acquire)){close_epoch(w,-EPIPE);return 0;}
#endif
 unsigned revision=tap_revision(w->tap);
#line 194
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(revision!=w->revision){if(!w->prepared_lifecycle||w->origin_bound){close_epoch(w,-EPIPE);++w->gaps;}w->revision=revision;}
#else
 if(revision!=w->revision){close_epoch(w,-EPIPE);++w->gaps;w->revision=revision;}
#endif
 for(unsigned i=0;i<budget;++i){
#line 196
  struct tap_record r;if(!take(w->tap,&r))break;
  revision=tap_revision(w->tap);
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  if(revision!=w->revision){if(!w->prepared_lifecycle||w->origin_bound){close_epoch(w,-EPIPE);++w->gaps;}w->revision=revision;}
  if(w->prepared_lifecycle&&atomic_load_explicit(&w->tap->physical_fault,memory_order_acquire)){close_epoch(w,-EPIPE);break;}
#else
  if(revision!=w->revision){close_epoch(w,-EPIPE);++w->gaps;w->revision=revision;}
#endif
#line 199
  if(r.revision!=w->revision)continue; /* Old physical epoch never gains authority again. */
  if(r.kind==REC_BEGIN){
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
   if(w->prepared_lifecycle){
    /* First authentic origin only. Per-TX tap epochs are not DSP epochs.
     * An intervening OFF/gap cannot authorize a hidden DSP restart. */
    if(w->origin_bound||r.epoch!=1||w->stream.state!=K7_APM21_STREAM_PREPARED){close_epoch(w,-EPIPE);++w->gaps;continue;}
    int rc=k7_apm21_stream_bind_origin(&w->stream,w->owner,w->epoch,r.first,r.other_first);
    if(rc){close_epoch(w,rc);continue;}
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
    w->mic_origin=r.first;w->reference_origin=r.other_first;
#endif
    w->tap_epoch=r.epoch;w->origin_bound=1;w->mic_next=r.first;w->ref_next=r.other_first;
    if(w->reply_lease)rc=k7_apm21_stream_rebind_reply(&w->stream,w->owner,w->epoch,0,w->reply_lease);
    if(rc)close_epoch(w,rc);else w->last_error=0;continue;
   }
#endif
   close_epoch(w,-EPIPE);w->epoch=r.epoch;w->mic_next=r.first;w->ref_next=r.other_first;
#line 202
   if(!w->binding_valid){++w->unverified_epochs;w->last_error=K7_APM21_STREAM_UNVERIFIED;continue;}
   int32_t upstream=0;
   int rc=k7_apm21_stream_start(&w->stream,w->owner,w->epoch,r.first,r.other_first,&w->binding.profile,&upstream);
   if(!rc && w->reply_lease)rc=k7_apm21_stream_rebind_reply(&w->stream,w->owner,w->epoch,0,w->reply_lease);
   if(rc){close_epoch(w,rc);continue;}
   w->last_error=0;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  }else if(w->stream.state==K7_APM21_STREAM_ACTIVE && r.epoch==(w->prepared_lifecycle?w->tap_epoch:w->epoch)){
#else
  }else if(w->stream.state==K7_APM21_STREAM_ACTIVE && r.epoch==w->epoch){
#endif
#line 209
   int rc=(r.kind==REC_MIC || r.kind==REC_REFERENCE)?enqueue_block(w,&r):-EPROTO;
   if(!rc)rc=process_ready(w);
   if(rc){close_epoch(w,rc);++w->gaps;}
  }
 }
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(w->revision!=tap_revision(w->tap)){if(!w->prepared_lifecycle||w->origin_bound){close_epoch(w,-EPIPE);++w->gaps;}w->revision=tap_revision(w->tap);}
 if(w->prepared_lifecycle&&atomic_load_explicit(&w->tap->physical_fault,memory_order_acquire))close_epoch(w,-EPIPE);
#else
 if(w->revision!=tap_revision(w->tap)){close_epoch(w,-EPIPE);++w->gaps;w->revision=tap_revision(w->tap);}
#endif
#line 215
 return 0; /* Observer errors never become audio/network/control failures. */
}
void k7_apm21_live_worker_close(struct k7_apm21_live_worker *w)
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
{if(w && w->initialized){close_epoch(w,-ECANCELED);
 if(w->prepared_lifecycle&&w->stream.consumer)(void)k7_apm21_stream_stop(&w->stream,w->owner,w->epoch);
 w->initialized=0;w->tap=NULL;}}
#else
{if(w && w->initialized){close_epoch(w,-ECANCELED);w->initialized=0;w->tap=NULL;}}
#endif
#line 219
#endif
