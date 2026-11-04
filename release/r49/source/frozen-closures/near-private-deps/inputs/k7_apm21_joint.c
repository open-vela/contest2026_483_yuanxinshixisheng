#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#include "k7_apm21_joint.h"
#include <errno.h>
#include <string.h>
static int fail(struct k7_apm21_joint *j,int rc){if(!j->failure)j->failure=rc<0?rc:-EPIPE;return j->failure;}
static int same(const struct ne_identity *a,const struct ne_identity *b){
#define E(x) a->x==b->x
 return E(owner)&&E(reference_instance)&&E(reference_epoch)&&E(clock_epoch)&&E(dialogue_instance)&&E(scope)&&E(lease)&&E(reply)&&E(mapping_revision)&&E(input_epoch)&&E(tx_generation);
#undef E
}
static int ctxsame(const struct vv_confirmed_wake_context *a,const struct vv_confirmed_wake_context *b){
#define E(x) a->x==b->x
 return E(owner_epoch)&&E(model_epoch)&&E(clean_epoch)&&E(alignment_epoch)&&E(scope_incarnation)&&E(lease_serial)&&E(reply_id)&&E(input_epoch)&&E(mode_epoch)&&E(asr_epoch)&&E(tx_generation);
#undef E
}
static int anchorsame(const struct k7_voice_anchor *a,const struct k7_voice_anchor *b){
#define E(x) a->x==b->x
 return E(before_us)&&E(after_us)&&E(rx_samples)&&E(generation)&&E(tx_queued_samples)&&E(rx_fifo_words)&&E(tx_fifo_words)&&E(discarded_words)&&E(epoch)&&E(event)&&E(flags)&&E(result);
#undef E
}
int k7_apm21_joint_begin(struct k7_apm21_joint *j,const struct ne_identity *id,const struct vv_confirmed_wake_context *ctx,uint64_t epoch){
 if(!j||!id||!ctx||j->begun||!epoch||!id->owner||!id->reference_instance||!id->reference_epoch||!id->clock_epoch||!id->dialogue_instance||!id->scope||!id->lease||!id->reply||!id->mapping_revision||!id->input_epoch||!id->tx_generation||ctx->owner_epoch!=id->owner||ctx->input_epoch!=id->input_epoch||ctx->tx_generation!=id->tx_generation||ctx->reply_id!=id->reply||ctx->scope_incarnation!=id->scope||ctx->lease_serial!=id->lease||!ctx->asr_epoch)return -EINVAL;
 j->identity=*id;j->context=*ctx;j->tap_epoch=epoch;j->begun=1;tra_arm(&j->archive,id->owner,id->tx_generation-1);return 0;
}
int k7_apm21_joint_anchor(struct k7_apm21_joint *j,const struct k7_voice_anchor *a){
 if(!j||!j->begun||!a)return -EINVAL;if(j->failure)return j->failure;
 if(a->generation&&a->generation!=j->identity.tx_generation)return fail(j,-ESTALE);
 if(a->flags||a->epoch!=j->identity.clock_epoch)return fail(j,-EPIPE);
 tra_anchor(&j->archive,a);return atomic_load(&j->archive.state)==TRA_FAILED?fail(j,j->archive.reason):0;
}
int k7_apm21_joint_record(struct k7_apm21_joint *j,const struct k7_fifo_ref_record *r){
 if(!j||!j->begun||!r)return -EINVAL;if(j->failure)return j->failure;
 if(r->instance!=j->identity.reference_instance||r->generation!=j->identity.tx_generation)return fail(j,-ESTALE);
 tra_record(&j->archive,r);return atomic_load(&j->archive.state)==TRA_FAILED?fail(j,j->archive.reason):0;
}
int k7_apm21_joint_idle(struct k7_apm21_joint *j,const struct k7_apm21_idle_event *e){
 if(!j||!j->begun||!e)return -EINVAL;if(j->failure)return j->failure;
 if(!e->serial||(j->last_idle_serial&&e->serial!=j->last_idle_serial+1)||e->input_epoch!=j->identity.input_epoch||e->off.generation!=j->identity.tx_generation||e->off.epoch!=j->identity.clock_epoch||e->off.event!=K7_VOICE_ANCHOR_AMP_OFF||e->off.flags||e->off.result||e->off.after_us<e->off.before_us||e->boundary.epoch!=e->off.epoch||e->boundary.flags||e->boundary.after_us<e->boundary.before_us)return fail(j,-ESTALE);
 j->last_idle_serial=e->serial;
 if(e->kind==K7_APM21_IDLE_OFF){if(j->have_off||e->samples)return fail(j,-EPROTO);j->off=*e;j->have_off=1;return 0;}
 if(!j->have_off||!anchorsame(&j->off.off,&e->off))return fail(j,-ESTALE);
 if(e->kind==K7_APM21_IDLE_TX_DISABLED){
  if(j->have_disabled||j->have_quiet||j->have_end||e->samples||(e->xfer&15)!=11||e->boundary.event!=K7_VOICE_ANCHOR_TX_STOP||e->boundary.generation!=j->identity.tx_generation||e->boundary.before_us<e->off.after_us||e->first!=e->next||e->first!=e->boundary.rx_samples)return fail(j,-EPROTO);
  j->disabled=*e;j->have_disabled=1;return 0;
 }
 if(e->kind==K7_APM21_IDLE_DIGITAL_QUIET){
  if(!j->have_disabled||j->have_quiet||j->have_end||e->samples||e->quiet_checks!=31||(e->xfer&15)!=11||e->boundary.generation!=j->identity.tx_generation||e->boundary.before_us<e->off.after_us||e->first!=e->next||e->first!=e->boundary.rx_samples)return fail(j,-EPROTO);
  j->quiet=*e;j->have_quiet=1;return 0;
 }
 if(e->kind==K7_APM21_IDLE_SOURCE_END){if(!j->have_quiet||j->have_end||e->boundary.event!=K7_VOICE_ANCHOR_TX_COMPLETE||e->boundary.generation!=j->identity.tx_generation||e->boundary.before_us<j->quiet.boundary.after_us)return fail(j,-EPROTO);j->source_end=*e;j->have_end=1;return 0;}
 if(e->kind==K7_APM21_IDLE_MIC){
  if(e->samples!=320||e->next-e->first!=320||e->first<=e->off.rx_samples||e->boundary.event!=K7_VOICE_ANCHOR_RX_FRAME||e->boundary.rx_samples!=e->next||(j->idle_frames&&e->first!=j->idle_next))return fail(j,-EPIPE);
  if(!j->idle_frames)j->idle_first=e->first;j->idle_next=e->next;j->idle_frames++;return 0;
 }
 return fail(j,-ESTALE); /* next TX cannot authorize queued OLD tail */
}
int k7_apm21_joint_cut(struct k7_apm21_joint *j,const struct k7_apm21_cut_descriptor *d){
 if(!j||!j->begun||!d)return -EINVAL;if(j->failure)return j->failure;
 if(j->have_cut||!d->serial||d->owner!=j->identity.owner||d->tap_epoch!=j->tap_epoch||d->reference_instance!=j->identity.reference_instance||d->reference_epoch!=j->identity.reference_epoch||d->generation!=j->identity.tx_generation||d->input_epoch!=j->identity.input_epoch||d->clock_epoch!=j->identity.clock_epoch||d->remaining_words>16||(d->remaining_words&1)||d->first>d->cut||d->cut>d->written||d->published_upper>d->cut||d->after_us<d->before_us||d->prefix_samples>=160||d->cut-d->published_upper!=d->prefix_samples||d->written-d->cut!=d->remaining_words/2)return fail(j,-ESTALE);
 j->cut=*d;j->have_cut=1;return 0;
}
int k7_apm21_joint_reconcile(struct k7_apm21_joint *j,const struct k7_apm21_live_worker *w){
 if(!j||!w||!j->begun)return -EINVAL;if(j->failure)return j->failure;
 unsigned ts=atomic_load(&j->archive.state);if(ts==TRA_FAILED)return fail(j,j->archive.reason);
 if(!j->have_cut||!j->have_off||!j->have_disabled||!j->have_quiet||!j->have_end||ts!=TRA_DONE)return 0;
 const struct tail_reference_archive *a=&j->archive;const struct k7_apm21_cut_descriptor *d=&j->cut;
 if(d->before_us<j->disabled.boundary.after_us||d->after_us>j->quiet.boundary.before_us||a->owner!=d->owner||a->instance!=d->reference_instance||a->epoch!=d->reference_epoch||a->generation!=d->generation||a->first!=d->first||a->next!=d->written||a->uncertain_partial_pair||a->digital_complete_pair_next!=d->cut||a->clear.tx_fifo_words!=d->remaining_words||j->source_end.boundary.tx_queued_samples!=a->source_next||j->source_end.boundary.discarded_words!=a->clear.discarded_words||w->owner!=d->owner||w->tap_epoch!=d->tap_epoch)return fail(j,-ESTALE);
 for(unsigned n=0;n<d->prefix_samples;n++){
  uint64_t index=d->published_upper+n;unsigned at=(unsigned)(index%TRA_SAMPLES);
  if(index<a->kept_first||index>=a->next||a->pcm[at]!=d->prefix_pcm[n]||a->zeros[at]!=((d->prefix_zero_bits[n/8]>>(n%8))&1u))return fail(j,-EILSEQ);
 }
 j->transport_verified=1; /* facts only; ingestion/cache gate follows */
 if(w->stream.ingestion_fault||w->stream.attempted_reference_next!=w->stream.accepted_reference_next||w->stream.accepted_reference_next!=w->stream.reference_next||(!j->hypothesis_enabled&&(w->stream.accepted_reference_next>d->cut||w->stream.attempted_reference_next>d->cut)))return -EPIPE;
 return 1;
}
int k7_apm21_joint_live_ready(const struct k7_apm21_joint *j,const struct k7_apm21_live_worker *w,const struct k7_apm21_evidence_bridge *b){
 if(!j||!w||!b||!j->begun||j->failure||j->have_off||j->have_cut||w->stream.state!=K7_APM21_STREAM_ACTIVE||w->stream.ingestion_fault||w->stream.attempted_reference_next!=w->stream.accepted_reference_next||w->stream.accepted_reference_next!=w->stream.reference_next||!b->packet_valid||b->failure||!same(&j->identity,&b->packet.identity)||!ctxsame(&j->context,&b->packet_context)||b->packet.cancel_observed)return 0;
 return w->owner==j->identity.owner&&w->tap_epoch==j->tap_epoch;
}
int k7_apm21_joint_tail_ready(struct k7_apm21_joint *j,const struct k7_apm21_live_worker *w,const struct k7_apm21_evidence_bridge *b){
 int rc=k7_apm21_joint_reconcile(j,w);if(rc!=1)return rc?rc:-EAGAIN;
 if(w->stream.state!=K7_APM21_STREAM_ACTIVE||!w->stream.have_tail||!w->pending_pair_valid||w->pending_half_lease!=j->identity.reply||w->pending_ref.first>UINT64_MAX-160||w->pending_ref.first+160!=w->stream.accepted_reference_next||w->pending_mic.first>UINT64_MAX-160||w->pending_mic.first+160!=w->stream.mic_next||b->failure||!b->have_identity||!same(&b->identity,&j->identity)||!ctxsame(&b->context,&j->context))return -ENODATA;
 /* Existing cached160/real32 are required, not an empty bridge ledger.
  * The next genuine MIC half releases them through the unchanged transform. */
 if(!j->hypothesis_enabled){
  if(w->ref_count||w->ref_next!=j->cut.published_upper||w->stream.accepted_reference_next!=j->cut.published_upper||w->reference_origin>j->cut.cut||w->mic_origin>UINT64_MAX-(j->cut.cut-w->reference_origin))return -EAGAIN;
  uint64_t nominal_cut=w->mic_origin+j->cut.cut-w->reference_origin;
  if(nominal_cut<j->disabled.boundary.rx_samples)return -ENODATA;
 }
 return 1;
}
int k7_apm21_joint_continue(struct k7_apm21_joint *j,struct k7_apm21_live_worker *w,struct k7_apm21_evidence_bridge *b){
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(!w||w->binding.product_experiment!=1)return 0;
 int rc=k7_apm21_joint_tail_ready(j,w,b);if(rc!=1)return rc==-EAGAIN?0:rc;
 if(!j->hypothesis_enabled){
  b->poststop_cut=j->cut.cut;b->poststop_enabled=1;j->hypothesis_enabled=1;
 }
 return k7_apm21_live_worker_poststop(w,&j->cut);
#else
 (void)j;(void)w;(void)b;return 0;
#endif
}
#endif
