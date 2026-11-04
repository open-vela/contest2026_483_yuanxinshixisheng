/* Explicit private replay only. No production microphone/render, VAD or ASR replacement. */
#if defined(K7_APM21_RECORDED_QUIET) && K7_APM21_RECORDED_QUIET
#include <nuttx/config.h>
#if !defined(CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER) || !defined(CONFIG_EXAMPLES_K7SOUND_VAD_PROBE)
#error Existing exclusive VAD owner and boot admission are required
#endif
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error This frozen S16LE recording requires the reviewed little-endian target
#endif
#include "k7_apm21_consumer.h"
#include "k7_vad_ort.h"
#include "near_evidence.h"
#include "raw_vad_owner.h"
#include "vad_probe_asset.h"
#include "k7boot_aliyun_sequence.h"
/* Private ABI in the same guarded boot TU; no shared header changes. */
int k7boot_aliyun_recorded_quiet_acquire(const void *);
int k7boot_aliyun_recorded_quiet_release(const void *);
#include "vv_native_service.h"
#include "vv_speech_owner.h"
#include "vv_voice_session.h"
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define CAPTURE_SAMPLES 180800u
#define COMMON_SAMPLES 180480u
#define CAPTURE_MIC_FIRST UINT64_C(1705807)
#define OLD_FINAL_FIRST 180224u
#define OLD_FINAL_NEXT 180800u
#define RETAINED_ROWS 8u
#define RESERVE_BYTES (16u*1024u*1024u)
#include "recorded_quiet_pcm.inc"
_Static_assert(sizeof(recorded_mic)==CAPTURE_SAMPLES*2u,"MIC byte count");
_Static_assert(sizeof(recorded_reference)==sizeof(recorded_mic),"Reference byte count");
_Static_assert(sizeof(recorded_residual)==sizeof(recorded_mic),"Saved residual byte count");

static atomic_flag command_busy=ATOMIC_FLAG_INIT;
static atomic_bool cleanup_retained;
static const char speech_owner_token;
struct row {
 uint64_t window,first,next;
 uint32_t p_bits;
 unsigned consecutive,candidate;
};
struct pass_stats {
 const char *name;
 unsigned fed_samples,windows,p_ge90,candidate_windows,max_consecutive;
 uint32_t max_p_bits,p_crc;
 uint64_t first_candidate_first,first_candidate_next,last_window_next;
 struct row tail[RETAINED_ROWS];
};
static struct {
 int result,resource_release;
 uint64_t run;
 unsigned apm_calls,api_halves,pending_half,held_tail;
 unsigned model_acquired,raw_probe,boot_gate,speech_claim;
 struct pass_stats passes[4];
} job;
static uint64_t sequence;
static _Alignas(K7_APM21_CONTEXT_ALIGNMENT) unsigned char apm_storage[K7_APM21_CONTEXT_BYTES];
static int16_t api_output[160],actual_tail[32],assembled[320];
static const struct ne_policy unchanged_policy={1000,.5,.01,.9f,3};

