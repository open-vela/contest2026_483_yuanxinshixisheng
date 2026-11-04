#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
#include "k7_apm21_evidence_bridge.h"
#include <errno.h>
#include <string.h>
static int same_id(const struct ne_identity *a,const struct ne_identity *b){
#define E(x) a->x==b->x
 return E(owner)&&E(reference_instance)&&E(reference_epoch)&&E(clock_epoch)&&
 E(dialogue_instance)&&E(scope)&&E(lease)&&E(reply)&&E(mapping_revision)&&E(input_epoch)&&E(tx_generation);
#undef E
}
static int same_ctx(const struct vv_confirmed_wake_context *a,const struct vv_confirmed_wake_context *b){
#define E(x) a->x==b->x
 return E(owner_epoch)&&E(model_epoch)&&E(clean_epoch)&&E(alignment_epoch)&&E(scope_incarnation)&&
 E(lease_serial)&&E(reply_id)&&E(input_epoch)&&E(mode_epoch)&&E(asr_epoch)&&E(tx_generation);
#undef E
}
static int context_valid(const struct ne_identity *id,const struct vv_confirmed_wake_context *c){
 return id&&c&&id->owner&&id->reference_instance&&id->reference_epoch&&id->clock_epoch&&
 id->dialogue_instance&&id->scope&&id->lease&&id->reply&&id->mapping_revision&&id->input_epoch&&id->tx_generation&&
 c->owner_epoch==id->owner&&c->input_epoch==id->input_epoch&&c->scope_incarnation==id->scope&&
 c->lease_serial==id->lease&&c->reply_id==id->reply&&c->tx_generation==id->tx_generation&&c->asr_epoch;
}
void k7_apm21_evidence_invalidate(struct k7_apm21_evidence_bridge *b,int reason){
 if(!b||!b->initialized)return;
 b->failure=reason<0?reason:-EPIPE;b->packet_valid=0;b->have_first=0;
 b->ledger_count=b->half_count=0;ne_invalidate(b->near);
}
static int fail(struct k7_apm21_evidence_bridge *b,int rc){
 b->rejected++;k7_apm21_evidence_invalidate(b,rc);return rc;
}
int k7_apm21_evidence_init(struct k7_apm21_evidence_bridge *b,uint64_t owner,uint64_t epoch,struct ne_worker *near){
 if(!b||!owner||!epoch||!near||b->initialized)return -EINVAL;
 b->owner=owner;b->dsp_epoch=epoch;b->near=near;b->initialized=1;return 0;
}
int k7_apm21_evidence_register(struct k7_apm21_evidence_bridge *b,
 const struct ne_identity *id,const struct vv_confirmed_wake_context *ctx,
 uint64_t mic,uint64_t ref,const int16_t raw[320],const int16_t reference[320],unsigned cancel,unsigned modeled){
 if(!b||!b->initialized||!raw||!reference||cancel>1||modeled>320)return -EINVAL;
 if(b->failure)return b->failure;
 if(!context_valid(id,ctx)||id->owner!=b->owner)return fail(b,-ESTALE);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(b->poststop_enabled&&ref<=UINT64_MAX-320&&ref+320>b->poststop_cut){
  unsigned expected=ref>=b->poststop_cut?320:(unsigned)(ref+320-b->poststop_cut);
  if(modeled!=expected)return fail(b,-EPROTO);
 }
 if(modeled){
  if(!b->poststop_enabled){b->unsupported++;return -ENODATA;}
  if(!cancel||ref>UINT64_MAX-320||ref+320<=b->poststop_cut)return fail(b,-EPROTO);
  unsigned expected=ref>=b->poststop_cut?320:(unsigned)(ref+320-b->poststop_cut);
  if(modeled!=expected)return fail(b,-EPROTO);
  for(unsigned i=320-modeled;i<320;i++)if(reference[i])return fail(b,-EILSEQ);
 }
#else
 if(modeled){b->unsupported++;return -ENODATA;} /* Not genuine source support. */
#endif
 if(mic>UINT64_MAX-320||ref>UINT64_MAX-320)return fail(b,-EOVERFLOW);
 if(b->ledger_count==K7_APM21_EVIDENCE_LEDGER)return fail(b,-ENOSPC);
 if(b->ledger_count){
  const struct k7_apm21_evidence_ledger *p=&b->ledger[(b->ledger_head+b->ledger_count-1)%K7_APM21_EVIDENCE_LEDGER];
  if(mic!=p->mic_first+320||ref!=p->reference_first+320||!same_id(id,&p->identity)||!same_ctx(ctx,&p->context))return fail(b,-EPIPE);
 }else if(b->have_identity&&(!same_id(id,&b->identity)||!same_ctx(ctx,&b->context)))return fail(b,-ESTALE);
 struct k7_apm21_evidence_ledger *r=&b->ledger[(b->ledger_head+b->ledger_count)%K7_APM21_EVIDENCE_LEDGER];
 r->identity=*id;r->context=*ctx;r->mic_first=mic;r->reference_first=ref;r->cancel_observed=cancel;
 memcpy(r->mic,raw,sizeof(r->mic));memcpy(r->reference,reference,sizeof(r->reference));b->ledger_count++;
 b->source_mic_next=mic+320;b->source_ref_next=ref+320;b->source_registered=1;return 0;
}
int k7_apm21_evidence_accept(void *arg,const struct k7_apm21_stream_output *y,
 const struct k7_apm21_live_block *mic,const struct k7_apm21_live_block *ref){
 struct k7_apm21_evidence_bridge *b=arg;
 if(!b||!b->initialized||!y||!mic||!ref)return -EINVAL;
 if(b->failure)return b->failure;
 if(y->owner!=b->owner||y->dsp_epoch!=b->dsp_epoch||y->samples!=160||y->qualified||
 y->algorithmic_delay_samples!=160||y->extra_fifo_samples!=32||
 mic->first!=y->nominal_mic_first||ref->first!=y->nominal_reference_first||
 mic->first>UINT64_MAX-320||ref->first>UINT64_MAX-320||
 y->available_after_mic_next!=mic->first+320||y->available_after_reference_next!=ref->first+320)return fail(b,-EPROTO);
 if(b->have_watermark&&(mic->first<b->last_mic_next||ref->first<b->last_ref_next)){b->rejected++;return -ESTALE;}
 if(b->half_count==K7_APM21_EVIDENCE_HALVES)return fail(b,-ENOSPC);
 struct k7_apm21_evidence_half *h=&b->halves[(b->half_head+b->half_count)%K7_APM21_EVIDENCE_HALVES];
 h->y=*y;h->mic=*mic;h->reference=*ref;b->half_count++;return 0;
}
static void pop_half(struct k7_apm21_evidence_bridge *b){b->half_head=(b->half_head+1)%K7_APM21_EVIDENCE_HALVES;b->half_count--;}
int k7_apm21_evidence_pump(struct k7_apm21_evidence_bridge *b){
 if(!b||!b->initialized)return -EINVAL;if(b->failure)return b->failure;int fed=0;
 while(b->half_count&&b->ledger_count){
  struct k7_apm21_evidence_half *h=&b->halves[b->half_head];
  struct k7_apm21_evidence_ledger *r=&b->ledger[b->ledger_head];
  if(h->mic.first<r->mic_first){b->rejected++;pop_half(b);continue;} /* retired OLD half, never relabel */
  if(h->mic.first>=r->mic_first+320){b->ledger_head=(b->ledger_head+1)%K7_APM21_EVIDENCE_LEDGER;b->ledger_count--;continue;}
  unsigned offset=(unsigned)(h->mic.first-r->mic_first);
  if((offset!=0&&offset!=160)||h->reference.first!=r->reference_first+offset||
   memcmp(h->mic.pcm,r->mic+offset,sizeof(h->mic.pcm))||
   memcmp(h->reference.pcm,r->reference+offset,sizeof(h->reference.pcm)))return fail(b,-EILSEQ);
  /* The authoritative OLD identity is copied from this exact source range.
   * An observed current mailbox or y.reply_lease cannot grant authority. */
  if(!b->have_identity){
   int rc=ne_begin(b->near,&r->identity,h->mic.first);if(rc)return fail(b,rc);
   b->identity=r->identity;b->context=r->context;b->have_identity=1;
  }else if(!same_id(&b->identity,&r->identity)||!same_ctx(&b->context,&r->context))return fail(b,-ESTALE);
  if(!b->have_first){b->first=*h;b->have_first=1;}
  else{
   if(h->mic.first!=b->first.mic.first+160||h->reference.first!=b->first.reference.first+160)return fail(b,-EPIPE);
   b->raw=(struct k7_voice_frame){.first_sample=b->first.mic.first,.epoch=b->identity.input_epoch,.samples=320};
   b->residual=b->raw;
   memcpy(b->raw.pcm,b->first.mic.pcm,320);memcpy(b->raw.pcm+160,h->mic.pcm,320);
   memcpy(b->reference,b->first.reference.pcm,320);memcpy(b->reference+160,h->reference.pcm,320);
   memcpy(b->residual.pcm,b->first.y.pcm,320);memcpy(b->residual.pcm+160,h->y.pcm,320);
   unsigned zeros=0;
   for(unsigned i=0;i<320;i++){
    const struct k7_apm21_live_block *q=i<160?&b->first.reference:&h->reference;unsigned k=i%160;
    b->zero[i]=(uint8_t)((q->explicit_zero_bits[k/8]>>(k%8))&1u);zeros+=b->zero[i];
    if(b->zero[i]&&b->reference[i])return fail(b,-EPROTO);
   }
   int nr=ne_feed(b->near,&b->identity,&b->identity,b->raw.first_sample,b->raw.pcm,b->reference,b->residual.pcm,&b->report);
   if(nr<0)return fail(b,nr);b->frames++;if(nr){b->windows++;if(b->report.candidate)b->candidates++;}
   b->packet=(struct k7_echo_packet){.identity=b->identity,.raw=b->raw,.residual=b->residual,
    .reference=b->reference,.reference_first=b->first.reference.first,.report=nr?&b->report:NULL,
    .cancel_observed=r->cancel_observed!=0,.source_stop_zero_samples=zeros,.applied_lag_samples=0};
   b->packet_context=b->context;b->packet_valid=1;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
   if(b->packet_sink){int sink=b->packet_sink(b->packet_sink_arg,&b->packet,&b->packet_context);if(sink<0)return fail(b,sink);}
#endif
   b->last_mic_next=h->mic.first+160;b->last_ref_next=h->reference.first+160;b->have_watermark=1;
   b->have_first=0;fed++;
  }
  if(h->mic.first+160==r->mic_first+320){b->ledger_head=(b->ledger_head+1)%K7_APM21_EVIDENCE_LEDGER;b->ledger_count--;}
  pop_half(b);
 }
 return fed;
}
int k7_apm21_evidence_rearm(struct k7_apm21_evidence_bridge *b,uint64_t owner){
 if(!b||!b->initialized||owner!=b->owner)return -ESTALE;
 ne_invalidate(b->near);b->failure=0;b->half_head=b->half_count=b->ledger_head=b->ledger_count=0;
 b->have_first=b->have_identity=b->packet_valid=b->source_registered=b->cancel_noted=0;return 0;
}
int k7_apm21_evidence_note_cancel(struct k7_apm21_evidence_bridge *b,
 const struct ne_identity *id,const struct vv_confirmed_wake_context *ctx,uint64_t mic,uint64_t ref){
 if(!b||!b->initialized||!id||!ctx)return -EINVAL;
 if(b->failure)return b->failure;if(!b->source_registered)return -EAGAIN;
 if(mic!=b->source_mic_next||ref!=b->source_ref_next)return fail(b,-EPIPE);
 const struct ne_identity *old=b->have_identity?&b->identity:&b->ledger[b->ledger_head].identity;
 const struct vv_confirmed_wake_context *c=b->have_identity?&b->context:&b->ledger[b->ledger_head].context;
 if(!same_id(id,old)||!same_ctx(ctx,c))return fail(b,-ESTALE);
 b->cancel_noted=1;return 0;
}
#endif
