/* One explicit private experiment, never a product clean-audio interface. */
#if defined(K7_APM21_FIXED_PHYSICAL) && K7_APM21_FIXED_PHYSICAL
#include <nuttx/config.h>
#if !defined(CONFIG_EXAMPLES_K7SOUND_SESSION) || !defined(CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER)
#error Existing physical session and exclusive raw voice owner are required
#endif
#if !defined(K7_APM21_RECORDED_QUIET) || !K7_APM21_RECORDED_QUIET
#error Keep the reviewed private post-stop boot lease in this candidate
#endif
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Fixed source and exported samples are S16LE
#endif
#include "k7_apm21_fixed_physical.h"
#include "k7_apm21_consumer.h"
#include "duplex_session.h"
#include "echo_diag_worker.h"
#include "raw_vad_owner.h"
#include "vv_native_service.h"
#include "vv_speech_owner.h"
#include "vv_voice_session.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "session_ab_pcm.inc"

int k7boot_aliyun_recorded_quiet_acquire(const void *);
int k7boot_aliyun_recorded_quiet_release(const void *);
#define INPUT_SAMPLES 240000u
#define TX_SAMPLES 256000u
#define API_HALVES (INPUT_SAMPLES/160u)
#define PENDING 8u
#define RESERVE_BYTES (16u*1024u*1024u)
_Static_assert(sizeof(k7_ab_pcm)==K7_AB_PCM_SAMPLES*2u,"fixed source byte count");
static atomic_flag command_busy=ATOMIC_FLAG_INIT;
static atomic_bool retained,stop_requested;
static const char speech_token;
static uint64_t sequence;
struct half_row {uint64_t mic_first,ref_first,ordinal,ref_before_us,ref_after_us;unsigned usec;};
struct ref_clock {uint64_t first,before_us,after_us;unsigned samples;};
static struct {
 uint64_t run,owner,clock_start,clock_last,mic_next,mic_first,ref_first;
 uint64_t ref_source_next,reference_next;
 int result,release,join_rc,destroy_rc;int32_t upstream;
 unsigned sealed,full,raw_samples,y_samples,halves,submitted,ended,tx_complete;
 unsigned speech_claim,raw_claim,boot_loan,source_checked,constructed,destroyed,session_destroyed;
 unsigned have_mic,have_begin,have_start,begun,initial_ref_notification;
 unsigned hist_head,hist_count,pending_count,apm_max_us;
 uint32_t mic_epoch,mic_crc,ref_crc,y_crc,zero_crc;
 struct k7_voice_session *session;
 struct k7_echo_diag *pairer;
 k7_apm21_context *consumer;
 struct k7_fifo_ref_record begin,pending[PENDING];
 struct k7_voice_anchor start;
 struct k7_voice_frame history[PENDING];
 struct k7_voice_session_status final_status;
 struct half_row row[API_HALVES];
 struct ref_clock ref_clock[INPUT_SAMPLES/320u];
 int16_t mic[INPUT_SAMPLES],ref[INPUT_SAMPLES],y[INPUT_SAMPLES];
 unsigned char zero[INPUT_SAMPLES/8u];
} job;
static _Alignas(K7_APM21_CONTEXT_ALIGNMENT) unsigned char storage[K7_APM21_CONTEXT_BYTES];
static int16_t api_y[160];
static uint32_t crc_byte(uint32_t c,unsigned char x){
 c^=x;for(unsigned i=0;i<8;++i)c=(c>>1)^((c&1u)?UINT32_C(0xedb88320):0u);return c;
}
static uint32_t crc_samples(uint32_t c,const int16_t *p,unsigned n){
 for(unsigned i=0;i<n;++i){uint16_t v=(uint16_t)p[i];c=crc_byte(crc_byte(c,(unsigned char)v),(unsigned char)(v>>8));}return c;
}
static uint64_t clock_us(void){
 struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||
  (uint64_t)t.tv_sec>(UINT64_MAX-999999u)/1000000u)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000000u+(unsigned long)t.tv_nsec/1000u;
}
static void pause_ms(void){struct timespec t={0,1000000};(void)nanosleep(&t,NULL);}
static int deadline(void){
 uint64_t n=clock_us();if(n==UINT64_MAX||n<job.clock_last)return -EIO;
 job.clock_last=n;
 if(atomic_load_explicit(&stop_requested,memory_order_acquire))return -ECANCELED;
 return n-job.clock_start>=UINT64_C(30000000)?-ETIMEDOUT:0;
}
static int idle(void){
 struct vv_native_service_status s;int rc=vv_native_service_status(&s);
 return rc?rc:(s.running||s.stopping||vv_voice_session_audio())?-EBUSY:0;
}
static int still_owned(void){int rc=idle();return rc?rc:vv_speech_owner_native_available(&speech_token)?0:-ESTALE;}
static int16_t source_sample(unsigned at){
 unsigned i=(at%K7_AB_PCM_SAMPLES)*2u;
 return (int16_t)((uint16_t)k7_ab_pcm[i]|((uint16_t)k7_ab_pcm[i+1u]<<8));
}
static int verify_source(void){
 uint32_t c=UINT32_MAX;for(size_t i=0;i<sizeof(k7_ab_pcm);++i)c=crc_byte(c,k7_ab_pcm[i]);
 if((c^UINT32_MAX)!=K7_AB_PCM_CRC)return -EBADMSG;
 job.source_checked=1;return 0;
}
static int snapshot(struct k7_fifo_ref_status *r,struct k7_echo_diag_mic_id *m){
 if(!job.session)return -ENOTCONN;
 struct k7_voice_session_status s;k7_voice_session_status(job.session,&s);
 if(!s.ready||!s.running||s.finished||s.stop_requested||s.input_epoch!=job.mic_epoch||
    s.kws_overflows||s.anchor_overflows||s.anchor_faults||s.render_overflows)return -EPIPE;
 int rc=k7_voice_session_fifo_status(job.session,r);
 if(rc)return rc;
 *m=(struct k7_echo_diag_mic_id){job.owner,s.input_epoch};
 return !r->eligible||r->exhausted||r->faults||r->generation!=1?-EPIPE:still_owned();
}
static int process_pairs(void){
 while(!job.full){
  struct k7_fifo_ref_status r;struct k7_echo_diag_mic_id m;struct k7_echo_diag_pair pair;
  int rc=snapshot(&r,&m);if(rc)return rc;
  rc=k7_echo_diag_pop(job.pairer,&r,m,&pair);if(rc<=0)return rc;
  if(!pair.hypothesis_only||pair.mic_owner!=job.owner||pair.mic_epoch!=job.mic_epoch||
     pair.mic_first!=job.mic_first+job.raw_samples||pair.fifo_first!=job.ref_first+job.raw_samples||
     job.raw_samples>INPUT_SAMPLES-320u)return -EPROTO;
  for(unsigned half=0;half<2;++half){
   unsigned offset=job.raw_samples;
   if(offset>INPUT_SAMPLES-160u||job.halves>=API_HALVES)return -EOVERFLOW;
   rc=snapshot(&r,&m);if(rc)return rc;
   if(r.instance!=pair.reference_instance||r.epoch!=pair.reference_epoch||r.generation!=pair.generation)return -ESTALE;
   memcpy(job.mic+offset,pair.microphone+half*160u,320u);
   memcpy(job.ref+offset,pair.reference+half*160u,320u);
   memcpy(job.zero+offset/8u,pair.explicit_zero_bits+half*20u,20u);
   job.mic_crc=crc_samples(job.mic_crc,job.mic+offset,160);
   job.ref_crc=crc_samples(job.ref_crc,job.ref+offset,160);
   for(unsigned i=0;i<20;++i)job.zero_crc=crc_byte(job.zero_crc,job.zero[offset/8u+i]);
   job.raw_samples+=160u; /* Genuine input survives even when this APM call fails. */
   const struct k7_apm21_timing timing={1,0,K7_APM21_DELAY_OFFLINE_HYPOTHESIS};
   struct k7_apm21_frame_report report={0};uint64_t before=clock_us();
   rc=k7_apm21_process_10ms(job.consumer,job.run,job.halves,job.ref+offset,job.mic+offset,&timing,api_y,&report);
   uint64_t after=clock_us();
   if(before==UINT64_MAX||after==UINT64_MAX||after<before||after-before>UINT_MAX)return -EIO;
   if(after-before>job.apm_max_us)job.apm_max_us=(unsigned)(after-before);
   if(rc)return rc<0?rc:-EPROTO;
   if(report.stream_epoch!=job.run||report.frame_ordinal!=job.halves||report.stage!=K7_APM21_COMPLETE||
      report.upstream_rc||report.api_output_samples!=160||report.algorithmic_output_delay_samples!=-1||
      report.nominal_support_qualified||report.delay_source!=K7_APM21_DELAY_OFFLINE_HYPOTHESIS||report.supplied_buffer_delay_ms)return -EPROTO;
   /* Recheck after the computation: an END/clear must not silently qualify a
    * just-computed block. Raw input remains; uncommitted Y is never padded. */
   rc=snapshot(&r,&m);if(rc)return rc;
   if(r.instance!=pair.reference_instance||r.epoch!=pair.reference_epoch||r.generation!=pair.generation)return -ESTALE;
   const struct ref_clock *clock=&job.ref_clock[offset/320u];
   if(clock->first!=pair.fifo_first||clock->samples!=320u||clock->after_us<clock->before_us)return -EPROTO;
   memcpy(job.y+job.y_samples,api_y,sizeof(api_y));job.y_crc=crc_samples(job.y_crc,api_y,160u);
   job.row[job.halves]=(struct half_row){pair.mic_first+half*160u,pair.fifo_first+half*160u,job.halves,clock->before_us,clock->after_us,(unsigned)(after-before)};
   job.y_samples+=160u;++job.halves;
  }
  if(job.raw_samples==INPUT_SAMPLES){job.full=1;job.sealed=1;return 0;}
 }
 return 0;
}
static int try_begin(void){
 if(job.begun||!job.have_begin||!job.have_start)return 0;
 struct k7_fifo_ref_status r;struct k7_echo_diag_mic_id m;int rc=snapshot(&r,&m);if(rc)return rc;
 if(job.start.generation!=1||job.start.flags||job.start.result||job.start.after_us<job.start.before_us||
    job.start.epoch!=k7_voice_session_anchor_epoch(job.session)||
    (job.hist_count&&job.history[job.hist_head].first_sample>job.start.rx_samples))return -EPIPE;
 rc=k7_echo_diag_begin(job.pairer,&job.begin,&r,m,job.begin.first_enqueue,job.start.rx_samples);if(rc)return rc;
 job.begun=1;job.mic_first=job.start.rx_samples;job.ref_first=job.begin.first_enqueue;
 for(unsigned i=0;i<job.hist_count;++i){struct k7_voice_frame *f=&job.history[(job.hist_head+i)%PENDING];
  rc=k7_echo_diag_microphone(job.pairer,m,f->first_sample,f->pcm,f->samples);if(rc)return rc;
 }
 job.hist_count=0;
 for(unsigned i=0;i<job.pending_count;++i){rc=k7_echo_diag_reference(job.pairer,&job.pending[i],&r);if(rc)return rc;}
 job.pending_count=0;return process_pairs();
}
static int drain(void){
 struct k7_voice_anchor a;int rc;
 for(unsigned i=0;i<16;++i){rc=k7_voice_session_take_anchor(job.session,&a);if(rc<0)return job.full?0:rc;if(!rc)break;
  if(!job.full&&(a.flags||a.result||a.after_us<a.before_us))return -EPIPE;
  if(a.event==K7_VOICE_ANCHOR_TX_START&&!job.full){if(job.have_start||a.generation!=1)return -EPROTO;job.start=a;job.have_start=1;}
 }
 if((rc=try_begin()))return rc;
 struct k7_fifo_ref_record ref;
 for(unsigned i=0;i<8;++i){
  rc=k7_voice_session_take_fifo_reference(job.session,&ref);
  if(rc<0){
   if(job.full)break;
   struct k7_fifo_ref_status s;
   if(rc==-EPIPE&&!job.begun&&!job.have_begin&&!job.initial_ref_notification&&
      !k7_voice_session_fifo_status(job.session,&s)&&s.eligible&&!s.exhausted&&!s.faults&&s.instance&&s.epoch&&s.generation==1){job.initial_ref_notification=1;continue;}
   return rc;
  }
  if(!rc)break;
  if(job.full)continue;
  if(ref.kind==K7_FIFO_REF_BEGIN){if(job.have_begin||ref.generation!=1)return -EPROTO;job.begin=ref;job.reference_next=ref.first_enqueue;job.have_begin=1;}
  else if(ref.kind==K7_FIFO_REF_PCM){
   if(!job.have_begin||ref.first_enqueue!=job.reference_next||ref.first_source!=job.ref_source_next||!ref.samples||ref.samples>320u)return -EPROTO;
   unsigned source=0;for(unsigned n=0;n<ref.samples;++n){unsigned zero=(ref.zero_bits[n/8u]>>(n%8u))&1u;
    if(zero){if(ref.pcm[n])return -EPROTO;}else{if(job.ref_source_next+source>=TX_SAMPLES||ref.pcm[n]!=source_sample((unsigned)job.ref_source_next+source))return -EBADMSG;++source;}}
   if(source!=ref.source_samples)return -EPROTO;
   uint64_t relative=ref.first_enqueue-job.begin.first_enqueue;
   if(relative<INPUT_SAMPLES){
    if(relative%320u||ref.samples!=320u)return -EPROTO;
    job.ref_clock[relative/320u]=(struct ref_clock){ref.first_enqueue,ref.before_us,ref.after_us,ref.samples};
   }
   job.reference_next+=ref.samples;job.ref_source_next+=source;
   if(job.begun){struct k7_fifo_ref_status s;struct k7_echo_diag_mic_id m;rc=snapshot(&s,&m);if(!rc)rc=k7_echo_diag_reference(job.pairer,&ref,&s);if(rc)return rc;}
   else{if(job.pending_count==PENDING)return -ENOSPC;job.pending[job.pending_count++]=ref;}
  }else return -EPIPE;
  if((rc=try_begin())||(job.begun&&(rc=process_pairs())))return rc;
 }
 struct k7_voice_frame frame;
 for(unsigned i=0;i<8;++i){
  rc=k7_voice_session_take_kws(job.session,&frame);if(rc<0)return job.full?0:rc;if(!rc)break;
  if(job.full)continue;
  if(frame.flags||frame.samples!=320u||frame.epoch!=job.mic_epoch||frame.first_sample>UINT64_MAX-320u||
     (job.have_mic&&frame.first_sample!=job.mic_next))return -EPIPE;
  job.have_mic=1;job.mic_next=frame.first_sample+320u;
  if(job.begun){struct k7_echo_diag_mic_id m={job.owner,job.mic_epoch};rc=k7_echo_diag_microphone(job.pairer,m,frame.first_sample,frame.pcm,320u);if(rc)return rc;}
  else{if(job.hist_count==PENDING){job.hist_head=(job.hist_head+1u)%PENDING;--job.hist_count;}
   job.history[(job.hist_head+job.hist_count)%PENDING]=frame;++job.hist_count;}
  if((rc=try_begin())||(job.begun&&(rc=process_pairs())))return rc;
 }
 return 0;
}
static int physical_run(void){
 const struct k7_voice_session_config config={.tx_prebuffer_samples=8192,.fifo_reference=true,.timing_anchors=true,.render_reference=false};
 int rc=k7sound_session_start(&config,&job.session);if(rc)return rc;
 if(!job.session)return -EPROTO;
 for(;;){struct k7_voice_session_status s;k7_voice_session_status(job.session,&s);
  if(s.finished)return s.result?s.result:-EIO;
  if(s.ready){if(!s.input_epoch)return -EPROTO;job.mic_epoch=s.input_epoch;break;}
  if((rc=deadline()))return rc;
  pause_ms();
 }
 if((rc=k7_voice_session_tx_begin(job.session,1)))return rc;
 for(;;){
  if((rc=deadline())||(rc=still_owned())||(rc=drain()))return rc;
  struct k7_voice_session_status s;k7_voice_session_status(job.session,&s);
  if(s.finished||!s.running||s.stop_requested||!s.ready)return s.result?s.result:-EPIPE;
  if(s.input_epoch!=job.mic_epoch||s.kws_overflows||s.anchor_overflows||s.anchor_faults)return -EPIPE;
  for(unsigned n=0;n<16&&job.submitted<TX_SAMPLES;++n){
   int16_t pcm[320];unsigned count=TX_SAMPLES-job.submitted;if(count>320u)count=320u;
   for(unsigned i=0;i<count;++i)pcm[i]=source_sample(job.submitted+i);
   rc=k7_voice_session_tx_write(job.session,1,pcm,count);if(rc==-EAGAIN){rc=0;break;}if(rc)return rc;job.submitted+=count;
  }
  if(job.submitted==TX_SAMPLES&&!job.ended){rc=k7_voice_session_tx_end(job.session,1);if(rc)return rc;job.ended=1;}
  struct k7_voice_tx_result result;rc=k7_voice_session_tx_result(job.session,1,&result);
  if(!rc){if(result.result||result.queued_samples!=TX_SAMPLES||result.discarded_words||result.pauses||!job.full)return result.result?result.result:-EPIPE;job.tx_complete=1;return 0;}
  if(rc!=-EAGAIN)return rc;
  pause_ms();
 }
}
static void *worker(void *unused){
 (void)unused;int rc=idle(),release=0;
 if(!rc){rc=vv_speech_owner_native_claim(&speech_token);job.speech_claim=!rc;}
 if(!rc){rc=k7_raw_vad_owner_voice_begin(job.owner);job.raw_claim=!rc;}
 if(!rc){rc=k7boot_aliyun_recorded_quiet_acquire(&speech_token);job.boot_loan=(!rc||rc==-ESTALE);}
 if(!rc)rc=still_owned();
 if(!rc&&(size_t)mallinfo().fordblks<RESERVE_BYTES)rc=-ENOMEM;
 if(!rc)rc=verify_source();
 if(!rc)rc=k7_echo_diag_create(&job.pairer);
 if(!rc){rc=k7_apm21_construct(storage,sizeof(storage),job.run,&job.consumer,&job.upstream);job.constructed=!rc&&job.consumer;}
 if(!rc&&job.upstream)rc=-EPROTO;
 if(!rc&&!job.consumer)rc=-EPROTO;
 job.clock_start=job.clock_last=clock_us();if(!rc&&job.clock_start==UINT64_MAX)rc=-EIO;
 if(!rc)rc=physical_run();
 if(job.session){
  k7_voice_session_stop(job.session);job.join_rc=k7_voice_session_join(job.session);
  k7_voice_session_status(job.session,&job.final_status);
  /* Safe retirement does not erase a failed producer run. join returns the
   * retired run result on success; final status independently retains it. */
  if(!rc&&job.join_rc&&job.join_rc!=-ECANCELED)rc=job.join_rc;
  if(!rc&&job.final_status.result&&job.final_status.result!=-ECANCELED)rc=job.final_status.result;
  if(!job.final_status.finished||!job.final_status.cleanup_ok||job.final_status.cleanup_error)
   release=job.final_status.cleanup_error?job.final_status.cleanup_error:-EIO;
  else{
   /* destroy itself checks the private actual joined bit. This is required
    * even when a failed pthread_join coincides with finished+cleanup_ok. No
    * pairer/APM context retains a session pointer after this success. */
   job.destroy_rc=k7_voice_session_destroy(job.session);release=job.destroy_rc;
   if(!release){job.session=NULL;job.session_destroyed=1;}
  }
 }
 if(!release){
  if(job.consumer){k7_apm21_destroy(job.consumer);job.consumer=NULL;job.destroyed=1;}
  k7_echo_diag_destroy(job.pairer);job.pairer=NULL;
 }
 if(!release&&job.boot_loan)release=k7boot_aliyun_recorded_quiet_release(&speech_token);
 if(!release&&job.raw_claim)release=k7_raw_vad_owner_voice_end(job.owner);
 if(!release&&job.speech_claim)vv_speech_owner_native_release(&speech_token);
 if(release)atomic_store_explicit(&retained,true,memory_order_release);
 job.release=release;job.result=rc?rc:release;job.sealed=1;
 return NULL;
}
static uint32_t rows_crc(unsigned first,unsigned count){
 uint32_t c=UINT32_MAX;
 for(unsigned i=first;i<first+count;++i){const struct half_row *r=&job.row[i];
  const uint64_t fields[]={r->mic_first,r->ref_first,r->ordinal,r->ref_before_us,r->ref_after_us};
  for(unsigned j=0;j<5;++j)for(unsigned b=0;b<8;++b)c=crc_byte(c,(unsigned char)(fields[j]>>(8u*b)));
  for(unsigned b=0;b<4;++b)c=crc_byte(c,(unsigned char)(r->usec>>(8u*b)));
 }
 return c^UINT32_MAX;
}
static void summary(void){
 printf("APM_PHYSICAL_CAPTURE run=%" PRIu64 " sealed=%u full=%u raw_samples=%u api_y_samples=%u halves=%u mic_first=%" PRIu64 " ref_first=%" PRIu64 " mic_crc=%08" PRIx32 " ref_crc=%08" PRIx32 " api_y_crc=%08" PRIx32 " zero_crc=%08" PRIx32 " max_apm_us=%u\n",
 job.run,job.sealed,job.full,job.raw_samples,job.y_samples,job.halves,job.mic_first,job.ref_first,
 job.mic_crc^UINT32_MAX,job.ref_crc^UINT32_MAX,job.y_crc^UINT32_MAX,job.zero_crc^UINT32_MAX,job.apm_max_us);
 printf("APM_PHYSICAL_RESULT rc=%d release_rc=%d submitted=%u tx_complete=%u source_crc_checked=%u construct=%u destroy=%u session_destroyed=%u pio_join_rc=%d session_destroy_rc=%d retained=%u timing=OFFLINE_HYPOTHESIS delay_ms=0 assumed_API_D=128 live_profile=UNKNOWN source_mapping=TX_START_FIFO_HYPOTHESIS qualified=0 VAD=0 ASR=0 cancel_actions=0 natural_barge_accepted=0\n",
 job.result,job.release,job.submitted,job.tx_complete,job.source_checked,job.constructed,job.destroyed,job.session_destroyed,
 job.join_rc,job.destroy_rc,(unsigned)atomic_load_explicit(&retained,memory_order_acquire));
 printf("APM_PHYSICAL_IDENTITY owner=%" PRIu64 " ref_instance=%" PRIu64 " ref_epoch=%u generation=%u mic_epoch=%u tx_start_us_before=%" PRIu64 " tx_start_us_after=%" PRIu64 " tx_start_rx=%" PRIu64 " tx_start_queued=%u anchor_epoch=%u source_samples=%u source_crc=%08" PRIx32 " source_sha256=%s source_repeat_modulo=%u input_limit=240000 padded_samples=0 cross_epoch_append=0\n",
 job.owner,job.begin.instance,job.begin.epoch,job.begin.generation,job.mic_epoch,
 job.start.before_us,job.start.after_us,job.start.rx_samples,job.start.tx_queued_samples,job.start.epoch,
 K7_AB_PCM_SAMPLES,K7_AB_PCM_CRC,K7_AB_PCM_SHA,K7_AB_PCM_SAMPLES);
 printf("APM_PHYSICAL_ROWS count=%u crc=%08" PRIx32 " format=5u64le_u32le field_order=mic_first,ref_first,ordinal,ref_before_us,ref_after_us,usec\n",job.halves,rows_crc(0,job.halves));
}
static void key_string(char out[25]){
 (void)snprintf(out,25,"%016" PRIx64 "%08" PRIx32,job.run,(job.mic_crc^job.ref_crc^job.y_crc)^UINT32_MAX);
}
static int decimal(const char *s,unsigned *out){
 if(!s||!s[0]||(s[0]=='0'&&s[1]))return -EINVAL;
 unsigned n=0;for(;*s;++s){if(*s<'0'||*s>'9'||n>(INPUT_SAMPLES-(unsigned)(*s-'0'))/10u)return -EINVAL;n=n*10u+(unsigned)(*s-'0');}
 *out=n;return 0;
}
static int dump(int argc,char **argv){
 if(argc!=6||!job.sealed||atomic_load(&retained))return -EBUSY;
 char key[25];key_string(key);if(strlen(argv[2])!=24u||strcmp(argv[2],key))return -ESTALE;
 const char *kind=argv[3];unsigned offset,count;
 if(decimal(argv[4],&offset)||decimal(argv[5],&count)||!count)return -EINVAL;
 unsigned bundle=!strcmp(kind,"bundle"),rows=!strcmp(kind,"rows"),zeros=!strcmp(kind,"zero");
 const int16_t *data=!strcmp(kind,"mic")?job.mic:!strcmp(kind,"reference")?job.ref:!strcmp(kind,"api-y")?job.y:NULL;
 unsigned total=rows?job.halves:zeros?job.raw_samples/8u:bundle?(job.raw_samples<job.y_samples?job.raw_samples:job.y_samples):data==job.y?job.y_samples:job.raw_samples;
 if((!bundle&&!rows&&!zeros&&!data)||count>(rows?64u:4096u)||offset>total||count>total-offset)return -EINVAL;
 printf("APM_PHYSICAL_DUMP_BEGIN key=%s kind=%s offset=%u count=%u total=%u run=%" PRIu64 " owner=%" PRIu64 " ref_instance=%" PRIu64 " ref_epoch=%u generation=%u mic_epoch=%u raw_samples=%u api_y_samples=%u qualified=0\n",
  key,kind,offset,count,total,job.run,job.owner,job.begin.instance,job.begin.epoch,job.begin.generation,job.mic_epoch,job.raw_samples,job.y_samples);
 if(rows){
  for(unsigned i=offset;i<offset+count;++i){const struct half_row *p=&job.row[i];
   printf("APM_PHYSICAL_ROW key=%s i=%u ordinal=%" PRIu64 " mic_first=%" PRIu64 " ref_first=%" PRIu64 " ref_block_before_us=%" PRIu64 " ref_block_after_us=%" PRIu64 " process_us=%u\n",
    key,i,p->ordinal,p->mic_first,p->ref_first,p->ref_before_us,p->ref_after_us,p->usec);}
  printf("APM_PHYSICAL_BLOCK key=%s kind=rows offset=%u count=%u crc=%08" PRIx32 "\n",key,offset,count,rows_crc(offset,count));
 }else for(unsigned stream=0;stream<(bundle?3u:1u);++stream){
  const char *name=bundle?(const char*[]){"mic","reference","api-y"}[stream]:kind;
  const int16_t *pcm=bundle?(const int16_t*[]){job.mic,job.ref,job.y}[stream]:data;
  uint32_t crc=UINT32_MAX;
  for(unsigned at=0;at<count;){unsigned n=count-at;if(n>128u)n=128u;
   printf("APM_PHYSICAL_PCM key=%s kind=%s offset=%u count=%u data=",key,name,offset+at,n);
   for(unsigned j=0;j<n;++j){unsigned i=offset+at+j;
    if(zeros){unsigned char v=job.zero[i];printf("%02x",v);crc=crc_byte(crc,v);}
    else{uint16_t v=(uint16_t)pcm[i];printf("%02x%02x",v&255u,v>>8);crc=crc_byte(crc_byte(crc,(unsigned char)v),(unsigned char)(v>>8));}}
   putchar('\n');at+=n;
  }
  printf("APM_PHYSICAL_BLOCK key=%s kind=%s offset=%u count=%u crc=%08" PRIx32 "\n",key,name,offset,count,crc^UINT32_MAX);
 }
 printf("APM_PHYSICAL_DUMP_END key=%s kind=%s offset=%u count=%u streams=%u run=%" PRIu64 " ref_instance=%" PRIu64 " ref_epoch=%u mic_epoch=%u\n",
 key,kind,offset,count,bundle?3u:rows?0u:1u,job.run,job.begin.instance,job.begin.epoch,job.mic_epoch);
 return 0;
}
int k7_apm21_fixed_physical_command(int argc,char **argv){
 if(argc<2)return 1;
 if(argc==2&&!strcmp(argv[1],"apm-fixed-stop")){atomic_store(&stop_requested,true);return 0;}
 if(atomic_flag_test_and_set_explicit(&command_busy,memory_order_acquire))return 1;
 if(argc==2&&!strcmp(argv[1],"apm-fixed-status")){
  summary();char key[25];key_string(key);printf("APM_PHYSICAL_SEALED key=%s sealed=%u raw_samples=%u api_y_samples=%u\n",key,job.sealed,job.raw_samples,job.y_samples);
  atomic_flag_clear_explicit(&command_busy,memory_order_release);return 0;
 }
 if(!strcmp(argv[1],"apm-fixed-dump")){
  int rc=dump(argc,argv);atomic_flag_clear_explicit(&command_busy,memory_order_release);return rc?1:0;
 }
 if(argc==2&&!strcmp(argv[1],"apm-fixed-clear")){
  if(job.session||job.consumer||job.pairer||atomic_load(&retained)){atomic_flag_clear(&command_busy);return 1;}
  memset(&job,0,sizeof(job));atomic_flag_clear_explicit(&command_busy,memory_order_release);return 0;
 }
 if(argc!=2||strcmp(argv[1],"apm-fixed-physical")||job.sealed||sequence>=UINT64_MAX-UINT64_C(0x41504d0000000000)){
  atomic_flag_clear_explicit(&command_busy,memory_order_release);return 1;
 }
 memset(&job,0,sizeof(job));job.run=++sequence;job.owner=UINT64_C(0x41504d0000000000)+job.run;
 job.mic_crc=job.ref_crc=job.y_crc=job.zero_crc=UINT32_MAX;
 atomic_store(&stop_requested,false);
 pthread_attr_t attr;pthread_t thread;int setup=pthread_attr_init(&attr),attr_release=0,joined=0;
 unsigned initialized=!setup,created=0;
 if(!setup)setup=pthread_attr_setstacksize(&attr,256u*1024u);
 if(!setup)setup=pthread_attr_setinheritsched(&attr,PTHREAD_INHERIT_SCHED);
 if(!setup)setup=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE);
 if(!setup){setup=pthread_create(&thread,&attr,worker,NULL);created=!setup;}
 if(initialized)attr_release=pthread_attr_destroy(&attr);
 if(created)joined=pthread_join(thread,NULL);
 if(attr_release||joined)atomic_store(&retained,true);
 if(created&&!joined)summary();
 printf("APM_PHYSICAL_WORKER setup_rc=%d attr_cleanup_rc=%d join_rc=%d created=%u joined=%u requested_stack_bytes=262144 inherit_sched=1 affinity_set_calls=0 job_bytes=%zu retained=%u\n",
 setup,attr_release,joined,created,created&&!joined,sizeof(job),(unsigned)atomic_load(&retained));
 if(!atomic_load(&retained))atomic_flag_clear_explicit(&command_busy,memory_order_release);
 return setup||attr_release||joined||job.result?1:0;
}
#endif