static uint32_t crc_byte(uint32_t crc,unsigned char x)
{
 crc^=x;
 for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
 return crc;
}
static int idle(void)
{
 struct vv_native_service_status s;
 int rc=vv_native_service_status(&s);
 return rc?rc:(s.running||s.stopping||vv_voice_session_audio())?-EBUSY:0;
}
static int still_owned(void)
{
 int rc=idle();
 return rc?rc:vv_speech_owner_native_available(&speech_owner_token)?0:-ESTALE;
}
static int verify_recording(void)
{
 unsigned char digest[32];
 const int16_t *const data[]={recorded_mic,recorded_reference,recorded_residual};
 const unsigned char *const expected[]={recorded_mic_sha,recorded_reference_sha,recorded_residual_sha};
 for(unsigned i=0;i<3;++i){
  if(k7_vad_probe_hash(data[i],sizeof(recorded_mic),digest,NULL))return -EIO;
  if(memcmp(digest,expected[i],32))return -EBADMSG;
 }
 return 0;
}
static struct ne_identity identity(void)
{
 /* Frozen archive identity only: never authorization for a current live reply. */
 return (struct ne_identity){1,1,3,1,1,1,1,1,1,1,2};
}
static int begin_pass(struct ne_worker *near,struct pass_stats *s,const char *name)
{
 memset(s,0,sizeof(*s));s->name=name;s->p_crc=UINT32_MAX;
 struct ne_identity id=identity();
 return ne_begin(near,&id,CAPTURE_MIC_FIRST);
}
static int observe(struct ne_worker *near,struct pass_stats *s,unsigned offset,const int16_t residual[320])
{
 int rc=still_owned();if(rc)return rc;
 if(offset!=s->fed_samples || offset>CAPTURE_SAMPLES-320u)return -EPROTO;
 struct ne_identity id=identity();struct ne_report r;
 rc=ne_feed(near,&id,&id,CAPTURE_MIC_FIRST+offset,recorded_mic+offset,
            recorded_reference+offset,residual,&r);
 if(rc<0)return rc;
 s->fed_samples+=320u;
 if(rc){
  if(r.qualified || r.model.input_first_sample<CAPTURE_MIC_FIRST ||
     r.model.input_end_sample>CAPTURE_MIC_FIRST+s->fed_samples)return -EPROTO;
  uint32_t bits;memcpy(&bits,&r.model.probability,sizeof(bits));
  if(bits>s->max_p_bits)s->max_p_bits=bits;
  for(unsigned i=0;i<4;++i)s->p_crc=crc_byte(s->p_crc,(unsigned char)(bits>>(8u*i)));
  if(r.model.probability>=unchanged_policy.minimum_probability)++s->p_ge90;
  if(r.consecutive>s->max_consecutive)s->max_consecutive=r.consecutive;
  if(r.candidate){
   if(!s->candidate_windows){s->first_candidate_first=r.evidence_first-CAPTURE_MIC_FIRST;s->first_candidate_next=r.evidence_next-CAPTURE_MIC_FIRST;}
   ++s->candidate_windows;
  }
  s->tail[s->windows%RETAINED_ROWS]=(struct row){r.model.window_id,
   r.model.input_first_sample-CAPTURE_MIC_FIRST,r.model.input_end_sample-CAPTURE_MIC_FIRST,
   bits,r.consecutive,r.candidate};
  s->last_window_next=r.model.input_end_sample-CAPTURE_MIC_FIRST;++s->windows;
 }
 return 0;
}
static int saved_pass(struct ne_worker *near,struct pass_stats *s,const char *name,
 const int16_t *data,unsigned samples)
{
 int rc=begin_pass(near,s,name);if(rc)return rc;
 if(samples>CAPTURE_SAMPLES || samples%320u)return -EINVAL;
 for(unsigned i=0;i<samples;i+=320u){rc=observe(near,s,i,data+i);if(rc)return rc;}
 return 0;
}
static int apm_pass(struct ne_worker *near,struct pass_stats *s)
{
 int rc=begin_pass(near,s,"APM_COMMON_HYPOTHESIS");if(rc)return rc;
 k7_apm21_context *apm=NULL;int32_t upstream=0;
 rc=k7_apm21_construct(apm_storage,sizeof(apm_storage),job.run,&apm,&upstream);
 if(!rc && (!apm||upstream))rc=-EPROTO;
 const struct k7_apm21_timing timing={1,0,K7_APM21_DELAY_OFFLINE_HYPOTHESIS};
 unsigned have_tail=0,pending=0;
 for(unsigned half=0;!rc && half<CAPTURE_SAMPLES/160u;++half){
  rc=still_owned();if(rc)break;
  struct k7_apm21_frame_report report={0};
  rc=k7_apm21_process_10ms(apm,job.run,half,recorded_reference+half*160u,
      recorded_mic+half*160u,&timing,api_output,&report);
  if(rc)break;
  if(report.stream_epoch!=job.run || report.frame_ordinal!=half ||
     report.stage!=K7_APM21_COMPLETE || report.upstream_rc || report.api_output_samples!=160 ||
     report.algorithmic_output_delay_samples!=-1 || report.nominal_support_qualified ||
     report.delay_source!=K7_APM21_DELAY_OFFLINE_HYPOTHESIS || report.supplied_buffer_delay_ms!=0){rc=-EPROTO;break;}
  ++job.apm_calls;
  if(have_tail){
   memcpy(assembled+pending,actual_tail,sizeof(actual_tail));
   memcpy(assembled+pending+32u,api_output,128u*sizeof(int16_t));
   pending+=160u;++job.api_halves;
   if(pending==320u){rc=observe(near,s,s->fed_samples,assembled);pending=0;}
  }
  memcpy(actual_tail,api_output+128,sizeof(actual_tail));have_tail=1;
 }
 job.pending_half=pending;job.held_tail=have_tail?32u:0u;
 /* No manufactured next MIC/reference, startup seed, partial ne frame or flush. */
 k7_apm21_destroy(apm);
 if(!rc && (job.apm_calls!=1130u || job.api_halves!=1129u || pending!=160u || s->fed_samples!=COMMON_SAMPLES))rc=-ENODATA;
 return rc;
}
static void print_pass(const struct pass_stats *s)
{
 if(!s->name)return;
 printf("APM_RECORDED_PASS name=%s fed_samples=%u windows=%u p_ge90=%u candidate_windows=%u max_consecutive=%u max_p_bits=%08" PRIx32 " p_crc=%08" PRIx32 " last_window_next=%" PRIu64 " first_candidate_first=%" PRIu64 " first_candidate_next=%" PRIu64 " qualified=0 actions=0\n",
  s->name,s->fed_samples,s->windows,s->p_ge90,s->candidate_windows,s->max_consecutive,
  s->max_p_bits,s->p_crc^UINT32_MAX,s->last_window_next,s->first_candidate_first,s->first_candidate_next);
 unsigned begin=s->windows>RETAINED_ROWS?s->windows-RETAINED_ROWS:0;
 for(unsigned i=begin;i<s->windows;++i){const struct row *p=&s->tail[i%RETAINED_ROWS];
  printf("APM_RECORDED_ROW pass=%s window=%" PRIu64 " first=%" PRIu64 " next=%" PRIu64 " p_bits=%08" PRIx32 " consecutive=%u candidate=%u\n",s->name,p->window,p->first,p->next,p->p_bits,p->consecutive,p->candidate);
 }
}
static unsigned saved_baseline_reproduced(void)
{
 const struct pass_stats *s=&job.passes[2];
 if(s->fed_samples!=CAPTURE_SAMPLES || s->windows!=353u || s->candidate_windows!=1u ||
    s->last_window_next!=OLD_FINAL_NEXT)return 0;
 const struct row *last=&s->tail[(s->windows-1u)%RETAINED_ROWS];
 return last->window==352u && last->first==OLD_FINAL_FIRST && last->next==OLD_FINAL_NEXT &&
        last->candidate==1u && last->consecutive==3u;
}
static void *worker(void *unused)
{
 (void)unused;struct k7_vad_probe_lease asset={0};struct k7_vad_ort *backend=NULL;
 struct k7_vad *vad=NULL;struct ne_worker *near=NULL;
 int rc=idle(),release=0;
 if(!rc){rc=vv_speech_owner_native_claim(&speech_owner_token);job.speech_claim=!rc;}
 if(!rc){rc=k7_raw_vad_owner_probe_begin();job.raw_probe=!rc;}
 if(!rc){rc=k7boot_aliyun_recorded_quiet_acquire(&speech_owner_token);job.boot_gate=(!rc||rc==-ESTALE);}
 if(!rc)rc=still_owned();
 if(!rc && (size_t)mallinfo().fordblks<RESERVE_BYTES)rc=-ENOMEM;
 if(!rc)rc=verify_recording();
 if(!rc){rc=k7_vad_probe_asset_acquire(&asset);job.model_acquired=!rc;}
 if(!rc){struct k7_vad_model model={asset.data,asset.bytes,k7_vad_probe_hash,NULL};rc=k7_vad_ort_create(OrtGetApiBase(),&model,&backend);}
 if(!rc && (size_t)mallinfo().fordblks<RESERVE_BYTES)rc=-ENOMEM;
 if(!rc)rc=k7_vad_create(k7_vad_ort_ops(),backend,&vad);
 if(!rc)rc=ne_create(vad,&unchanged_policy,&near);
 if(!rc)rc=saved_pass(near,&job.passes[0],"RAW_COMMON",recorded_mic,COMMON_SAMPLES);
 if(!rc)rc=saved_pass(near,&job.passes[1],"SAVED_RES_COMMON",recorded_residual,COMMON_SAMPLES);
 if(!rc)rc=saved_pass(near,&job.passes[2],"SAVED_RES_FULL",recorded_residual,CAPTURE_SAMPLES);
 if(!rc)rc=apm_pass(near,&job.passes[3]);
 ne_destroy(near);k7_vad_destroy(vad);k7_vad_ort_destroy(backend);
 if(asset.id)release=k7_vad_probe_asset_release(&asset);
 /* Unknown resource cleanup retains all remaining admissions. Never advertise
  * READY, release somebody else's resident model, or silently start voice. */
 if(!release && job.boot_gate)release=k7boot_aliyun_recorded_quiet_release(&speech_owner_token);
 if(!release && job.raw_probe)release=k7_raw_vad_owner_probe_end();
 if(!release && job.speech_claim)vv_speech_owner_native_release(&speech_owner_token);
 if(release)atomic_store_explicit(&cleanup_retained,true,memory_order_release);
 job.resource_release=release;job.result=rc?rc:release;
 for(unsigned i=0;i<4;++i)print_pass(&job.passes[i]);
 printf("APM_RECORDED_BOUNDARY source_samples=180800 planned_common_samples=180480 APM_calls=%u APM_input_processed_samples=%u APM_ne_fed_samples=%u output_halves=%u first_call_published=0 pending_half_samples=%u retained_actual_tail_samples=%u nominal_unpublished_at_success=320 final_old_window_first=180224 final_old_window_next=180800 old_final_window_common_covered=0 old_final_candidate=UNOBSERVED_IN_APM saved_full_candidate_windows=%u tail_flushed=0 padded_samples=0\n",
  job.apm_calls,job.apm_calls*160u,job.passes[3].fed_samples,job.api_halves,job.pending_half,job.held_tail,job.passes[2].candidate_windows);
 printf("APM_RECORDED_RESULT rc=%d release_rc=%d run=%" PRIu64 " execution_success=%u baseline_full_reproduced=%u comparison_candidate_change_qualified=0 model_acquired=%u raw_probe_admitted=%u boot_gate_admitted=%u dataset=a68e6d57 archive_MIC_first=1705807 archive_ref_first=28441 capture_lag_hypothesis_samples=39 buffer_delay_ms=0 delay_source=OFFLINE_HYPOTHESIS API_D128_measured_scope=715_zero_render_delay0 real_reference_D128_verified=0 actual_ADC=0 actual_render=0 cloud=0 gimbal=0 ASR=0 cancel_actions=0 quiet_acceptance=0 near_retention_accepted=0 qualified=0\n",
  job.result,release,job.run,(unsigned)(job.result==0),saved_baseline_reproduced(),job.model_acquired,job.raw_probe,job.boot_gate);
 return NULL;
}
int k7_apm21_recorded_quiet_command(int argc,char **argv)
{
 if(argc!=2 || strcmp(argv[1],"apm-recorded-quiet"))return 1;
 if(atomic_flag_test_and_set_explicit(&command_busy,memory_order_acquire))return 1;
 if(sequence==UINT64_MAX){atomic_flag_clear_explicit(&command_busy,memory_order_release);return 1;}
 memset(&job,0,sizeof(job));job.run=++sequence;job.result=-ECANCELED;
 pthread_attr_t attr;pthread_t thread;
 int setup=pthread_attr_init(&attr),attr_release=0,joined=0;
 unsigned initialized=!setup,created=0;
 if(!setup)setup=pthread_attr_setstacksize(&attr,256u*1024u);
 if(!setup)setup=pthread_attr_setinheritsched(&attr,PTHREAD_INHERIT_SCHED);
 if(!setup)setup=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE);
 if(!setup){setup=pthread_create(&thread,&attr,worker,NULL);created=!setup;}
 if(initialized)attr_release=pthread_attr_destroy(&attr);
 if(created)joined=pthread_join(thread,NULL);
 if(attr_release||joined)atomic_store_explicit(&cleanup_retained,true,memory_order_release);
 printf("APM_RECORDED_WORKER setup_rc=%d attr_cleanup_rc=%d join_rc=%d created=%u joined=%u requested_stack_bytes=262144 inherit_sched=1 affinity_set_calls=0 diagnostic_gate_retained=%u\n",
  setup,attr_release,joined,created,created&&!joined,(unsigned)atomic_load_explicit(&cleanup_retained,memory_order_acquire));
 if(!atomic_load_explicit(&cleanup_retained,memory_order_acquire))atomic_flag_clear_explicit(&command_busy,memory_order_release);
 return setup||attr_release||joined||job.result?1:0;
}
#endif
