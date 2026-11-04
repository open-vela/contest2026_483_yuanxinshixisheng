#if defined(K7_APM21_PHYSICAL_STREAM) && K7_APM21_PHYSICAL_STREAM
#include "k7_apm21_physical_stream.h"
#include <string.h>

static int current(const struct k7_apm21_physical_stream *s,uint64_t owner,uint64_t epoch)
{
 if(!s || !owner || !epoch)return K7_APM21_STREAM_ARGUMENT;
 if(s->owner!=owner || s->dsp_epoch!=epoch)return K7_APM21_STREAM_STALE;
 return s->state==K7_APM21_STREAM_ACTIVE?0:K7_APM21_STREAM_DISCONTINUITY;
}
static int fail(struct k7_apm21_physical_stream *s,int reason)
{
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 s->ingestion_fault=1;
#endif
 s->state=K7_APM21_STREAM_GAP;s->have_tail=0;s->reply_lease=0;
 s->last_error=reason?reason:K7_APM21_STREAM_DISCONTINUITY;
 return s->last_error;
}
static int profile_valid(const struct k7_apm21_stream_profile *p)
{
 unsigned nonzero=0;
 if(!p || p->kind!=K7_APM21_STREAM_PROFILE_TARGET_D128 ||
    p->measured_algorithmic_delay_samples!=128)return 0;
 for(unsigned i=0;i<sizeof(p->target_receipt_sha256);++i)nonzero|=p->target_receipt_sha256[i];
 return nonzero!=0;
}
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
int k7_apm21_stream_prepare(struct k7_apm21_physical_stream *s,uint64_t owner,
 uint64_t epoch,const struct k7_apm21_stream_profile *profile,int32_t *upstream)
{
 if(upstream)*upstream=0;
 if(!s||!owner||!epoch||!upstream)return K7_APM21_STREAM_ARGUMENT;
 if((s->state!=K7_APM21_STREAM_EMPTY&&s->state!=K7_APM21_STREAM_CLOSED)||s->consumer)return K7_APM21_STREAM_BUSY;
 if(epoch<=s->last_epoch)return K7_APM21_STREAM_STALE;
 if(!profile_valid(profile))return K7_APM21_STREAM_UNVERIFIED;
 s->owner=owner;s->dsp_epoch=s->last_epoch=epoch;s->ordinal=0;
 s->reply_lease=s->lease_high_water=s->pending_reply_lease=0;
 s->profile=*profile;s->have_tail=0;s->last_error=0;
 int rc=k7_apm21_construct(s->consumer_storage,sizeof(s->consumer_storage),epoch,&s->consumer,upstream);
 if(rc||!s->consumer){s->state=K7_APM21_STREAM_CLOSED;s->last_error=rc?rc:K7_APM21_STREAM_REPORT;return s->last_error;}
 s->state=K7_APM21_STREAM_PREPARED;return 0;
}
int k7_apm21_stream_bind_origin(struct k7_apm21_physical_stream *s,uint64_t owner,
 uint64_t epoch,uint64_t mic,uint64_t ref)
{
 if(!s||!owner||!epoch)return K7_APM21_STREAM_ARGUMENT;
 if(owner!=s->owner||epoch!=s->dsp_epoch)return K7_APM21_STREAM_STALE;
 if(s->state!=K7_APM21_STREAM_PREPARED||!s->consumer)return K7_APM21_STREAM_BUSY;
 if(mic>UINT64_MAX-160||ref>UINT64_MAX-160)return K7_APM21_STREAM_OVERFLOW;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 s->attempted_reference_next=s->accepted_reference_next=ref;s->ingestion_fault=0;
#endif
 s->mic_next=mic;s->reference_next=ref;s->state=K7_APM21_STREAM_ACTIVE;return 0;
}
#endif
int k7_apm21_stream_start(struct k7_apm21_physical_stream *s,uint64_t owner,
#line 26
 uint64_t epoch,uint64_t mic,uint64_t ref,const struct k7_apm21_stream_profile *profile,
 int32_t *upstream)
{
 if(upstream)*upstream=0;
 if(!s || !owner || !epoch || !upstream)return K7_APM21_STREAM_ARGUMENT;
 if(s->state!=K7_APM21_STREAM_EMPTY && s->state!=K7_APM21_STREAM_CLOSED)
  return K7_APM21_STREAM_BUSY;
 if(s->consumer)return K7_APM21_STREAM_BUSY;
 if(epoch<=s->last_epoch)return K7_APM21_STREAM_STALE;
 if(!profile_valid(profile))return K7_APM21_STREAM_UNVERIFIED;
 if(mic>UINT64_MAX-160 || ref>UINT64_MAX-160)return K7_APM21_STREAM_OVERFLOW;
 s->owner=owner;s->dsp_epoch=s->last_epoch=epoch;s->ordinal=0;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 s->attempted_reference_next=s->accepted_reference_next=ref;s->ingestion_fault=0;
#endif
 s->mic_next=mic;s->reference_next=ref;s->reply_lease=s->lease_high_water=0;
 s->profile=*profile;s->have_tail=0;s->last_error=0;
 int rc=k7_apm21_construct(s->consumer_storage,sizeof(s->consumer_storage),epoch,&s->consumer,upstream);
 if(rc){s->state=K7_APM21_STREAM_CLOSED;s->last_error=rc;return rc;}
 if(!s->consumer){s->state=K7_APM21_STREAM_CLOSED;s->last_error=K7_APM21_STREAM_REPORT;return s->last_error;}
 s->state=K7_APM21_STREAM_ACTIVE;return 0;
}
int k7_apm21_stream_rebind_reply(struct k7_apm21_physical_stream *s,uint64_t owner,
 uint64_t epoch,uint64_t expected,uint64_t next)
{
 int rc=current(s,owner,epoch);if(rc)return rc;
 if(expected!=s->reply_lease || (next && next<=s->lease_high_water))return K7_APM21_STREAM_STALE;
 s->reply_lease=next;if(next)s->lease_high_water=next;return 0;
}
int k7_apm21_stream_gap(struct k7_apm21_physical_stream *s,uint64_t owner,uint64_t epoch,int reason)
{
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(s&&s->owner==owner&&s->dsp_epoch==epoch&&s->state==K7_APM21_STREAM_PREPARED)
  return fail(s,reason<0?reason:K7_APM21_STREAM_DISCONTINUITY);
#endif
 int rc=current(s,owner,epoch);if(rc)return rc;
 /* Never turn a positive caller event into a success/warmup result. */
#line 56
 return fail(s,reason<0?reason:K7_APM21_STREAM_DISCONTINUITY);
}
int k7_apm21_stream_process(struct k7_apm21_physical_stream *s,
 const struct k7_apm21_stream_input *in,struct k7_apm21_stream_output *out)
{
 if(!s || !in)return K7_APM21_STREAM_ARGUMENT;
 int rc=current(s,in->owner,in->dsp_epoch);if(rc)return rc;
 if(!out || !in->mic || !in->reference || !in->timing || in->samples!=160 ||
    in->reference_complete!=1)return fail(s,K7_APM21_STREAM_DISCONTINUITY);
 if(in->ordinal!=s->ordinal || in->mic_first!=s->mic_next ||
    in->reference_first!=s->reference_next)return fail(s,K7_APM21_STREAM_DISCONTINUITY);
 if(s->ordinal==UINT64_MAX || in->mic_first>UINT64_MAX-160 ||
    in->reference_first>UINT64_MAX-160)return fail(s,K7_APM21_STREAM_OVERFLOW);
 struct k7_apm21_frame_report report={0};
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 s->attempted_reference_next=in->reference_first+160; /* API may ingest before reporting failure. */
#endif
 rc=k7_apm21_process_10ms(s->consumer,s->dsp_epoch,s->ordinal,in->reference,in->mic,
                         in->timing,s->api_output,&report);
 if(rc)return fail(s,rc<0?rc:K7_APM21_STREAM_REPORT);
 if(report.stream_epoch!=s->dsp_epoch || report.frame_ordinal!=s->ordinal ||
    report.stage!=K7_APM21_COMPLETE || report.upstream_rc || report.api_output_samples!=160 ||
    report.algorithmic_output_delay_samples!=-1 || report.nominal_support_qualified ||
    report.delay_source!=in->timing->source ||
    report.supplied_buffer_delay_ms!=in->timing->supplied_buffer_delay_ms)
  return fail(s,K7_APM21_STREAM_REPORT);
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 s->accepted_reference_next=in->reference_first+160; /* Includes first validated WARMUP call. */
#endif
 struct k7_apm21_stream_output result={0};
 if(s->have_tail){
  if(in->mic_first<160 || in->reference_first<160)return fail(s,K7_APM21_STREAM_OVERFLOW);
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  result.owner=s->owner;result.dsp_epoch=s->dsp_epoch;result.reply_lease=s->pending_reply_lease;
#else
  result.owner=s->owner;result.dsp_epoch=s->dsp_epoch;result.reply_lease=s->reply_lease;
#endif
#line 83
  result.accepted_ordinal=s->ordinal;result.nominal_mic_first=in->mic_first-160;
  result.nominal_reference_first=in->reference_first-160;
  result.available_after_mic_next=in->mic_first+160;
  result.available_after_reference_next=in->reference_first+160;
  result.samples=160;result.algorithmic_delay_samples=160;result.extra_fifo_samples=32;
  /* qualified remains0 even with a verified delay profile. */
  memcpy(result.pcm,s->real_tail,sizeof(s->real_tail));
  memcpy(result.pcm+32,s->api_output,128u*sizeof(int16_t));
 }
 unsigned publish=s->have_tail;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 s->pending_reply_lease=s->reply_lease;
#endif
#line 93
 memcpy(s->real_tail,s->api_output+128,sizeof(s->real_tail));s->have_tail=1;
 s->mic_next=in->mic_first+160;s->reference_next=in->reference_first+160;s->ordinal++;
 if(!publish)return K7_APM21_STREAM_WARMUP;
 *out=result;return K7_APM21_STREAM_OUTPUT;
}
int k7_apm21_stream_stop(struct k7_apm21_physical_stream *s,uint64_t owner,uint64_t epoch)
{
 if(!s || !owner || !epoch)return K7_APM21_STREAM_ARGUMENT;
 if(s->owner!=owner || s->dsp_epoch!=epoch)return K7_APM21_STREAM_STALE;
 if(s->state==K7_APM21_STREAM_CLOSED)return 0;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(s->state!=K7_APM21_STREAM_ACTIVE && s->state!=K7_APM21_STREAM_GAP && s->state!=K7_APM21_STREAM_PREPARED)
#else
 if(s->state!=K7_APM21_STREAM_ACTIVE && s->state!=K7_APM21_STREAM_GAP)
#endif
#line 104
  return K7_APM21_STREAM_ARGUMENT;
 k7_apm21_destroy(s->consumer);s->consumer=NULL;
 s->state=K7_APM21_STREAM_CLOSED;s->have_tail=0;s->reply_lease=0;return 0;
}
#endif
