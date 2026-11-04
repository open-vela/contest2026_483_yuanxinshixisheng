#if defined(K7_ECHO_AEC10_ADAPTER) && K7_ECHO_AEC10_ADAPTER && (!K7_NEAR_EXPERIMENT || !K7_NEAR_MULTIROUND || !K7_ECHO_CANCEL_CONTINUATION || !K7_ECHO_RUNTIME_CONTINUITY || !K7_ECHO_RESIDUAL_EVIDENCE || !K7_ECHO_NEAR_ADAPTER)
#error AEC10 integration requires the full existing experimental CC/NE worker
#endif
#line 1
#if (defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE) || (defined(K7_ECHO_NEAR_ADAPTER) && K7_ECHO_NEAR_ADAPTER) || (defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION) || (defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT)
#include "echo_diag_runtime.h"
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#include "k7_apm21_live_wiring.h"
#if !K7_ECHO_RUNTIME_CONTINUITY || !K7_NEAR_EXPERIMENT || !K7_NEAR_MULTIROUND
#error APM live observer requires the existing full near worker
#endif
#endif
#include "echo_diag_worker.h"
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
#include "k7_apm21_evidence_bridge.h"
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#include "k7_apm21_joint.h"
#endif
#endif
#line 10
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
#include "k7_apm21_prepared_lifecycle.h"
#if !defined(K7_ECHO_RESIDUAL_EVIDENCE) || !K7_ECHO_RESIDUAL_EVIDENCE || !defined(K7_ECHO_RAW_VAD_OBSERVER) || !K7_ECHO_RAW_VAD_OBSERVER
#error prepared lifecycle requires the exclusive existing residual model owner
#endif
#include "vv_voice_session.h"
#endif
#line 10
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
#include "cancel_continuation.h"
#if !K7_ECHO_RESIDUAL_EVIDENCE || !K7_ECHO_NEAR_ADAPTER
#error cancel continuation requires residual owner and observer adapter
#endif
#endif
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
#include "near_evidence.h"
#if defined(K7_ECHO_NEAR_ADAPTER) && K7_ECHO_NEAR_ADAPTER
#include "echo_observer.h"
#endif
#if !defined(K7_ECHO_RAW_VAD_OBSERVER) || !K7_ECHO_RUNTIME_CONTINUITY
#error residual evidence requires model owner and continuity
#endif
#endif
#include "k7_aec.h"
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include "diag_bridge.h"
#include "k7_echo_reply_identity.h"
#endif

#ifdef K7_ECHO_RAW_VAD_OBSERVER
#include "raw_vad_observer.h"
#endif
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif

#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
#if !K7_NEAR_EXPERIMENT || !K7_NEAR_MULTIROUND || !K7_ECHO_CANCEL_CONTINUATION || !K7_ECHO_RUNTIME_CONTINUITY
#error freeze trace requires the complete current experimental multiround runtime
#endif
#endif
#define CAPTURE_FRAMES 1500u
#define CAPTURE_SAMPLES (CAPTURE_FRAMES*320u)
#define ANCHORS 4096u
#define HISTORY 64u
#define TAIL_PRE 5120u
#define TAIL_POST 16000u
#define TAIL_SAMPLES (67u*320u)
enum { TAIL_READY=0,TAIL_ARMED,TAIL_ACTIVE,TAIL_FROZEN };
enum { READY=0, ARMED, ACTIVE, FROZEN };

#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
#if !K7_NEAR_EXPERIMENT || !K7_NEAR_MULTIROUND || !K7_ECHO_CANCEL_CONTINUATION || !K7_ECHO_RUNTIME_CONTINUITY
#error paired capture requires the current experimental multiround CC runtime
#endif
enum { PC_EMPTY=0,PC_WRITING,PC_SEALED };
enum { PC_CANDIDATE=1,PC_FULL,PC_FREEZE,PC_ROUND_END,PC_DISCONTINUITY };
struct paired_capture_row {
 uint64_t mic_first,ref_first,serial,window,window_first,window_next;
 double mic_power,ref_power,res_power,correlation2;
 float probability;int model_rc;
 unsigned source_stop_zeros,consecutive,candidate,clipped;
};
_Static_assert(sizeof(float)==4&&sizeof(double)==8,"capture numeric export widths");
#endif

enum { CMD_NONE=0,CMD_ARM,CMD_ABORT,CMD_CLEAR,CMD_TAIL_ARM,CMD_TAIL_CLEAR
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 ,CMD_VAD_STATUS
#endif
 };
struct k7_echo_runtime {
 struct k7_voice_session *session; uint64_t owner;
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 struct k7_apm21_evidence_bridge apm21_evidence;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 struct k7_apm21_joint apm21_joint;
 struct k7_voice_anchor apm21_unclaimed_clear;
 atomic_uint apm21_prompt_release,apm21_prompt_retired;
 atomic_int apm21_prompt_error;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 struct cc_pair apm21_source_pending;
 unsigned apm21_source_wait,apm21_source_cancel;
 struct k7_fifo_ref_record apm21_ref_pending;
 unsigned apm21_ref_wait,apm21_have_physical_cut;
 struct k7_apm21_cut_descriptor apm21_physical_cut;
 struct k7_apm21_idle_event apm21_idle_pending;
 unsigned apm21_idle_wait,apm21_eof_verified;
#endif
#endif
 bool apm21_diag_error_logged;
#endif
#line 84
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 /* Control owns command publication; worker owns APM/session attachment. */
 unsigned preopened;
 atomic_uint prepared_phase;
 atomic_int prepared_result;
 struct k7_voice_session *attach_session;
 struct k7_apm21_live_binding prepared_binding;
#endif
#line 84
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
 struct k7_apm21_live_worker apm21_live;
#endif
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
 struct ne_worker *near; bool residual_mode;
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
 struct cancel_continuation *cc;
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 bool lag_primed;int16_t lag_pcm[39];unsigned char lag_zero[39];
#endif
#endif
 uint64_t evidence_windows,processed_frames;unsigned capture_anchor_overflow;
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 uint64_t round_candidates;
 float round_peak;bool round_reported;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT && defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 /* Bridge counters are session cumulative; these are the real round bases. */
 uint64_t apm21_round_frames_base,apm21_round_windows_base,apm21_round_candidates_base;
#endif
#endif
#endif
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 struct k7_raw_vad *vad;
 k7_raw_vad_sink vad_sink;void *vad_context;
 struct k7_raw_vad_stats vad_reply;
 unsigned vad_rx_faults;uint64_t vad_tap_rejected;
#endif
 pthread_t thread; bool joined;
 atomic_bool stop,done;
 atomic_uint state,command,frames,rx_faults;
 atomic_int command_result;
 atomic_uint tail_state,tail_samples;
 uint32_t tail_epoch,tail_floor,tail_generation,tail_anchor_epoch;unsigned tail_faults;
 uint64_t tail_first,tail_next,tail_end,tail_boundary,tail_before_us,tail_after_us;
 int tail_reason;bool tail_complete;
 int16_t tail[TAIL_SAMPLES];
 struct k7_voice_ring rx;
 struct k7_echo_diag *pairer;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 struct diag_bridge *bridge;
 struct k7_echo_reply_identity reply;
 struct vv_confirmed_wake_context context;
 struct k7_aec_continuity_identity continuity;
 uint64_t bridge_lease,processed_mic_next,processed_ref_next,cancel_mic_next,cancel_ref_next;
#if defined(K7_ECHO_AEC10_ADAPTER) && K7_ECHO_AEC10_ADAPTER
 /* Input accepted watermark and output nominal watermark are independent. */
 uint64_t published_mic_next,published_ref_next;
 struct diag_bridge_paired_output aec10_output;
#endif
#line 115
 unsigned seals;
#else
 struct k7_aec *aec;
#endif

 struct k7_voice_frame history[HISTORY]; unsigned head,count;
 uint32_t history_epoch,anchor_epoch,floor,generation,ref_epoch;
 unsigned faults_at_arm,anchor_count; int finish_reason;
 uint64_t instance,mic_first,fifo_first,zero_samples;
 struct k7_voice_anchor start,anchors[ANCHORS]; bool have_start,pending_begin;
 struct k7_fifo_ref_record begin;

#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
 /* Private worker-only numeric evidence; no public ABI or borrowed pointers. */
 struct {
  unsigned poll_code,poll_valid;int poll_rc;
  uint32_t poll_clock;bool poll_clock_valid;
  struct k7_echo_reply_identity poll_reply;
  struct k7_voice_session_status poll_session;
  struct k7_voice_anchor anchor;bool anchor_valid;
  uint64_t mic_first;uint32_t mic_epoch;unsigned mic_samples,mic_flags;
  uint64_t ref_instance,ref_first,ref_source,ref_before,ref_after;
  uint32_t ref_epoch,ref_generation,ref_kind,ref_faults,ref_discarded;
  unsigned ref_samples;int ref_result;bool ref_valid;
 } trace;
#endif
#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
 /* Independent immutable metadata; existing PCM arrays are unused in CC. */
 struct {
  atomic_uint state;struct ne_identity id;
  unsigned frames,reason;int fault;
  uint64_t mic_first,mic_next,ref_first,ref_next,first_serial,next_serial,zeros;
  struct paired_capture_row row[CAPTURE_FRAMES];
 } capture;
#endif
 int16_t mic[CAPTURE_SAMPLES],reference[CAPTURE_SAMPLES],residual[CAPTURE_SAMPLES];
 uint8_t zeros[CAPTURE_SAMPLES/8];
};
static pthread_mutex_t control=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool requested=ATOMIC_VAR_INIT(false);
static struct k7_echo_runtime *current;
static void pause_ms(void){struct timespec t={0,1000000};(void)nanosleep(&t,NULL);}
bool k7_echo_runtime_requested(void){return atomic_load(&requested);}
static unsigned state(struct k7_echo_runtime *s){return atomic_load_explicit(&s->state,memory_order_acquire);}
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
/* Worker-only counters from the real residual ne_feed path. RAW-only observer
 * statistics do not describe this model. No shared header/status ABI changes. */
static void report_round(struct k7_echo_runtime*s,int reason){
 if(s->round_reported||!s->generation)return;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT && defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 if(s->preopened&&s->prepared_binding.product_experiment==1){
  const struct k7_apm21_evidence_bridge *b=&s->apm21_evidence;
  printf("NEAR_EXPERIMENT round_summary reply=%llu tx=%u processed=%llu residual_windows=%llu max_p=%.6f candidate_windows=%llu cancel_observed=%u freeze_reason=%d calibrated=0 path=apm21\n",
   (unsigned long long)s->reply.reply,s->generation,
   (unsigned long long)(b->frames-s->apm21_round_frames_base),
   (unsigned long long)(b->windows-s->apm21_round_windows_base),(double)s->round_peak,
   (unsigned long long)(b->candidates-s->apm21_round_candidates_base),s->reply.cancelled,reason);
  s->round_reported=true;return;
 }
#endif
 printf("NEAR_EXPERIMENT round_summary reply=%llu tx=%u processed=%llu residual_windows=%llu max_p=%.6f candidate_windows=%llu cancel_observed=%u freeze_reason=%d calibrated=0\n",
  (unsigned long long)s->reply.reply,s->generation,
  (unsigned long long)s->processed_frames,
  (unsigned long long)s->evidence_windows,(double)s->round_peak,
  (unsigned long long)s->round_candidates,s->reply.cancelled,reason);
 s->round_reported=true;
}
#endif

#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
/* Worker is the sole writer. SEALED release publishes all POD metadata and
 * PCM; it is never reset by ARM/CLEAR/CMD100 or by session close. */
static unsigned capture_state(const struct k7_echo_runtime*s){
 return atomic_load_explicit(&s->capture.state,memory_order_acquire);
}
static void capture_seal(struct k7_echo_runtime*s,unsigned reason,int fault){
 if(capture_state(s)!=PC_WRITING)return;
 s->capture.reason=reason;s->capture.fault=fault;
 atomic_store_explicit(&s->capture.state,PC_SEALED,memory_order_release);
}
#endif

static void freeze(struct k7_echo_runtime *s,int why){
 unsigned st=state(s);if(st!=ACTIVE&&st!=ARMED)return;
#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
 capture_seal(s,PC_FREEZE,why);
#endif
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 report_round(s,why);
#endif
 k7_echo_diag_invalidate(s->pairer);
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
 ne_invalidate(s->near);
#if defined(K7_ECHO_NEAR_ADAPTER) && K7_ECHO_NEAR_ADAPTER
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 if(s->preopened)k7_apm21_evidence_invalidate(&s->apm21_evidence,why?why:-EPIPE);
 else
#endif
 k7_echo_observer_emit(NULL,why?why:-EPIPE);
#line 199
#endif
#endif
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 (void)diag_bridge_invalidate(s->bridge,why);
#else
 (void)k7_aec_invalidate(s->aec,K7_AEC_BAD_REFERENCE);
#endif

 s->pending_begin=false;s->finish_reason=why;
 atomic_store_explicit(&s->state,FROZEN,memory_order_release);
}

#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
/* Only called by the worker, outside the mailbox mutex. Snapshot BEFORE
 * invalidation, emit AFTER the original freeze completes. Each ARMED/ACTIVE
 * transition can emit once; repeated faults in FROZEN emit nothing. These
 * separately sampled values are observational, never an authority check. */
static void trace_reply(const char*label,const struct k7_echo_reply_identity*v){
 printf("ECHO_FREEZE_REPLY label=%s instance=%llu revision=%llu scope=%llu lease=%llu owner=%llu reply=%llu asr=%u input=%u tx=%u live=%u bound=%u cancelled=%u retired=%u cancel_us=%llu\n",
  label,(unsigned long long)v->instance,(unsigned long long)v->revision,
  (unsigned long long)v->scope_incarnation,(unsigned long long)v->lease_serial,
  (unsigned long long)v->owner,(unsigned long long)v->reply,v->asr,v->input,v->tx,
  v->live,v->bound,v->cancelled,v->retired_terminal,(unsigned long long)v->cancel_observed_us);
}
static void trace_session(const char*label,const struct k7_voice_session_status*v){
 printf("ECHO_FREEZE_SESSION label=%s ready=%u running=%u finished=%u stop_requested=%u result=%d mic_next=%llu input=%u asr=%u asr_failed=%u anchor_epoch=%u anchor_overflows=%u anchor_faults=%u tx_state=%u tx=%u pauses=%u kws_overflows=%u asr_overflows=%u render_overflows=%u cleanup_ok=%u cleanup_error=%d\n",
  label,v->ready,v->running,v->finished,v->stop_requested,v->result,
  (unsigned long long)v->microphone_samples,v->input_epoch,v->asr_epoch,v->asr_failed_epoch,
  v->anchor_epoch,v->anchor_overflows,v->anchor_faults,(unsigned)v->tx_state,
  v->tx_generation,v->tx_pauses,v->kws_overflows,v->asr_overflows,v->render_overflows,
  v->cleanup_ok,v->cleanup_error);
}
static void freeze_at(struct k7_echo_runtime*s,int why,unsigned site){
 unsigned st=state(s);if(st!=ACTIVE&&st!=ARMED)return;
 bool pending=s->pending_begin;
 struct k7_voice_session_status v={0};struct k7_fifo_ref_status f={0};
 struct k7_echo_reply_identity live={0};
 k7_voice_session_status(s->session,&v);
 int fr=k7_voice_session_fifo_status(s->session,&f),rr=k7_echo_reply_snapshot(&live);
 freeze(s,why);
 printf("ECHO_FREEZE site=%u reason=%d state=%u owner=%llu generation=%u floor=%u history_epoch=%u history_count=%u rx_faults=%u arm_rx_faults=%u pending_begin=%u have_start=%u anchor_count=%u bridge=%u cc=%u mic_next=%llu ref_next=%llu cancel_mic_next=%llu cancel_ref_next=%llu seals=%u tail_state=%u tail_reason=%d poll_code=%u poll_rc=%d poll_valid=%u poll_clock_valid=%u poll_clock=%u\n",
  site,why,st,(unsigned long long)s->owner,s->generation,s->floor,s->history_epoch,s->count,
  atomic_load(&s->rx_faults),s->faults_at_arm,pending,s->have_start,s->anchor_count,
  s->bridge_lease!=0,s->cc!=NULL,(unsigned long long)s->processed_mic_next,
  (unsigned long long)s->processed_ref_next,(unsigned long long)s->cancel_mic_next,
  (unsigned long long)s->cancel_ref_next,s->seals,atomic_load(&s->tail_state),s->tail_reason,
  s->trace.poll_code,s->trace.poll_rc,s->trace.poll_valid,s->trace.poll_clock_valid,s->trace.poll_clock);
 trace_session("freeze",&v);trace_reply("cached",&s->reply);
 printf("ECHO_FREEZE_LIVE snapshot_rc=%d fifo_rc=%d fifo_instance=%llu fifo_epoch=%u fifo_generation=%u fifo_faults=%u fifo_eligible=%u fifo_exhausted=%u clock_expected=%u ref_expected=%llu ref_epoch_expected=%llu\n",
  rr,fr,(unsigned long long)f.instance,f.epoch,f.generation,f.faults,f.eligible,f.exhausted,
  (unsigned)s->continuity.clock_epoch,(unsigned long long)s->continuity.reference_instance,
  (unsigned long long)s->continuity.reference_epoch);
 if(!rr)trace_reply("freeze_live",&live);
 if(s->trace.poll_valid&1u)trace_reply("poll_failure",&s->trace.poll_reply);
 if(s->trace.poll_valid&2u)trace_session("poll_failure",&s->trace.poll_session);
 const struct k7_voice_anchor*a=&s->trace.anchor;
 printf("ECHO_FREEZE_ANCHOR valid=%u event=%u generation=%u epoch=%u before_us=%llu after_us=%llu rx=%llu tx_queued=%u rx_words=%u tx_words=%u discarded=%u flags=%u result=%d start_generation=%u start_epoch=%u start_rx=%llu\n",
  s->trace.anchor_valid,a->event,a->generation,a->epoch,(unsigned long long)a->before_us,
  (unsigned long long)a->after_us,(unsigned long long)a->rx_samples,a->tx_queued_samples,
  a->rx_fifo_words,a->tx_fifo_words,a->discarded_words,a->flags,a->result,
  s->start.generation,s->start.epoch,(unsigned long long)s->start.rx_samples);
 printf("ECHO_FREEZE_INPUT mic_first=%llu mic_epoch=%u mic_samples=%u mic_flags=%u ref_valid=%u ref_instance=%llu ref_epoch=%u ref_generation=%u ref_kind=%u ref_first=%llu ref_source=%llu ref_samples=%u ref_before_us=%llu ref_after_us=%llu ref_faults=%u ref_discarded=%u ref_result=%d begin_instance=%llu begin_epoch=%u begin_generation=%u begin_first=%llu\n",
  (unsigned long long)s->trace.mic_first,s->trace.mic_epoch,s->trace.mic_samples,s->trace.mic_flags,
  s->trace.ref_valid,(unsigned long long)s->trace.ref_instance,s->trace.ref_epoch,
  s->trace.ref_generation,s->trace.ref_kind,(unsigned long long)s->trace.ref_first,
  (unsigned long long)s->trace.ref_source,s->trace.ref_samples,
  (unsigned long long)s->trace.ref_before,(unsigned long long)s->trace.ref_after,
  s->trace.ref_faults,s->trace.ref_discarded,s->trace.ref_result,
  (unsigned long long)s->begin.instance,s->begin.epoch,s->begin.generation,
  (unsigned long long)s->begin.first_enqueue);
}
#define FREEZE_AT(s,why,site) freeze_at(s,why,site)
static int poll_fail(struct k7_echo_runtime*s,unsigned code,int rc,
 const struct k7_echo_reply_identity*v,const struct k7_voice_session_status*st){
 s->trace.poll_code=code;s->trace.poll_rc=rc;s->trace.poll_valid=(v?1u:0u)|(st?2u:0u);
 if(v)s->trace.poll_reply=*v;
 if(st)s->trace.poll_session=*st;
 return rc;
}
#define POLL_FAIL(code,rc,v,st) poll_fail(s,code,rc,v,st)
static uint32_t poll_clock(struct k7_echo_runtime*s){
 s->trace.poll_clock=k7_voice_session_anchor_epoch(s->session);s->trace.poll_clock_valid=true;
 return s->trace.poll_clock;
}
#define POLL_CLOCK(s) poll_clock(s)
#endif

/* Independent RAW snapshot. No render zeros, AEC reset or clean authority.
 * Worker owns all non-atomic fields; release FROZEN publishes immutable data. */
static unsigned tail_state(struct k7_echo_runtime*s){return atomic_load_explicit(&s->tail_state,memory_order_acquire);}
static void tail_freeze(struct k7_echo_runtime*s,int why){
 unsigned st=tail_state(s);if(st!=TAIL_ARMED&&st!=TAIL_ACTIVE)return;
 s->tail_reason=why;s->tail_complete=!why&&s->tail_next==s->tail_end&&atomic_load(&s->tail_samples)>=TAIL_PRE+TAIL_POST;
 atomic_store_explicit(&s->tail_state,TAIL_FROZEN,memory_order_release);
}
static void tail_frame(struct k7_echo_runtime*s,const struct k7_voice_frame*f){
 unsigned st=tail_state(s);if(st!=TAIL_ARMED&&st!=TAIL_ACTIVE)return;
 if(f->samples!=320||f->flags||f->epoch!=s->tail_epoch||f->first_sample>UINT64_MAX-320){tail_freeze(s,-EPIPE);return;}
 if(st!=TAIL_ACTIVE)return;
 if(f->first_sample+320<=s->tail_next)return; /* already copied from history */
 if(f->first_sample!=s->tail_next||s->tail_next>s->tail_end||s->tail_end-s->tail_next<320){tail_freeze(s,-EPIPE);return;}
 unsigned n=atomic_load(&s->tail_samples);
 if(n>TAIL_SAMPLES-320){tail_freeze(s,-ENOSPC);return;}
 memcpy(s->tail+n,f->pcm,320*sizeof(int16_t));s->tail_next+=320;
 atomic_store(&s->tail_samples,n+320);
 if(s->tail_next==s->tail_end){
  /* Anchors are drained before each MIC batch. An actual new TX/input epoch
   * can already exist while its queued event has not been consumed. Recheck
   * the pinned owner immediately before committing a successful snapshot.
   * A change later than the last sample may conservatively reject RAW. */
  struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
  if(atomic_load(&s->rx_faults)!=s->tail_faults)tail_freeze(s,-ENOSPC);
  else if(!v.ready||!v.running||v.finished||v.stop_requested||
          v.input_epoch!=s->tail_epoch||v.tx_generation!=s->tail_generation||
          v.anchor_epoch!=s->tail_anchor_epoch)tail_freeze(s,-EPIPE);
  else tail_freeze(s,0);
 }
}
static void tail_anchor(struct k7_echo_runtime*s,const struct k7_voice_anchor*a){
 unsigned st=tail_state(s);if(st!=TAIL_ARMED&&st!=TAIL_ACTIVE)return;
 if(a->flags||a->after_us<a->before_us||a->epoch!=k7_voice_session_anchor_epoch(s->session)){tail_freeze(s,-EPIPE);return;}
 if(st==TAIL_ACTIVE){
  if(a->event==K7_VOICE_ANCHOR_AMP_OFF&&a->result){tail_freeze(s,-EPIPE);return;}
  if(a->event==K7_VOICE_ANCHOR_TX_START&&a->generation!=s->tail_generation)tail_freeze(s,-EPIPE);
  return;
 }
 if(a->event!=K7_VOICE_ANCHOR_AMP_OFF||a->generation<=s->tail_floor)return;
 if(a->result||a->generation!=k7_voice_session_tx_generation(s->session)||
   k7_voice_session_input_epoch(s->session)!=s->tail_epoch){tail_freeze(s,-EPIPE);return;}
 uint64_t base=a->rx_samples-a->rx_samples%320;
 if(base<TAIL_PRE||a->rx_samples>UINT64_MAX-TAIL_POST-319){tail_freeze(s,-ENODATA);return;}
 uint64_t first=base-TAIL_PRE,end=((a->rx_samples+TAIL_POST+319)/320)*320;
 if(end<first||end-first>TAIL_SAMPLES||!s->count||s->history_epoch!=s->tail_epoch){tail_freeze(s,-ENODATA);return;}
 const struct k7_voice_frame*old=&s->history[s->head];
 const struct k7_voice_frame*last=&s->history[(s->head+s->count-1)%HISTORY];
 if(first<old->first_sample||first>last->first_sample){tail_freeze(s,-ENODATA);return;}
 s->tail_generation=a->generation;s->tail_first=s->tail_next=first;s->tail_end=end;
 s->tail_anchor_epoch=a->epoch;s->tail_boundary=a->rx_samples;s->tail_before_us=a->before_us;s->tail_after_us=a->after_us;
 atomic_store(&s->tail_samples,0);atomic_store_explicit(&s->tail_state,TAIL_ACTIVE,memory_order_release);
 for(unsigned i=0;i<s->count&&tail_state(s)==TAIL_ACTIVE;i++){
  const struct k7_voice_frame*f=&s->history[(s->head+i)%HISTORY];
  if(f->first_sample>=first)tail_frame(s,f);
 }
}
static int snapshot(struct k7_echo_runtime *s,struct k7_fifo_ref_status *r,
 struct k7_echo_diag_mic_id *m){
 struct k7_voice_session_status st; k7_voice_session_status(s->session,&st);
 if(!st.ready||!st.running||st.finished||st.stop_requested||!st.input_epoch)return -EPIPE;
 int rc=k7_voice_session_fifo_status(s->session,r);if(rc)return rc;
 *m=(struct k7_echo_diag_mic_id){s->owner,st.input_epoch};return 0;
}
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
/* Worker-only: mailbox lock is released inside snapshot before ANY session
 * access or DSP. No read-only identity can grant clean/model/alignment proof. */
static bool reply_same(const struct k7_echo_reply_identity*a,const struct k7_echo_reply_identity*b){
 return a->instance==b->instance&&a->scope_incarnation==b->scope_incarnation&&
 a->lease_serial==b->lease_serial&&a->owner==b->owner&&a->reply==b->reply&&
 a->asr==b->asr&&a->input==b->input&&a->tx==b->tx;
}
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
static struct ne_identity near_identity(struct k7_echo_runtime*s){
 return (struct ne_identity){.owner=s->owner,.reference_instance=s->continuity.reference_instance,
 .reference_epoch=s->continuity.reference_epoch,.clock_epoch=s->continuity.clock_epoch,
 .dialogue_instance=s->reply.instance,.scope=s->reply.scope_incarnation,.lease=s->reply.lease_serial,
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 .reply=s->reply.reply,.mapping_revision=s->preopened&&s->prepared_binding.product_experiment==1?K7_APM21_NOMINAL_REVISION:s->continuity.mapping_epoch,
#else
 .reply=s->reply.reply,.mapping_revision=s->continuity.mapping_epoch,
#endif
 .input_epoch=s->continuity.input_epoch,.tx_generation=s->continuity.render_generation};
}
#endif

#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
static bool capture_same(const struct ne_identity*a,const struct ne_identity*b){
#define PC_EQ(n) a->n==b->n
 return PC_EQ(owner)&&PC_EQ(reference_instance)&&PC_EQ(reference_epoch)&&PC_EQ(clock_epoch)&&
  PC_EQ(dialogue_instance)&&PC_EQ(scope)&&PC_EQ(lease)&&PC_EQ(reply)&&PC_EQ(mapping_revision)&&
  PC_EQ(input_epoch)&&PC_EQ(tx_generation);
#undef PC_EQ
}
/* Copy only already computed genuine PCM. Any diagnostic mismatch seals this
 * prefix and returns; it never freezes the runtime or changes an authority. */
static unsigned capture_frame(struct k7_echo_runtime*s,const struct cc_pair*p,
 const struct k7_voice_frame*res,const unsigned char zero[320]){
 unsigned cs=capture_state(s);if(cs==PC_SEALED)return UINT_MAX;
 struct ne_identity id=near_identity(s);
 if(cs==PC_EMPTY){
  s->capture.id=id;s->capture.mic_first=s->capture.mic_next=p->microphone_first;
  s->capture.ref_first=s->capture.ref_next=p->reference_first;
  s->capture.first_serial=s->capture.next_serial=s->processed_frames;
  atomic_store_explicit(&s->capture.state,PC_WRITING,memory_order_release);
 }
 unsigned frame=s->capture.frames;
 if(frame>=CAPTURE_FRAMES){capture_seal(s,PC_FULL,0);return UINT_MAX;}
 if(!capture_same(&id,&s->capture.id)||p->microphone_first!=s->capture.mic_next||
    p->reference_first!=s->capture.ref_next||s->processed_frames!=s->capture.next_serial||
    p->microphone_first>UINT64_MAX-320||p->reference_first>UINT64_MAX-320||
    s->processed_frames==UINT64_MAX){capture_seal(s,PC_DISCONTINUITY,-EPIPE);return UINT_MAX;}
 struct paired_capture_row*r=&s->capture.row[frame];
 *r=(struct paired_capture_row){.mic_first=p->microphone_first,.ref_first=p->reference_first,
  .serial=s->processed_frames,.model_rc=INT_MIN};
 memcpy(s->mic+frame*320,p->microphone,sizeof(p->microphone));
 memcpy(s->reference+frame*320,p->reference,sizeof(p->reference));
 memcpy(s->residual+frame*320,res->pcm,sizeof(res->pcm));
 memset(s->zeros+frame*40,0,40);
 for(unsigned i=0;i<320;i++)if(zero[i]){s->zeros[frame*40+i/8]|=(uint8_t)(1u<<(i%8));r->source_stop_zeros++;}
 s->capture.zeros+=r->source_stop_zeros;s->capture.frames=frame+1;
 s->capture.mic_next=p->microphone_first+320;s->capture.ref_next=p->reference_first+320;
 s->capture.next_serial=s->processed_frames+1;return frame;
}
static void capture_model(struct k7_echo_runtime*s,unsigned frame,int nr,const struct ne_report*r){
 if(frame==UINT_MAX||capture_state(s)!=PC_WRITING||frame>=s->capture.frames)return;
 struct paired_capture_row*out=&s->capture.row[frame];out->model_rc=nr;
 if(nr>0){out->window=r->model.window_id;out->window_first=r->model.input_first_sample;
  out->window_next=r->model.input_end_sample;out->probability=r->model.probability;
  out->mic_power=r->microphone_power;out->ref_power=r->reference_power;
  out->res_power=r->residual_power;out->correlation2=r->correlation2;
  out->consecutive=r->consecutive;out->candidate=r->candidate;out->clipped=r->clipped;
  if(r->candidate){capture_seal(s,PC_CANDIDATE,0);return;}
 }
 if(s->capture.frames==CAPTURE_FRAMES)capture_seal(s,PC_FULL,0);
}
#endif

static int continuity_poll(struct k7_echo_runtime*s){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
 s->trace.poll_code=0;s->trace.poll_rc=0;s->trace.poll_valid=0;s->trace.poll_clock_valid=false;
#endif
 if(state(s)!=ACTIVE)return 0;
 struct k7_echo_reply_identity v;int rc=k7_echo_reply_snapshot(&v);if(rc)return 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(1,rc,NULL,NULL)
#else
rc
#endif
;
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
 /* A retired cancelled reply may leave its original lineage while RX lives.
  * A different live reply is a new stream, never accepted as tail. */
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 bool retired=!v.live&&v.retired_terminal&&v.cancelled&&v.bound&&reply_same(&v,&s->reply);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 /* speaker_end and reply_end each retire this same mailbox. Either one or
  * both may publish before this poll; no cancellation or new identity may
  * be hidden by these two terminal-only revisions. Real RX/clock follows. */
 bool retired_eof=s->preopened&&s->prepared_binding.product_experiment==1&&
  s->reply.live&&s->reply.bound&&!s->reply.retired_terminal&&
  !v.live&&v.retired_terminal&&!v.cancelled&&!s->reply.cancelled&&v.bound&&
  !v.cancel_observed_us&&!s->reply.cancel_observed_us&&reply_same(&v,&s->reply)&&
  v.revision>s->reply.revision&&v.revision-s->reply.revision<=2;
 retired=retired||retired_eof;
#endif
#else
 bool retired=s->reply.cancelled&&!v.live;
#endif
 if(!retired&&(!v.live||!v.bound||!reply_same(&v,&s->reply)))return 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(2,-EPIPE,&v,NULL)
#else
-EPIPE
#endif
;
#else
 if(!v.live||!v.bound||!reply_same(&v,&s->reply))return 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(2,-EPIPE,&v,NULL)
#else
-EPIPE
#endif
;
#endif
 /* Recheck real RX/reference before recording even a software boundary. */
 struct k7_voice_session_status st;k7_voice_session_status(s->session,&st);
 if(!st.ready||!st.running||st.finished||st.stop_requested||st.input_epoch!=s->reply.input||
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
 st.tx_generation!=s->reply.tx||
#else
 !k7_voice_session_fifo_matches(s->session,s->continuity.reference_instance,
  (uint32_t)s->continuity.reference_epoch,v.tx)||
#endif
 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_CLOCK(s)
#else
k7_voice_session_anchor_epoch(s->session)
#endif
!=s->continuity.clock_epoch)return 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(3,-EPIPE,&v,&st)
#else
-EPIPE
#endif
;
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 if(retired&&s->reply.cancelled)return 0;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(retired_eof)return 0; /* Exact retired identity; real RX/clock checks above. */
#endif
#else
 if(retired)return 0;
#endif
#endif
 if(v.revision==s->reply.revision)return v.cancelled==s->reply.cancelled?0:
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(4,-EPIPE,&v,&st)
#else
-EPIPE
#endif
;
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 unsigned steps=retired?2:1;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 bool revision_ok=s->reply.revision<=UINT64_MAX-steps&&v.revision==s->reply.revision+steps;
 if(s->preopened&&s->prepared_binding.product_experiment==1&&retired){
  /* Cancel publishes once; speaker_end/reply_end each retire afterward.
   * An unscheduled worker may first observe either terminal revision.
   * This is still one original cancel/seal, never a new reply or clock. */
  revision_ok=s->reply.live&&s->reply.bound&&!s->reply.retired_terminal&&
   !s->reply.cancel_observed_us&&v.revision>s->reply.revision&&
   v.revision-s->reply.revision>=2&&v.revision-s->reply.revision<=3;
 }
 if(!revision_ok||
#else
 if(s->reply.revision>UINT64_MAX-steps||v.revision!=s->reply.revision+steps||
#endif
#else
 if(s->reply.revision==UINT64_MAX||v.revision!=s->reply.revision+1||
#endif
 !v.cancelled||s->reply.cancelled||!v.cancel_observed_us)return 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(5,-EPIPE,&v,&st)
#else
-EPIPE
#endif
;
 /* The indices are processed-next at worker observation, not the physical
  * cancel instant, DAC tail or a qualified onset interval. No frame, no seal. */
 #if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 if(!s->bridge_lease&&!s->preopened)return 
#else
 if(!s->bridge_lease)return 
#endif

#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(6,-EAGAIN,&v,&st)
#else
-EAGAIN
#endif
;
 #endif
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 if(s->preopened){struct ne_identity apm_id=near_identity(s);rc=k7_apm21_evidence_note_cancel(&s->apm21_evidence,&apm_id,&s->context,s->processed_mic_next,s->processed_ref_next);}
 else
#endif
 rc=diag_bridge_boundary(s->bridge,s->bridge_lease,&s->context,
  s->processed_mic_next,s->processed_ref_next,&s->bridge_lease);
 if(rc)return 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
POLL_FAIL(7,rc,&v,&st)
#else
rc
#endif
;
 s->cancel_mic_next=s->processed_mic_next;s->cancel_ref_next=s->processed_ref_next;
 s->seals++;s->reply=v;
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
#if !defined(K7_ECHO_CANCEL_CONTINUATION) || !K7_ECHO_CANCEL_CONTINUATION
 ne_invalidate(s->near); /* no qualified cancel-tail continuation */
#endif
#endif
 return 0;
}
static int continuity_begin(struct k7_echo_runtime*s,struct k7_echo_diag_mic_id m){
 struct k7_echo_reply_identity v;int rc=k7_echo_reply_snapshot(&v);if(rc)return rc;
 if(!v.live||!v.bound||v.cancelled||v.owner!=m.owner||v.input!=m.epoch||v.tx!=s->begin.generation)return -EPIPE;
 s->reply=v;s->context=(struct vv_confirmed_wake_context){.owner_epoch=m.owner,.input_epoch=m.epoch,
 .scope_incarnation=v.scope_incarnation,.lease_serial=v.lease_serial,.asr_epoch=v.asr,
 .reply_id=v.reply,.tx_generation=v.tx};
 s->continuity=(struct k7_aec_continuity_identity){.owner_epoch=m.owner,.mapping_epoch=v.instance,
 .reference_instance=s->begin.instance,.reference_epoch=s->begin.epoch,.input_epoch=m.epoch,
 .clock_epoch=s->start.epoch,.render_generation=v.tx};
 s->processed_mic_next=s->start.rx_samples;s->processed_ref_next=s->begin.first_enqueue;
 s->seals=0;s->cancel_mic_next=s->cancel_ref_next=0;
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 s->bridge_lease=0;s->lag_primed=false;rc=0;
#if defined(K7_ECHO_AEC10_ADAPTER) && K7_ECHO_AEC10_ADAPTER
 s->published_mic_next=s->processed_mic_next;s->published_ref_next=s->processed_ref_next;
#endif
#line 548
#else
 rc=diag_bridge_begin(s->bridge,&s->context,&s->continuity,
  s->processed_mic_next,s->processed_ref_next,&s->bridge_lease);
 if(!rc&&s->near){struct ne_identity id=near_identity(s);rc=ne_begin(s->near,&id,s->processed_mic_next);}
#endif
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 if(!rc&&s->preopened){struct ne_identity id=near_identity(s);
  rc=k7_apm21_joint_begin(&s->apm21_joint,&id,&s->context,s->apm21_live.tap_epoch);
  if(!rc)rc=k7_apm21_joint_record(&s->apm21_joint,&s->begin);
 }
#endif
 s->processed_frames=s->evidence_windows=0;s->capture_anchor_overflow=0;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT && defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE && defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 if(s->preopened&&s->prepared_binding.product_experiment==1){
  s->apm21_round_frames_base=s->apm21_evidence.frames;
  s->apm21_round_windows_base=s->apm21_evidence.windows;
  s->apm21_round_candidates_base=s->apm21_evidence.candidates;s->round_peak=0;
 }
#endif
 return rc;
#else
 return diag_bridge_begin(s->bridge,&s->context,&s->continuity,
  s->processed_mic_next,s->processed_ref_next,&s->bridge_lease);
#endif
}

#endif
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
/* Sole-worker alternative model branch before legacy lag39. Diagnostic mode
 * never emits. Explicit one-shot product mode separately checks joint source
 * and OLD context before the original observer/near-driver/route consumes it. */
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
/* Worker-only first fault snapshot, at its original call site. Later service
 * timeout/stop snapshots cannot overwrite the cause of a lost continuation. */
static int apm21_first_fault(struct k7_echo_runtime *s,int rc,unsigned site){
 if(rc>=0||rc==-EAGAIN)return rc;
 if(s->apm21_joint.prompt_only)atomic_store_explicit(&s->apm21_prompt_error,rc,memory_order_release);
 if(!s->apm21_diag_error_logged){
  const struct k7_apm21_joint *j=&s->apm21_joint;
  const struct k7_apm21_live_worker *w=&s->apm21_live;
  const struct k7_apm21_evidence_bridge *b=&s->apm21_evidence;
  s->apm21_diag_error_logged=true;
  printf("APM21_FIRSTFAULT site=%u rc=%d owner=%llu reply=%llu tx=%u joint=%d bridge=%d off=%u disabled=%u quiet=%u end=%u cut=%u transport=%u hypothesis=%u source_wait=%u packet_wait=%u stream=%u ingestion=%u source_mic=%llu source_ref=%llu published_mic=%llu published_ref=%llu attempted_ref=%llu accepted_ref=%llu stream_ref=%llu cut_at=%llu cut_published=%llu pending_pair=%u ref_count=%u\n",
   site,rc,(unsigned long long)s->owner,(unsigned long long)s->reply.reply,s->reply.tx,j->failure,b->failure,
   j->have_off,j->have_disabled,j->have_quiet,j->have_end,j->have_cut,j->transport_verified,j->hypothesis_enabled,
   s->apm21_source_wait,b->packet_pending,w->stream.state,w->stream.ingestion_fault,
   (unsigned long long)b->source_mic_next,(unsigned long long)b->source_ref_next,
   (unsigned long long)b->last_mic_next,(unsigned long long)b->last_ref_next,
   (unsigned long long)w->stream.attempted_reference_next,(unsigned long long)w->stream.accepted_reference_next,
   (unsigned long long)w->stream.reference_next,(unsigned long long)j->cut.cut,(unsigned long long)j->cut.published_upper,w->pending_pair_valid,w->ref_count);
 }
 return rc;
}
/* Cancellation is a software boundary, not a source discontinuity. A packet
 * already proved against the immutable genuine FIFO ledger needs no invented
 * poststop zeros or completed stop proof. Keep every old identity field. */
static int apm21_genuine_packet(struct k7_echo_runtime *s,const struct k7_echo_packet *p,const struct vv_confirmed_wake_context *c){
 const struct k7_apm21_joint *j=&s->apm21_joint;
 const struct k7_apm21_live_worker *w=&s->apm21_live;
 if(!j->begun)return -EINVAL;
 if(j->failure)return j->failure;
 if(s->apm21_evidence.failure)return s->apm21_evidence.failure;
 if(w->stream.state!=K7_APM21_STREAM_ACTIVE||w->stream.ingestion_fault||
    w->stream.attempted_reference_next!=w->stream.accepted_reference_next||w->stream.accepted_reference_next!=w->stream.reference_next)return -EPIPE;
 if(w->owner!=j->identity.owner||w->tap_epoch!=j->tap_epoch)return -ESTALE;
#define SAME_ID(x) if(p->identity.x!=j->identity.x)return -ESTALE
 SAME_ID(owner);SAME_ID(reference_instance);SAME_ID(reference_epoch);SAME_ID(clock_epoch);SAME_ID(dialogue_instance);SAME_ID(scope);SAME_ID(lease);SAME_ID(reply);SAME_ID(mapping_revision);SAME_ID(input_epoch);SAME_ID(tx_generation);
#undef SAME_ID
#define SAME_CTX(x) if(c->x!=j->context.x)return -ESTALE
 SAME_CTX(owner_epoch);SAME_CTX(model_epoch);SAME_CTX(clean_epoch);SAME_CTX(alignment_epoch);SAME_CTX(scope_incarnation);SAME_CTX(lease_serial);SAME_CTX(reply_id);SAME_CTX(input_epoch);SAME_CTX(mode_epoch);SAME_CTX(asr_epoch);SAME_CTX(tx_generation);
#undef SAME_CTX
 if(p->reference_first>UINT64_MAX-320)return -EOVERFLOW;
 uint64_t upper=j->have_cut?j->cut.cut:w->stream.accepted_reference_next;
 return !j->hypothesis_enabled&&p->reference_first+320<=upper;
}
/* Successful EOF has a different real order from cancellation: the FIFO cut
 * and clear precede the tail guard, then AMP_OFF, a fresh disabled readback,
 * SOURCE_END and finally QUIET on a genuine RX successor. Preserve that order
 * and every original timestamp; do not manufacture a cancellation event. */
static int apm21_eof_idle(struct k7_apm21_joint *j,const struct k7_apm21_idle_event *e){
 if(e->kind!=K7_APM21_IDLE_SOURCE_END&&e->kind!=K7_APM21_IDLE_DIGITAL_QUIET)
  return k7_apm21_joint_idle(j,e);
 if(!j->begun||j->failure)return j->failure?j->failure:-EINVAL;
 if(!j->have_off||!j->have_disabled||!e->serial||
    (j->last_idle_serial&&e->serial!=j->last_idle_serial+1)||
    e->input_epoch!=j->identity.input_epoch||e->samples||e->first!=e->next||
    e->first!=e->boundary.rx_samples||e->boundary.epoch!=j->identity.clock_epoch||
    e->boundary.generation!=j->identity.tx_generation||e->boundary.flags||e->boundary.result||
    e->boundary.after_us<e->boundary.before_us||e->boundary.before_us<j->disabled.boundary.after_us)return -ESTALE;
#define OFF_SAME(x) if(e->off.x!=j->off.off.x)return -ESTALE
 OFF_SAME(before_us);OFF_SAME(after_us);OFF_SAME(rx_samples);OFF_SAME(generation);
 OFF_SAME(tx_queued_samples);OFF_SAME(rx_fifo_words);OFF_SAME(tx_fifo_words);
 OFF_SAME(discarded_words);OFF_SAME(epoch);OFF_SAME(event);OFF_SAME(flags);OFF_SAME(result);
#undef OFF_SAME
 if(e->kind==K7_APM21_IDLE_SOURCE_END){
  if(j->have_end||e->boundary.event!=K7_VOICE_ANCHOR_TX_COMPLETE)return -EPROTO;
  j->source_end=*e;j->have_end=1;
 }else{
  if(j->have_quiet||e->quiet_checks!=K7_APM21_IDLE_QUIET_CHECKS||
     (e->xfer&15u)!=11u||e->first<=j->off.off.rx_samples)return -EPROTO;
  j->quiet=*e;j->have_quiet=1;
 }
 j->last_idle_serial=e->serial;return 0;
}
static int apm21_eof_continue(struct k7_echo_runtime *s){
 struct k7_apm21_joint *j=&s->apm21_joint;
 struct k7_apm21_live_worker *w=&s->apm21_live;
 const struct tail_reference_archive *a=&j->archive;
 const struct k7_apm21_cut_descriptor *d=&j->cut;
 if(j->failure)return j->failure;
 if(atomic_load(&a->state)==TRA_FAILED)return a->reason?a->reason:-EPIPE;
 if(!j->have_cut||!j->have_off||!j->have_disabled||!j->have_end||!j->have_quiet||
    atomic_load(&a->state)!=TRA_DONE)return 0;
 if(d->after_us>j->off.off.before_us||a->clear.after_us>j->off.off.before_us||
    a->owner!=d->owner||a->instance!=d->reference_instance||a->epoch!=d->reference_epoch||
    a->generation!=d->generation||a->first!=d->first||a->next!=d->written||
    a->uncertain_partial_pair||a->digital_complete_pair_next!=d->cut||d->cut!=d->written||
    d->remaining_words||a->clear.result||a->clear.tx_fifo_words||a->clear.discarded_words||
    j->source_end.boundary.tx_queued_samples!=a->source_next||j->source_end.boundary.discarded_words||
    w->owner!=d->owner||w->tap_epoch!=d->tap_epoch||w->stream.ingestion_fault||
    w->stream.attempted_reference_next!=w->stream.accepted_reference_next||
    w->stream.accepted_reference_next!=w->stream.reference_next)return -ESTALE;
 if(!s->apm21_eof_verified){
  if(w->ref_count||w->ref_next!=d->published_upper||w->stream.reference_next!=d->published_upper)return 0;
  for(unsigned n=0;n<d->prefix_samples;n++){
   uint64_t index=d->published_upper+n;unsigned at=(unsigned)(index%TRA_SAMPLES);
   if(index<a->kept_first||index>=a->next||a->pcm[at]!=d->prefix_pcm[n]||
      a->zeros[at]!=((d->prefix_zero_bits[n/8]>>(n%8))&1u))return -EILSEQ;
  }
  /* Mode 2 has real successful-EOF proof, not a cancel bit or clean audio. */
  if(!j->prompt_only){s->apm21_evidence.poststop_cut=d->cut;s->apm21_evidence.poststop_enabled=2;}
  s->apm21_eof_verified=1;j->transport_verified=1;
 }
 return k7_apm21_live_worker_poststop(w,d);
}
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
static int apm21_prompt_begin(struct k7_echo_runtime *s,const struct k7_fifo_ref_record *r){
 struct k7_apm21_live_worker *w=&s->apm21_live;struct k7_echo_reply_identity id;
 struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
 if(!s->have_start||s->start.generation!=r->generation)return -EAGAIN;
 if(k7_echo_reply_snapshot(&id)||id.live||(id.bound&&(!id.retired_terminal||id.owner!=s->owner||id.input!=v.input_epoch||!id.tx||id.tx>s->floor))||(id.owner&&id.owner!=s->owner)||
    state(s)!=READY||s->cc||s->bridge_lease||s->pending_begin||s->generation||
    !v.running||!v.ready||v.finished||v.stop_requested||!v.input_epoch||
    r->generation!=v.tx_generation||r->generation<=s->floor||s->start.flags||s->start.result||
    s->start.epoch!=k7_voice_session_anchor_epoch(s->session)||s->start.rx_samples!=w->mic_origin||
    w->reply_lease||w->stream.reply_lease||w->round_pending||w->stream.ingestion_fault||
    w->stream.state!=K7_APM21_STREAM_ACTIVE||w->last_error)return -ESTALE;
 int rc=k7_apm21_joint_prompt_begin(&s->apm21_joint,s->owner,r,v.input_epoch,s->start.epoch,w->tap_epoch);
 if(!rc)rc=k7_apm21_joint_anchor(&s->apm21_joint,&s->start);
 if(!rc&&s->apm21_unclaimed_clear.event){
  if(s->apm21_unclaimed_clear.generation!=r->generation)rc=-ESTALE;
  else rc=k7_apm21_joint_anchor(&s->apm21_joint,&s->apm21_unclaimed_clear);
 }
 memset(&s->apm21_unclaimed_clear,0,sizeof(s->apm21_unclaimed_clear));
 return rc;
}
static int apm21_prompt_retire(struct k7_echo_runtime *s){
 struct k7_apm21_joint *j=&s->apm21_joint;struct k7_apm21_live_worker *w=&s->apm21_live;
 if(!j->prompt_only||!s->apm21_eof_verified||!j->transport_verified)return 0;
 uint32_t generation=j->identity.tx_generation;
 /* The actual non-scoped speaker posts this only after its successful TX,
  * source worker join and before releasing the shared speaker lock. */
 if(atomic_load_explicit(&s->apm21_prompt_release,memory_order_acquire)!=generation)return 0;
 struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
 if(!v.running||!v.ready||v.finished||v.stop_requested||v.tx_state!=K7_VOICE_TX_IDLE||
    v.tx_generation!=generation||v.input_epoch!=j->identity.input_epoch||
    k7_voice_session_anchor_epoch(s->session)!=j->identity.clock_epoch||
    w->reply_lease||w->stream.reply_lease||!s->apm21_have_physical_cut)return -ESTALE;
 if(w->round_serial==UINT64_MAX)return -EOVERFLOW;
 struct k7_apm21_round_ticket ticket={.serial=w->round_serial+1,.owner=s->owner,
  .dsp_epoch=w->epoch,.reply_lease=0,.retired_checks=K7_APM21_ROUND_RETIRED_CHECKS,
  .cut=s->apm21_physical_cut};
 int rc=k7_apm21_live_worker_round_retire(w,&ticket);if(rc)return rc;
 s->floor=generation;s->have_start=false;s->apm21_ref_wait=s->apm21_idle_wait=0;
 s->apm21_have_physical_cut=s->apm21_eof_verified=0;
 memset(j,0,sizeof(*j));memset(&s->start,0,sizeof(s->start));
 atomic_store_explicit(&s->apm21_prompt_release,0,memory_order_release);
 atomic_store_explicit(&s->apm21_prompt_retired,generation,memory_order_release);
 printf("APM21_PROMPT generation=%u retired=1 reply_lease=0 actions=0 dsp_epoch=%llu\n",generation,(unsigned long long)w->epoch);fflush(stdout);
 return 0;
}
#endif
static void apm21_joint_sources(struct k7_echo_runtime *s){
 if(!s->preopened||s->apm21_joint.failure)return;
#if !defined(K7_APM21_NOMINAL_EXPERIMENT) || !K7_APM21_NOMINAL_EXPERIMENT
 if(!s->apm21_joint.begun)return;
#endif
 struct k7_apm21_idle_event e;int rc=0;
 for(unsigned i=0;i<32;i++){
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(s->apm21_idle_wait){e=s->apm21_idle_pending;rc=1;}
  else
#endif
  rc=k7_apm21_idle_take(k7_voice_session_apm21_idle_events(s->session),&e);if(rc<=0)break;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  s->apm21_idle_wait=0;
  if(e.off.generation<=s->floor)continue; /* OLD metadata, already retired. */
  if(!s->apm21_joint.begun){s->apm21_idle_pending=e;s->apm21_idle_wait=1;break;}
  if(s->prepared_binding.product_experiment==1&&!s->reply.cancelled)
   rc=apm21_eof_idle(&s->apm21_joint,&e);
  else
#endif
   rc=k7_apm21_joint_idle(&s->apm21_joint,&e);
  if(rc)break;
 }
 if(rc<0){s->apm21_joint.failure=rc;k7_apm21_evidence_invalidate(&s->apm21_evidence,rc);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  (void)apm21_first_fault(s,rc,1);
#endif
  return;}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(!s->apm21_joint.begun)return;
#endif
 unsigned source_site=2;
 struct k7_apm21_cut_descriptor cut;rc=k7_apm21_tap_take_cut(s->apm21_live.tap,s->owner,&cut);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(rc>0){
  s->apm21_physical_cut=cut;s->apm21_have_physical_cut=1;
  struct k7_apm21_cut_descriptor mapped;
  int mapped_rc=k7_apm21_live_worker_map_cut(&s->apm21_live,&cut,&mapped);
  if(mapped_rc)rc=mapped_rc;else cut=mapped;
 }
#endif
 if(rc>0){source_site=3;rc=k7_apm21_joint_cut(&s->apm21_joint,&cut);}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(!rc&&s->prepared_binding.product_experiment==1&&!s->reply.cancelled){source_site=6;rc=apm21_eof_continue(s);if(!rc)rc=apm21_prompt_retire(s);}
 else
#endif
 {
  if(!rc){source_site=4;rc=k7_apm21_joint_reconcile(&s->apm21_joint,&s->apm21_live);}
  if(rc>=0&&s->apm21_joint.transport_verified){source_site=5;rc=k7_apm21_joint_continue(&s->apm21_joint,&s->apm21_live,&s->apm21_evidence);}
 }
 (void)source_site;
 if(rc<0){s->apm21_joint.failure=rc;k7_apm21_evidence_invalidate(&s->apm21_evidence,rc);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  (void)apm21_first_fault(s,rc,source_site);
#endif
 }
}
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
static int apm21_product_packet(void *arg,const struct k7_echo_packet *packet,const struct vv_confirmed_wake_context *context){
 struct k7_echo_runtime *s=arg;
 if(s->prepared_binding.product_experiment!=1)return 0;
 if(!context||!packet)return -EINVAL;
 int rc=continuity_poll(s);if(rc)return apm21_first_fault(s,rc,10);
 if(s->apm21_joint.failure)return apm21_first_fault(s,s->apm21_joint.failure,11);
 int genuine=apm21_genuine_packet(s,packet,context);
 if(genuine<0)return apm21_first_fault(s,genuine,14);
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 /* Every real model report reaches the sink. Pending EAGAIN retries cannot
  * inflate max, and frame/window/candidate totals remain bridge-owned. */
 if(packet->report&&packet->report->model.probability>s->round_peak)
  s->round_peak=packet->report->model.probability;
#endif
 /* Drain matched old EOF PCM/model output to keep DSP continuous. The real
  * completed reply cannot gain a new onset or PF action from its idle tail. */
 if(s->apm21_eof_verified&&!packet->cancel_observed&&!s->reply.cancelled)return 0;
 if(!s->reply.cancelled&&s->apm21_joint.have_off)return -EAGAIN;
 if(!genuine){
  if(packet->cancel_observed||s->reply.cancelled){
   if(!s->apm21_joint.hypothesis_enabled)return -EAGAIN;
   rc=k7_apm21_joint_tail_ready(&s->apm21_joint,&s->apm21_live,&s->apm21_evidence);
   if(rc!=1)return rc==-EAGAIN?-EAGAIN:apm21_first_fault(s,rc<0?rc:-EPIPE,12);
  }else if(s->apm21_joint.have_cut||s->apm21_joint.have_off)return -EAGAIN;
  else if(!k7_apm21_joint_live_ready(&s->apm21_joint,&s->apm21_live,&s->apm21_evidence))return apm21_first_fault(s,-EPIPE,13);
 }
 struct k7_echo_packet product=*packet;k7_apm21_nominal_identity_expected(&product.apm_nominal);
 if(s->reply.cancelled&&!packet->cancel_observed)product.report=NULL; /* preserve ordered original PCM, never stale onset */
 if(packet->cancel_observed&&s->apm21_joint.hypothesis_enabled){uint64_t first=packet->reference_first,cut=s->apm21_joint.cut.cut;
  if(first>UINT64_MAX-320)return -EOVERFLOW;
  product.apm_post_stop_hypothesis_samples=first>=cut?320:first+320>cut?(unsigned)(first+320-cut):0;
 }
 k7_echo_observer_emit(&product,0);return 0;
}
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
/* Diagnostic mirror of the existing ne_policy {1000,.5,.01,.9f,3} only.
 * Bits: 1 probability, 2 residual power, 4 correlation, 8 residual/ref ratio,
 * 16 clipping, 32 consecutive windows. Never changes report/candidate. */
static unsigned apm21_gate_fail_mask(const struct ne_report *r){
 return (r->model.probability<.9f?1u:0u)|(r->residual_power<1000?2u:0u)|
  (r->correlation2>.5?4u:0u)|(r->residual_power<r->reference_power*.01?8u:0u)|
  (r->clipped?16u:0u)|(r->consecutive<3?32u:0u);
}
#endif
static void apm21_diagnostic_pump(struct k7_echo_runtime *s)
{
 uint64_t prior=s->apm21_evidence.windows;
 int rc=k7_apm21_evidence_pump(&s->apm21_evidence);
 if(s->apm21_evidence.have_watermark){s->published_mic_next=s->apm21_evidence.last_mic_next;s->published_ref_next=s->apm21_evidence.last_ref_next;}
 if(s->apm21_evidence.windows!=prior){
  const struct ne_report *r=&s->apm21_evidence.report;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(s->prepared_binding.product_experiment==1){
   if(r->candidate||(s->apm21_evidence.windows%16)==0)
    printf("APM21_NOMINAL explicit_product=1 mapping=%llu reply=%llu candidate=%u qualified=0 lag_applied=0 poststop_hypothesis=%u p=%.6f mic_power=%.1f ref_power=%.1f res_power=%.1f corr=%.6f clipped=%u consecutive=%u fail_mask=%u\n",
     (unsigned long long)K7_APM21_NOMINAL_REVISION,(unsigned long long)r->identity.reply,r->candidate,s->apm21_joint.hypothesis_enabled,
     (double)r->model.probability,r->microphone_power,r->reference_power,r->residual_power,r->correlation2,r->clipped,r->consecutive,apm21_gate_fail_mask(r));
  }else
#endif
  if(r->candidate||(s->apm21_evidence.windows%16)==0)
   printf("APM21_EVIDENCE diagnostic=1 actions=0 reply=%llu first=%llu next=%llu p=%.6f candidate=%u qualified=0 lag_applied=0\n",
    (unsigned long long)r->identity.reply,(unsigned long long)r->model.input_first_sample,
    (unsigned long long)r->model.input_end_sample,(double)r->model.probability,r->candidate);
 }
 /* One first error line, not an unbounded busy-loop stream. */
 if(rc<0&&!s->apm21_diag_error_logged){
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  (void)apm21_first_fault(s,rc,20);
#else
  s->apm21_diag_error_logged=true;printf("APM21_EVIDENCE diagnostic=1 actions=0 result=%d qualified=0\n",rc);
#endif
 }
}
static int apm21_diagnostic_pair(struct k7_echo_runtime *s,const struct cc_pair *p,unsigned cancel)
{
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(s->prepared_binding.product_experiment==1&&p->post_mute_samples&&!s->apm21_evidence.poststop_enabled)return -EAGAIN;
#endif
 struct ne_identity id=near_identity(s);
 int rc=k7_apm21_evidence_register(&s->apm21_evidence,&id,&s->context,
  p->microphone_first,p->reference_first,p->microphone,p->reference,cancel,p->post_mute_samples);
 if(!rc){s->processed_mic_next=p->microphone_first+320;s->processed_ref_next=p->reference_first+320;apm21_diagnostic_pump(s);}
 else if(rc!=-ENODATA&&!s->apm21_diag_error_logged){
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  (void)apm21_first_fault(s,rc,21);
#else
  s->apm21_diag_error_logged=true;printf("APM21_EVIDENCE diagnostic=1 actions=0 result=%d qualified=0\n",rc);
#endif
 }
 return rc;
}
#endif
static void paired(struct k7_echo_runtime*s){
#line 564
 for(unsigned n=0;n<8&&state(s)==ACTIVE;n++){
  int rc=continuity_poll(s);struct cc_pair p;
  if(!rc){
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
   if(s->preopened&&s->apm21_source_wait){p=s->apm21_source_pending;rc=1;}
   else
#endif
   rc=cc_pop(s->cc,&p);
  }
  if(!rc)return;
  if(rc<0){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1001)
#else
freeze(s,rc)
#endif
;return;}
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
  if(s->preopened){
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
   unsigned observed=s->apm21_source_wait?s->apm21_source_cancel:s->reply.cancelled;
   int source=apm21_diagnostic_pair(s,&p,observed);
   if(source==-EAGAIN){s->apm21_source_pending=p;s->apm21_source_cancel=observed;s->apm21_source_wait=1;return;}
   s->apm21_source_wait=0;if(source<0)return;
#else
   (void)apm21_diagnostic_pair(s,&p,s->reply.cancelled);
#endif
   continue;
  }
#endif
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
  /* Provisional field lag=39: retain genuine prior reference, never pad the
#line 577
   * initial delay with invented samples. First 320 MIC samples are warmup. */
  int16_t delayed[320];unsigned char zero[320];
  for(unsigned i=0;i<39;i++){delayed[i]=s->lag_pcm[i];zero[i]=s->lag_zero[i];}
  for(unsigned i=39;i<320;i++){delayed[i]=p.reference[i-39];zero[i]=(i-39>=320-p.post_mute_samples);}
  for(unsigned i=0;i<39;i++){s->lag_pcm[i]=p.reference[281+i];s->lag_zero[i]=(281+i>=320-p.post_mute_samples);}
  if(!s->lag_primed){s->lag_primed=true;continue;}
  if(p.reference_first<39){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-ERANGE,1002)
#else
freeze(s,-ERANGE)
#endif
;return;}
  memcpy(p.reference,delayed,sizeof(delayed));p.reference_first-=39;p.post_mute_samples=0;
  for(unsigned i=0;i<320;i++)p.post_mute_samples+=zero[i];
  if(!s->bridge_lease){
   rc=diag_bridge_begin(s->bridge,&s->context,&s->continuity,p.microphone_first,p.reference_first,&s->bridge_lease);
#if defined(K7_ECHO_AEC10_ADAPTER) && K7_ECHO_AEC10_ADAPTER
   if(!rc){s->published_mic_next=p.microphone_first;s->published_ref_next=p.reference_first;}
#endif
   struct ne_identity id=near_identity(s);if(!rc)rc=ne_begin(s->near,&id,p.microphone_first);
#line 595
   if(rc){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1003)
#else
freeze(s,rc)
#endif
;return;}
  }
#endif
  struct k7_voice_frame raw={.first_sample=p.microphone_first,.epoch=p.input_epoch,.samples=320},res;
  memcpy(raw.pcm,p.microphone,sizeof(raw.pcm));
#if defined(K7_ECHO_AEC10_ADAPTER) && K7_ECHO_AEC10_ADAPTER
  /* before/after are the immutable worker lineage, validated against actual
   * session/reply snapshots by continuity_poll before AND after this call.
   * They do not claim an atomic physical render/MIC observation. */
  rc=diag_bridge_process_paired(s->bridge,s->bridge_lease,&s->continuity,&s->continuity,
   &raw,p.reference,p.reference_first,zero,1,&s->aec10_output);
  if(rc>=0){
   s->processed_mic_next=p.microphone_first+320;s->processed_ref_next=p.reference_first+320;
   int poll_rc=continuity_poll(s);
   if(poll_rc)rc=poll_rc;
  }
  if(rc==0)continue; /* No fabricated startup/residual/model frame. */
  if(rc>0){
   const struct diag_bridge_paired_output*out=&s->aec10_output;
   raw=out->raw;res=out->residual;
   p.microphone_first=raw.first_sample;p.reference_first=out->reference_first;
   p.input_epoch=raw.epoch;p.post_mute_samples=out->source_stop_zero_samples;
   memcpy(p.microphone,raw.pcm,sizeof(p.microphone));
   memcpy(p.reference,out->reference,sizeof(p.reference));memcpy(zero,out->zero,sizeof(zero));
   s->published_mic_next=out->published_mic_next;s->published_ref_next=out->published_ref_next;
   if(!s->processed_frames)printf("ECHO_AEC10 algorithm_delay=%u publication_delay=%u input_mic_next=%llu nominal_mic_next=%llu input_ref_next=%llu nominal_ref_next=%llu startup_unavailable=%llu qualified=0\n",
    out->backend_delay_samples,out->publication_delay_samples,
    (unsigned long long)s->processed_mic_next,(unsigned long long)s->published_mic_next,
    (unsigned long long)s->processed_ref_next,(unsigned long long)s->published_ref_next,
    (unsigned long long)out->startup_unavailable_samples);
   rc=0;
  }
#else
  rc=diag_bridge_process(s->bridge,s->bridge_lease,&s->continuity,&raw,p.reference,p.reference_first,&res);
#endif
#line 607
  if(rc){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1004)
#else
freeze(s,rc)
#endif
;return;}
#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
  unsigned capture_index=capture_frame(s,&p,&res,zero);
#endif
#if !defined(K7_ECHO_AEC10_ADAPTER) || !K7_ECHO_AEC10_ADAPTER
  s->processed_mic_next=p.microphone_first+320;s->processed_ref_next=p.reference_first+320;
#endif
#line 618
  struct ne_identity id=near_identity(s);struct ne_report report;
  int nr=ne_feed(s->near,&id,&id,p.microphone_first,p.microphone,p.reference,res.pcm,&report);
#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
  capture_model(s,capture_index,nr,&report);
#endif
  rc=continuity_poll(s);if(rc||nr<0){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc?rc:nr,1005)
#else
freeze(s,rc?rc:nr)
#endif
;return;}
  struct k7_echo_packet packet={.identity=id,.raw=raw,.residual=res,.reference=p.reference,
   .reference_first=p.reference_first,.report=nr?&report:NULL,
   .cancel_observed=s->reply.cancelled,.source_stop_zero_samples=p.post_mute_samples
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
   ,.applied_lag_samples=39
#endif
  };
  k7_echo_observer_emit(&packet,0);
  if(s->processed_frames==UINT64_MAX){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EOVERFLOW,1006)
#else
freeze(s,-EOVERFLOW)
#endif
;return;}s->processed_frames++;
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
  if(nr){s->evidence_windows++;
   if(report.model.probability>s->round_peak)s->round_peak=report.model.probability;
   if(report.candidate)s->round_candidates++;
   if(report.candidate||(s->evidence_windows%16)==0)
#else
  if(nr){s->evidence_windows++;if(report.candidate||(s->evidence_windows%16)==0)
#endif
   printf("CANCEL_CONTINUATION mic_next=%llu ref_next=%llu cancel=%u source_stop_zero=%u p=%.6f candidate=%u qualified=0\n",
    (unsigned long long)s->processed_mic_next,(unsigned long long)s->processed_ref_next,
    s->reply.cancelled,p.post_mute_samples,(double)report.model.probability,report.candidate);}
 }
}
static int cc_mic(struct k7_echo_runtime*s,const struct k7_voice_frame*f){
 uint64_t first=f->first_sample;unsigned skip=0;
 if(first<s->start.rx_samples){uint64_t d=s->start.rx_samples-first;if(d>=f->samples)return 0;skip=(unsigned)d;first+=skip;}
 return cc_microphone(s->cc,s->owner,f->epoch,first,f->pcm+skip,f->samples-skip);
}
#else
static void paired(struct k7_echo_runtime *s){
 for(unsigned n=0;n<8&&state(s)==ACTIVE;n++){
  struct k7_fifo_ref_status r;struct k7_echo_diag_mic_id m;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  int rc=continuity_poll(s);
  if(!rc)rc=snapshot(s,&r,&m);
  struct k7_echo_diag_pair pair;
#else
  int rc=snapshot(s,&r,&m);struct k7_echo_diag_pair pair;
#endif
  if(!rc)rc=k7_echo_diag_pop(s->pairer,&r,m,&pair);
  if(!rc)return;
  if(rc<0){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1007)
#else
freeze(s,rc)
#endif
;return;}
  unsigned frame=atomic_load(&s->frames);
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
  if(!s->residual_mode&&frame>=CAPTURE_FRAMES){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EFBIG,1008)
#else
freeze(s,-EFBIG)
#endif
;return;}
#else
  if(frame>=CAPTURE_FRAMES){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EFBIG,1009)
#else
freeze(s,-EFBIG)
#endif
;return;}
#endif
  int16_t out[320];
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  struct k7_voice_frame f={.first_sample=pair.mic_first,.epoch=pair.mic_epoch,.samples=320},res;
  memcpy(f.pcm,pair.microphone,sizeof(f.pcm));
  rc=diag_bridge_process(s->bridge,s->bridge_lease,&s->continuity,&f,pair.reference,pair.fifo_first,&res);
  if(!rc)memcpy(out,res.pcm,sizeof(out));
#else
  rc=k7_aec_diagnostic_process(s->aec,pair.microphone,pair.reference,320,out);
#endif

  /* Recheck actual reference revocation after potentially slow DSP. */
  if(!rc&&!k7_voice_session_fifo_matches(s->session,pair.reference_instance,
       pair.reference_epoch,pair.generation))rc=-EPIPE;
  if(!rc&&k7_voice_session_input_epoch(s->session)!=pair.mic_epoch)rc=-EPIPE;
  if(rc){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1010)
#else
freeze(s,rc)
#endif
;return;}
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  struct k7_aec_stats stats;struct k7_aec_continuity_status cs;
  rc=diag_bridge_status(s->bridge,&cs,&stats);
  if(!rc&&(!cs.active||cs.qualified))rc=-EPROTO;
  if(!rc){s->processed_mic_next=cs.mic_next_sample;s->processed_ref_next=cs.reference_next_sample;}
  if(!rc)rc=continuity_poll(s);
#else
  struct k7_aec_stats stats;rc=k7_aec_get_stats(s->aec,&stats);
#endif

  if(rc||stats.ready||stats.frames||!pair.hypothesis_only){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EPROTO,1011)
#else
freeze(s,-EPROTO)
#endif
;return;}
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
  if(s->near){
   struct ne_identity id=near_identity(s);struct ne_report report;
   int nr=ne_feed(s->near,&id,&id,pair.mic_first,pair.microphone,pair.reference,out,&report);
   /* Re-read actual RX/reference/reply after model work, before publishing. */
   int live=continuity_poll(s);
   if(live||s->reply.cancelled){ne_invalidate(s->near);if(live){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,live,1012)
#else
freeze(s,live)
#endif
;return;}}
   else if(nr<0){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,nr,1013)
#else
freeze(s,nr)
#endif
;return;}
   else {
#if defined(K7_ECHO_NEAR_ADAPTER) && K7_ECHO_NEAR_ADAPTER
    struct k7_echo_packet packet={.identity=id,.raw=f,.residual=res,
     .reference=pair.reference,.reference_first=pair.fifo_first,.report=nr?&report:NULL};
    k7_echo_observer_emit(&packet,0);
#endif
   }
   if(!live&&!s->reply.cancelled&&nr){
    if(s->evidence_windows==UINT64_MAX){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EOVERFLOW,1014)
#else
freeze(s,-EOVERFLOW)
#endif
;return;}
    s->evidence_windows++;
    if(report.candidate||(s->evidence_windows%16)==0)
     printf("RESIDUAL_EVIDENCE owner=%llu reply=%llu first=%llu next=%llu p=%.6f candidate=%u qualified=0 hypothesis=1 actions=0\n",
      (unsigned long long)id.owner,(unsigned long long)id.reply,
      (unsigned long long)report.model.input_first_sample,(unsigned long long)report.model.input_end_sample,
      (double)report.model.probability,report.candidate);
   }
  }
  if(s->processed_frames==UINT64_MAX){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EOVERFLOW,1015)
#else
freeze(s,-EOVERFLOW)
#endif
;return;}s->processed_frames++;
  /* Capture prefix stays bounded and immutable when full; DSP keeps consuming. */
  if(frame>=CAPTURE_FRAMES)continue;
#endif
  if(!frame){s->mic_first=pair.mic_first;s->fifo_first=pair.fifo_first;}
  memcpy(s->mic+frame*320,pair.microphone,sizeof(pair.microphone));
  memcpy(s->reference+frame*320,pair.reference,sizeof(pair.reference));
  memcpy(s->residual+frame*320,out,sizeof(out));
  memcpy(s->zeros+frame*40,pair.explicit_zero_bits,40);
  for(unsigned i=0;i<320;i++)s->zero_samples+=(pair.explicit_zero_bits[i/8]>>(i%8))&1u;
  atomic_store_explicit(&s->frames,frame+1,memory_order_release);
 }
}
#endif

#ifdef K7_ECHO_RAW_VAD_OBSERVER
/* Low-priority diagnostic worker only. The sole KWS producer still performs
 * the same bounded copy. RAW probability is not a barge-in event. */
static bool vad_recoverable(enum k7_raw_vad_failure f){
 return f==K7_RAW_VAD_FRAME||f==K7_RAW_VAD_CONTINUITY||
        f==K7_RAW_VAD_OWNER||f==K7_RAW_VAD_STALE||f==K7_RAW_VAD_TAP_LOSS;
}
static void vad_frame(struct k7_echo_runtime*s,const struct k7_voice_frame*f){
 if(!s->vad)return;
 unsigned faults=atomic_load(&s->rx_faults);
 if(faults!=s->vad_rx_faults){
  k7_raw_vad_invalidate(s->vad,K7_RAW_VAD_TAP_LOSS);
  s->vad_rx_faults=faults;
  if(faults!=UINT_MAX)(void)k7_raw_vad_rearm(s->vad);
 }
 struct k7_raw_vad_stats st;(void)k7_raw_vad_get_stats(s->vad,&st);
 if(!st.armed)return;
 struct k7_raw_vad_report report;int rc=k7_raw_vad_feed(s->vad,f,&report);
 if(atomic_load(&s->rx_faults)!=faults){
  k7_raw_vad_invalidate(s->vad,K7_RAW_VAD_TAP_LOSS);
  if(s->vad_tap_rejected!=UINT64_MAX)s->vad_tap_rejected++;
  return; /* No callback from an inference overlapping a lost tap. */
 }
 if(rc<0){
  (void)k7_raw_vad_get_stats(s->vad,&st);
  if(vad_recoverable(st.last_failure))(void)k7_raw_vad_rearm(s->vad);
 }else if(rc==1&&s->vad_sink)s->vad_sink(s->vad_context,&report);
}
#endif
static void microphone(struct k7_echo_runtime *s,const struct k7_voice_frame *f){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
 s->trace.mic_first=f->first_sample;s->trace.mic_epoch=f->epoch;
 s->trace.mic_samples=f->samples;s->trace.mic_flags=f->flags;
#endif
 tail_frame(s,f);
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 vad_frame(s,f);
#endif
 if(f->samples!=320||f->flags||!f->epoch||f->first_sample>UINT64_MAX-320){
  
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EPROTO,1016)
#else
freeze(s,-EPROTO)
#endif
;s->count=s->head=0;return;
 }
 if(s->count){struct k7_voice_frame *last=&s->history[(s->head+s->count-1)%HISTORY];
  if(f->epoch!=last->epoch||f->first_sample!=last->first_sample+320){
   
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EPIPE,1017)
#else
freeze(s,-EPIPE)
#endif
;tail_freeze(s,-EPIPE);s->head=s->count=0;
  }
 }
 s->history_epoch=f->epoch;
 if(s->count==HISTORY){s->head=(s->head+1)%HISTORY;s->count--;}
 s->history[(s->head+s->count)%HISTORY]=*f;s->count++;
 if(state(s)==ACTIVE){
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
  int rc=cc_mic(s,f);
#else
  int rc=k7_echo_diag_microphone(s->pairer,(struct k7_echo_diag_mic_id){s->owner,f->epoch},
    f->first_sample,f->pcm,f->samples);
#endif
  if(rc)
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1018)
#else
freeze(s,rc)
#endif
;else paired(s);
 }
}
static void try_begin(struct k7_echo_runtime *s){
 if(!s->pending_begin||state(s)!=ARMED||!s->have_start)return;
 if(s->start.generation!=s->begin.generation)return;
 struct k7_fifo_ref_status r;struct k7_echo_diag_mic_id m;int rc=snapshot(s,&r,&m);
 if(!rc&&(!s->count||s->history_epoch!=m.epoch||s->start.flags||s->start.result||
    s->start.epoch!=k7_voice_session_anchor_epoch(s->session)||
    s->start.after_us<s->start.before_us))rc=-EPIPE;
 /* This aligns software indices to TX_START's RX snapshot only. It is NOT
  * a DAC timestamp, acoustic delay estimate, or ALIGNMENT_VERIFIED assertion. */
 if(!rc)rc=k7_echo_diag_begin(s->pairer,&s->begin,&r,m,
                            s->begin.first_enqueue,s->start.rx_samples);
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 if(!rc)rc=continuity_begin(s,m);
#else
 if(!rc)rc=k7_aec_diagnostic_reset(s->aec);
#endif

#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
 if(!rc){cc_destroy(s->cc);s->cc=NULL;rc=cc_create(s->owner,m.epoch,s->start.epoch,s->start.rx_samples,s->floor,&s->cc);}
 if(!rc)rc=cc_record(s->cc,&s->begin);
#endif
 if(rc){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1019)
#else
freeze(s,rc)
#endif
;return;}
 s->generation=s->begin.generation;s->instance=s->begin.instance;s->ref_epoch=s->begin.epoch;
 s->anchors[0]=s->start;s->anchor_count=1;s->pending_begin=false;
 atomic_store_explicit(&s->state,ACTIVE,memory_order_release);
 for(unsigned i=0;i<s->count&&state(s)==ACTIVE;i++){
  const struct k7_voice_frame *f=&s->history[(s->head+i)%HISTORY];
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
  rc=cc_mic(s,f);
#else
  rc=k7_echo_diag_microphone(s->pairer,m,f->first_sample,f->pcm,f->samples);
#endif
  if(rc)
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1020)
#else
freeze(s,rc)
#endif
;else paired(s);
 }
}
static void anchors(struct k7_echo_runtime *s){
 for(unsigned i=0;i<16;i++){
  struct k7_voice_anchor a;int rc=k7_voice_session_take_anchor(s->session,&a);
  if(!rc)break;
  if(rc<0){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1021)
#else
freeze(s,rc)
#endif
;tail_freeze(s,rc);s->have_start=false;break;}
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
  s->trace.anchor=a;s->trace.anchor_valid=true;
#endif
  tail_anchor(s,&a);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(s->preopened&&state(s)==READY&&!s->apm21_joint.begun&&
     a.generation>s->floor&&a.event==K7_VOICE_ANCHOR_TX_CLEAR){
   if(s->apm21_unclaimed_clear.event){(void)apm21_first_fault(s,-ESTALE,40);freeze(s,-ESTALE);}
   else s->apm21_unclaimed_clear=a;
  }
#endif
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
  if(s->preopened&&s->apm21_joint.begun){int joint_rc=k7_apm21_joint_anchor(&s->apm21_joint,&a);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
   if(joint_rc<0)(void)apm21_first_fault(s,joint_rc,30);
#else
   (void)joint_rc;
#endif
  }
#endif
  if(a.flags||a.epoch!=k7_voice_session_anchor_epoch(s->session)){
   
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EPIPE,1022)
#else
freeze(s,-EPIPE)
#endif
;s->have_start=false;continue;
  }
  if(a.event==K7_VOICE_ANCHOR_TX_START){s->start=a;s->have_start=true;}
  if(state(s)==ACTIVE){
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
   int cr=cc_anchor(s->cc,&a);if(cr){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,cr,1023)
#else
freeze(s,cr)
#endif
;continue;}
#endif
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
   if(s->anchor_count==ANCHORS){if(s->residual_mode){s->capture_anchor_overflow=1;continue;}
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-ENOSPC,1024)
#else
freeze(s,-ENOSPC)
#endif
;continue;}
#else
   if(s->anchor_count==ANCHORS){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-ENOSPC,1025)
#else
freeze(s,-ENOSPC)
#endif
;continue;}
#endif
   s->anchors[s->anchor_count++]=a;
  }
 }
}
static void references(struct k7_echo_runtime *s){
#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
 if(state(s)==READY&&s->near){struct k7_echo_reply_identity v;
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
  if(
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
     !s->apm21_joint.prompt_only&&
#endif
     !k7_echo_reply_snapshot(&v)&&v.live&&v.bound&&!v.cancelled&&v.owner==s->owner&&v.tx&&v.tx>s->floor){
#else
  if(!k7_echo_reply_snapshot(&v)&&v.live&&v.bound&&!v.cancelled&&v.owner==s->owner&&v.tx){
#endif
   s->floor=v.tx-1;s->faults_at_arm=atomic_load(&s->rx_faults);s->pending_begin=false;
   printf("NEAR_EXPERIMENT auto_arm=1 reply=%llu tx=%u calibrated=0\n",(unsigned long long)v.reply,v.tx);
   atomic_store_explicit(&s->state,ARMED,memory_order_release);
  }
 }
#endif
 try_begin(s);
 if(s->pending_begin){
  if(!k7_voice_session_fifo_matches(s->session,s->begin.instance,s->begin.epoch,s->begin.generation))
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EPIPE,1026)
#else
freeze(s,-EPIPE)
#endif
;
  return;
 }
 for(unsigned i=0;i<16;i++){
  struct k7_fifo_ref_record r;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  int rc;
  if(s->preopened&&s->apm21_ref_wait){r=s->apm21_ref_pending;rc=1;}
  else
#endif
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  rc=k7_voice_session_take_fifo_archive(s->session,&r);
#else
  int rc=k7_voice_session_take_fifo_archive(s->session,&r);
#endif
#else
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  rc=k7_voice_session_take_fifo_reference(s->session,&r);
#else
  int rc=k7_voice_session_take_fifo_reference(s->session,&r);
#endif
#endif
  if(!rc)break;
  if(rc<0){
   /* take announces every new reference epoch with EPIPE, including its
    * first healthy BEGIN. Before selecting a turn there is no old mapping
    * to invalidate; consume that BEGIN on the next poll. */
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
   if(s->preopened&&s->prepared_binding.product_experiment==1&&state(s)==READY&&
      !s->apm21_joint.begun&&rc==-EPIPE){
    struct k7_fifo_ref_status v;
    if(!k7_voice_session_fifo_status(s->session,&v)&&v.generation>s->floor&&!v.faults)break;
   }
#endif
   if(state(s)==ARMED&&!s->pending_begin&&rc==-EPIPE){
    struct k7_fifo_ref_status v;
    if(!k7_voice_session_fifo_status(s->session,&v)&&v.generation>s->floor&&v.faults)
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1027)
#else
freeze(s,rc)
#endif
;
   }else 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1028)
#else
freeze(s,rc)
#endif
;
   break;
  }
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
  s->trace.ref_instance=r.instance;s->trace.ref_epoch=r.epoch;
  s->trace.ref_generation=r.generation;s->trace.ref_kind=r.kind;
  s->trace.ref_first=r.first_enqueue;s->trace.ref_source=r.first_source;
  s->trace.ref_samples=r.samples;s->trace.ref_before=r.before_us;
  s->trace.ref_after=r.after_us;s->trace.ref_faults=r.faults;
  s->trace.ref_discarded=r.discarded_words;s->trace.ref_result=r.result;s->trace.ref_valid=true;
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(s->preopened){
   if(state(s)!=ACTIVE&&r.generation<=s->floor)continue;
   uint64_t mapped;int map=k7_apm21_live_worker_map_reference(&s->apm21_live,r.first_enqueue,&mapped);
   if(map==-EAGAIN){s->apm21_ref_pending=r;s->apm21_ref_wait=1;return;}
   if(map){(void)apm21_first_fault(s,map,32);freeze(s,map);return;}
   if(state(s)==READY&&r.kind==K7_FIFO_REF_BEGIN&&!s->apm21_joint.begun){
    int prompt=apm21_prompt_begin(s,&r);
    if(prompt==-EAGAIN){s->apm21_ref_pending=r;s->apm21_ref_wait=1;return;}
    if(prompt){(void)apm21_first_fault(s,prompt,41);freeze(s,prompt);return;}
   }
   s->apm21_ref_wait=0;r.first_enqueue=mapped;
  }
#endif
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(s->preopened&&s->apm21_joint.prompt_only){
   if(r.kind==K7_FIFO_REF_END)anchors(s);
   int prompt=k7_apm21_joint_record(&s->apm21_joint,&r);
   if(prompt){(void)apm21_first_fault(s,prompt,42);freeze(s,prompt);return;}
   continue;
  }
#endif
  if(state(s)==ARMED&&r.kind==K7_FIFO_REF_BEGIN&&r.generation>s->floor){
   s->begin=r;s->pending_begin=true;try_begin(s);if(s->pending_begin)break;
  }else if(state(s)==ACTIVE){
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
#if defined(K7_NEAR_CANCEL_ORDER_FIX) && K7_NEAR_CANCEL_ORDER_FIX
   /* The producer publishes TX_CLEAR before historical END. Those queues are
    * independent: END may become visible after this iteration's anchor pass.
    * Re-read real published anchors before validating END; bounded16 reads,
    * no fabricated clear, no identity/epoch or archive check is relaxed. */
   if(r.kind==K7_FIFO_REF_END){anchors(s);if(state(s)!=ACTIVE)break;}
#endif
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
   if(r.kind==K7_FIFO_REF_END)anchors(s);
   if(s->preopened&&s->apm21_joint.begun){int joint_rc=k7_apm21_joint_record(&s->apm21_joint,&r);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
    if(joint_rc<0)(void)apm21_first_fault(s,joint_rc,31);
#else
    (void)joint_rc;
#endif
   }
#endif
   rc=cc_record(s->cc,&r);
#else
   if(r.kind!=K7_FIFO_REF_PCM){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-EPIPE,1029)
#else
freeze(s,-EPIPE)
#endif
;continue;}
   struct k7_fifo_ref_status status;rc=k7_voice_session_fifo_status(s->session,&status);
   if(!rc)rc=k7_echo_diag_reference(s->pairer,&r,&status);
#endif
   if(rc)
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1030)
#else
freeze(s,rc)
#endif
;else paired(s);
  }
 }
}
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
/* Worker-only transition, called after platform closes route and PF source.
 * RX ring remains SPSC: do not reinitialize its producer counters. Old queued
 * MIC is discarded by the next real TX_START sample boundary. */
static int multiround_reset(struct k7_echo_runtime*s){
 struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
 struct k7_echo_reply_identity id;int rc=k7_echo_reply_snapshot(&id);if(rc)return rc;
 /* A reply may fail before any TX bind after the prior worker reset. Its
  * retired mailbox has no input/TX identity. Only that still-empty READY
  * window may be retried; a changed physical TX or any pending timeline is
  * not an empty reply and must retain the original strict checks. */
 bool empty=!id.bound&&!id.tx&&!id.input&&state(s)==READY&&
    !s->bridge_lease&&!s->cc&&!s->pending_begin&&!s->have_start&&
    !s->generation&&s->floor==v.tx_generation&&s->context.input_epoch==v.input_epoch;
 if(!s->near||!v.ready||!v.running||v.finished||v.stop_requested||!v.input_epoch||
    v.tx_state!=K7_VOICE_TX_IDLE||id.live||!id.retired_terminal||id.owner!=s->owner||
    (!empty&&(id.tx!=v.tx_generation||id.input!=v.input_epoch))||
    tail_state(s)==TAIL_ARMED||tail_state(s)==TAIL_ACTIVE)return -EBUSY;
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(s->preopened&&s->prepared_binding.product_experiment==1){
  /* The serial caller reached this command only after the original route
   * ready checks and synchronous PF/source/observer release. A real fault
   * cannot become a fresh turn merely by retiring its software identity. */
  if(s->apm21_joint.failure)return s->apm21_joint.failure;
  if(s->apm21_evidence.failure)return s->apm21_evidence.failure;
  if(s->apm21_live.stream.ingestion_fault)return -EPIPE;
  if(!empty){
   if(!s->apm21_joint.begun||!s->apm21_have_physical_cut||
      !s->apm21_joint.transport_verified||
      (s->reply.cancelled?!s->apm21_joint.hypothesis_enabled:!s->apm21_eof_verified))return -EAGAIN;
   if(s->apm21_live.round_serial==UINT64_MAX)return -EOVERFLOW;
   struct k7_apm21_round_ticket ticket={.serial=s->apm21_live.round_serial+1,
    .owner=s->owner,.dsp_epoch=s->apm21_live.epoch,.reply_lease=s->reply.reply,
    .retired_checks=K7_APM21_ROUND_RETIRED_CHECKS,.cut=s->apm21_physical_cut};
   rc=k7_apm21_live_worker_round_retire(&s->apm21_live,&ticket);if(rc)return rc;
  }
 }
#endif
#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
 capture_seal(s,PC_ROUND_END,0);
#endif
 report_round(s,s->finish_reason);
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
 if(s->preopened){int apm_rearm=k7_apm21_evidence_rearm(&s->apm21_evidence,s->owner);if(apm_rearm)return apm_rearm;s->apm21_diag_error_logged=false;}
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
 if(s->preopened&&s->prepared_binding.product_experiment==1){
  memset(&s->apm21_joint,0,sizeof(s->apm21_joint));
  memset(&s->apm21_unclaimed_clear,0,sizeof(s->apm21_unclaimed_clear));
  s->apm21_source_wait=s->apm21_ref_wait=s->apm21_idle_wait=s->apm21_have_physical_cut=s->apm21_eof_verified=0;
 }
#endif
#endif
#line 1101
 k7_echo_diag_invalidate(s->pairer);ne_invalidate(s->near);(void)diag_bridge_invalidate(s->bridge,-ECANCELED);
 cc_destroy(s->cc);s->cc=NULL;s->bridge_lease=0;s->lag_primed=false;
 memset(s->lag_pcm,0,sizeof(s->lag_pcm));memset(s->lag_zero,0,sizeof(s->lag_zero));
 s->floor=v.tx_generation;s->pending_begin=false;s->have_start=false;s->generation=0;
 s->anchor_count=0;s->finish_reason=0;s->head=s->count=0;
 s->round_peak=0;s->round_candidates=0;s->round_reported=false;
 memset(&s->reply,0,sizeof(s->reply));memset(&s->begin,0,sizeof(s->begin));memset(&s->start,0,sizeof(s->start));
 atomic_store(&s->frames,0);atomic_store_explicit(&s->state,READY,memory_order_release);return 0;
}
#endif
static void command(struct k7_echo_runtime *s){
 unsigned cmd=atomic_load_explicit(&s->command,memory_order_acquire);if(!cmd)return;
 int rc=0;unsigned st=state(s);
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
 if(cmd==100){rc=multiround_reset(s);}else
#endif
 if(cmd==CMD_ARM){
  struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
  if(st!=READY||!v.ready||v.finished||v.stop_requested||v.tx_state!=K7_VOICE_TX_IDLE)rc=-EBUSY;
  else if(atomic_load(&s->rx_faults)==UINT_MAX)rc=-EOVERFLOW;
  else{s->floor=v.tx_generation;s->faults_at_arm=atomic_load(&s->rx_faults);
   s->generation=0;s->anchor_count=0;s->finish_reason=0;s->zero_samples=0;
   s->pending_begin=false;s->have_start=false;atomic_store(&s->frames,0);
   atomic_store_explicit(&s->state,ARMED,memory_order_release);}
 }else if(cmd==CMD_TAIL_ARM){
  struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
  if(tail_state(s)!=TAIL_READY||!v.ready||!v.running||v.finished||v.stop_requested||v.tx_state!=K7_VOICE_TX_IDLE||!v.input_epoch)rc=-EBUSY;
  else if(atomic_load(&s->rx_faults)==UINT_MAX)rc=-EOVERFLOW;
  else{s->tail_epoch=v.input_epoch;s->tail_floor=v.tx_generation;s->tail_faults=atomic_load(&s->rx_faults);
   s->tail_generation=0;s->tail_anchor_epoch=0;s->tail_first=s->tail_next=s->tail_end=s->tail_boundary=0;s->tail_before_us=s->tail_after_us=0;
   s->tail_reason=0;s->tail_complete=false;atomic_store(&s->tail_samples,0);atomic_store_explicit(&s->tail_state,TAIL_ARMED,memory_order_release);}
 }else if(cmd==CMD_TAIL_CLEAR){
  if(tail_state(s)==TAIL_ARMED||tail_state(s)==TAIL_ACTIVE)rc=-EBUSY;
  else{atomic_store(&s->tail_samples,0);s->tail_generation=0;s->tail_complete=false;s->tail_reason=0;
   atomic_store_explicit(&s->tail_state,TAIL_READY,memory_order_release);}
 }
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 else if(cmd==CMD_VAD_STATUS){
  if(!s->vad)rc=-ENODEV;else rc=k7_raw_vad_get_stats(s->vad,&s->vad_reply);
 }
#endif
 else if(cmd==CMD_ABORT){
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-ECANCELED,1031)
#else
freeze(s,-ECANCELED)
#endif
;tail_freeze(s,-ECANCELED);}
 else if(cmd==CMD_CLEAR){
  if(st==ARMED||st==ACTIVE)rc=-EBUSY;
  else{atomic_store(&s->frames,0);s->anchor_count=0;s->generation=0;s->finish_reason=0;
   atomic_store_explicit(&s->state,READY,memory_order_release);}
 }else rc=-EINVAL;
 atomic_store(&s->command_result,rc);atomic_store_explicit(&s->command,CMD_NONE,memory_order_release);
}
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
enum { AP_PREPARING=1,AP_PREPARED,AP_ATTACH,AP_ATTACHED,AP_PAUSE,AP_PAUSED,AP_FAILED };
static int prepared_wait(struct k7_echo_runtime *s,unsigned pending)
{
 for(unsigned i=0;i<2000;i++){
  unsigned phase=atomic_load_explicit(&s->prepared_phase,memory_order_acquire);
  if(phase!=pending)return atomic_load(&s->prepared_result);
  if(atomic_load_explicit(&s->done,memory_order_acquire))return -EPIPE;
  pause_ms();
 }
 return -ETIMEDOUT; /* Non-NULL context and loan remain owned. */
}
/* True means original session-reading loop is allowed. An unattached or paused
 * prepared worker never reads a borrowed physical session or drains its tap. */
static int prepared_step(struct k7_echo_runtime *s)
{
 unsigned phase=atomic_load_explicit(&s->prepared_phase,memory_order_acquire);
 if(phase==AP_ATTACH){
  int rc=k7_apm21_live_worker_attach(&s->apm21_live,k7_voice_session_apm21_tap(s->attach_session));
  if(!rc)s->session=s->attach_session;
  atomic_store(&s->prepared_result,rc);
  atomic_store_explicit(&s->prepared_phase,rc?AP_FAILED:AP_ATTACHED,memory_order_release);
  phase=rc?AP_FAILED:AP_ATTACHED;
 }else if(phase==AP_PAUSE){
  int rc=k7_apm21_live_worker_pause(&s->apm21_live);
  atomic_store(&s->prepared_result,rc);
  atomic_store_explicit(&s->prepared_phase,rc?AP_FAILED:AP_PAUSED,memory_order_release);
  return 0;
 }
 return phase==AP_ATTACHED;
}
#endif
static void *worker(void *arg){
#line 1157
 struct k7_echo_runtime *s=arg;
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
 /* No verified profile/mapping ships. NULL drains evidence only and cannot
  * construct APM. A reviewed physical binding must be supplied here later. */
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(s->preopened){
  int rc=k7_apm21_live_worker_prepare(&s->apm21_live,s->owner,&s->prepared_binding);
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
  if(!rc)rc=k7_apm21_evidence_init(&s->apm21_evidence,s->owner,s->owner,s->near);
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
  if(!rc){s->apm21_evidence.packet_sink=apm21_product_packet;s->apm21_evidence.packet_sink_arg=s;}
#endif
  if(!rc)rc=k7_apm21_live_worker_set_output_sink(&s->apm21_live,k7_apm21_evidence_accept,&s->apm21_evidence);
#endif
#line 1164
  atomic_store(&s->prepared_result,rc);
  atomic_store_explicit(&s->prepared_phase,rc?AP_FAILED:AP_PREPARED,memory_order_release);
 }else
#endif
 (void)k7_apm21_live_worker_init(&s->apm21_live,k7_voice_session_apm21_tap(s->session),s->owner,NULL);
#line 1162
#endif
 while(!atomic_load(&s->stop)){
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  if(s->preopened&&!prepared_step(s)){pause_ms();continue;}
#endif
#line 1164
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
  struct k7_echo_reply_identity apm21_reply;
  uint64_t apm21_lease=0;
  if(!k7_echo_reply_snapshot(&apm21_reply)&&apm21_reply.live&&apm21_reply.bound&&!apm21_reply.cancelled&&apm21_reply.owner==s->owner)apm21_lease=apm21_reply.reply;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
  /* Cancellation keeps the OLD reply through its source tail. Only a newly
   * bound, non-cancelled next reply changes authority; snapshot failure/retired
   * old scope cannot relabel buffered samples as a new lease. */
  if(!s->preopened||apm21_lease)
#endif
  (void)k7_apm21_live_worker_reply(&s->apm21_live,apm21_lease);
#line 1169
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
  (void)k7_apm21_live_worker_drain(&s->apm21_live,s->preopened?32u:64u);
#else
  (void)k7_apm21_live_worker_drain(&s->apm21_live,64);
#endif
#line 1170
#endif

  command(s);
  if((state(s)==ARMED||state(s)==ACTIVE)&&atomic_load(&s->rx_faults)!=s->faults_at_arm)
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-ENOSPC,1032)
#else
freeze(s,-ENOSPC)
#endif
;
  if((tail_state(s)==TAIL_ARMED||tail_state(s)==TAIL_ACTIVE)&&atomic_load(&s->rx_faults)!=s->tail_faults)tail_freeze(s,-ENOSPC);
  if(tail_state(s)==TAIL_ARMED||tail_state(s)==TAIL_ACTIVE){
   struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
   if(!v.ready||!v.running||v.finished||v.stop_requested||v.input_epoch!=s->tail_epoch)tail_freeze(s,-EPIPE);
  }
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  if(state(s)==ACTIVE){int rc=continuity_poll(s);if(rc)
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,rc,1033)
#else
freeze(s,rc)
#endif
;}
#endif
  anchors(s);references(s);
  for(unsigned i=0;i<16;i++){struct k7_voice_frame f;int rc=k7_voice_ring_take(&s->rx,&f);if(rc<=0)break;microphone(s,&f);}
#if defined(K7_APM21_DIAGNOSTIC_EVIDENCE) && K7_APM21_DIAGNOSTIC_EVIDENCE
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
  try_begin(s);apm21_joint_sources(s);
  paired(s);if(s->preopened)apm21_diagnostic_pump(s);pause_ms();
#else
  try_begin(s);paired(s);if(s->preopened)apm21_diagnostic_pump(s);pause_ms();
#endif
#else
  try_begin(s);paired(s);pause_ms();
#endif
 }
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
 k7_apm21_live_worker_close(&s->apm21_live);
#endif
 
#if defined(K7_NEAR_FREEZE_TRACE) && K7_NEAR_FREEZE_TRACE
FREEZE_AT(s,-ECANCELED,1034)
#else
freeze(s,-ECANCELED)
#endif
;tail_freeze(s,-ECANCELED);atomic_store_explicit(&s->done,true,memory_order_release);return NULL;
}
static void free_runtime(struct k7_echo_runtime *s){
 if(!s)return;
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
 ne_destroy(s->near);
#if defined(K7_ECHO_CANCEL_CONTINUATION) && K7_ECHO_CANCEL_CONTINUATION
 cc_destroy(s->cc);
#endif
#endif
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 k7_raw_vad_destroy(s->vad);
#endif
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 diag_bridge_destroy(s->bridge);
#else
 (void)k7_aec_destroy(s->aec);
#endif
 k7_echo_diag_destroy(s->pairer);
 memset(s,0,sizeof(*s));free(s);
}
static int open_core(struct k7_voice_session *session,uint64_t owner,
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 struct k7_vad *model,k7_raw_vad_sink sink,void *sink_context,
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
 bool residual_mode,
#endif
#endif
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 const struct k7_apm21_live_binding *prepared_binding,
#endif
 struct k7_echo_runtime **out){
 if(!out)return -EINVAL;
#line 1237
 *out=NULL;if(!k7_echo_runtime_requested())return 0;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(prepared_binding){
  if(session||!owner||!model||!residual_mode||!k7_apm21_live_binding_valid(prepared_binding))return -EINVAL;
 }else
#endif
 if(!session||!owner||!k7_voice_session_fifo_enabled(session)||!k7_voice_session_anchors_enabled(session))return -EINVAL;
#line 1239
 int e=pthread_mutex_lock(&control);if(e)return -e;
 int rc=0;struct k7_echo_runtime *s=NULL;
 if(current){rc=-EBUSY;goto done;}
 s=calloc(1,sizeof(*s));if(!s){rc=-ENOMEM;goto done;}
 s->session=session;s->owner=owner;k7_voice_ring_init(&s->rx);
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(prepared_binding){
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
 atomic_init(&s->apm21_prompt_release,0);atomic_init(&s->apm21_prompt_retired,0);atomic_init(&s->apm21_prompt_error,0);
#endif
 s->preopened=1;s->prepared_binding=*prepared_binding;atomic_store(&s->prepared_phase,AP_PREPARING);}
#endif
#line 1244
 rc=k7_echo_diag_create(&s->pairer);
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 if(!rc)rc=diag_bridge_create(&s->bridge);
#else
 if(!rc)rc=k7_aec_create(NULL,&s->aec);
#endif

#ifdef K7_ECHO_RAW_VAD_OBSERVER
 if(!rc&&model){
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
  s->residual_mode=residual_mode;
  if(residual_mode){
   /* Diagnostic policy only. Calibration/near retention have not been granted. */
   const struct ne_policy policy={1000,.5,.01,.9f,3};
   rc=ne_create(model,&policy,&s->near);
  }else
#endif
  rc=k7_raw_vad_create(session,owner,model,&s->vad);
  s->vad_sink=sink;s->vad_context=sink_context;
 }
#endif
 if(rc)goto done;
 pthread_attr_t attr;e=pthread_attr_init(&attr);bool initialized=e==0;
 /* The pinned ARM MLAS SGEMM frame alone uses 71968 bytes. VAD runs
  * synchronously on this worker; retain headroom and verify target high-water. */
#ifdef K7_ECHO_RAW_VAD_OBSERVER
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(!e)e=pthread_attr_setstacksize(&attr,model?256u*1024u:32768u);
#else
 if(!e)e=pthread_attr_setstacksize(&attr,model?128u*1024u:32768u);
#endif
#line 1271
#else
 if(!e)e=pthread_attr_setstacksize(&attr,32768);
#endif
#ifdef __NuttX__
 if(!e)e=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
 if(!e)e=pthread_attr_setschedpolicy(&attr,SCHED_FIFO);
 struct sched_param priority={.sched_priority=80};
 if(!e)e=pthread_attr_setschedparam(&attr,&priority);
#ifdef CONFIG_SMP
 cpu_set_t cpus;CPU_ZERO(&cpus);
#if CONFIG_SMP_NCPUS > 7
 CPU_SET(7,&cpus);
#else
 CPU_SET(0,&cpus);
#endif
 if(!e)e=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);
#endif
#endif
 if(!e)e=pthread_create(&s->thread,&attr,worker,s);
 if(initialized)pthread_attr_destroy(&attr);
 if(e){rc=-e;goto done;}
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 current=s;*out=s;
 if(s->preopened)rc=prepared_wait(s,AP_PREPARING);
 s=NULL;
#else
 current=s;*out=s;s=NULL;
#endif
#line 1293
done:free_runtime(s);pthread_mutex_unlock(&control);return rc;
}
int k7_echo_runtime_open(struct k7_voice_session *session,uint64_t owner,struct k7_echo_runtime **out){
 return open_core(session,owner,
#ifdef K7_ECHO_RAW_VAD_OBSERVER
                  NULL,NULL,NULL,
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
                  false,
#endif
#endif
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
                  NULL,
#endif
                  out);
}
#ifdef K7_ECHO_RAW_VAD_OBSERVER
#line 1306
int k7_echo_runtime_open_vad(struct k7_voice_session *session,uint64_t owner,
 struct k7_vad *model,k7_raw_vad_sink sink,void *context,struct k7_echo_runtime **out){
 if(!model){if(out)*out=NULL;return -EINVAL;}
 return open_core(session,owner,model,sink,context,
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
                  false,
#endif
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
                  NULL,
#endif
                  out);
}
#endif
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
#line 1317
int k7_echo_runtime_open_residual_vad(struct k7_voice_session *session,uint64_t owner,
 struct k7_vad *model,struct k7_echo_runtime **out){
 if(!model){if(out)*out=NULL;return -EINVAL;}
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 return open_core(session,owner,model,NULL,NULL,true,NULL,out);
#else
 return open_core(session,owner,model,NULL,NULL,true,out);
#endif
#line 1321
}
#endif
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
int k7_echo_runtime_preopen_residual(uint64_t owner,struct k7_vad *model,
 const struct k7_apm21_live_binding *binding,struct k7_echo_runtime **out)
{
 if(!out||*out||!owner||!model)return -EINVAL;
 if(!k7_apm21_live_binding_valid(binding))return K7_APM21_STREAM_UNVERIFIED;
 if(!k7_echo_runtime_requested())return -ENODEV;
 return open_core(NULL,owner,model,NULL,NULL,true,binding,out);
}
int k7_echo_runtime_is_prepared(struct k7_echo_runtime *s){return s&&s->preopened;}
int k7_echo_runtime_attach_prepared(struct k7_echo_runtime *s,struct k7_voice_session *session,uint64_t owner)
{
 if(!s||!session||!owner)return -EINVAL;
 int err=pthread_mutex_lock(&control);if(err)return -err;
 int rc=0;
 if(s!=current||!s->preopened||s->owner!=owner||s->joined||atomic_load(&s->done))rc=-ESTALE;
 else if(atomic_load_explicit(&s->prepared_phase,memory_order_acquire)!=AP_PREPARED)rc=-EBUSY;
 else if(vv_voice_session_apm21_startup_owner_epoch(session)!=owner||!k7_voice_session_fifo_enabled(session)||!k7_voice_session_anchors_enabled(session))rc=-ESTALE;
 else {
  struct k7_voice_session_status status={0};k7_voice_session_status(session,&status);
  if(!status.running||status.finished||status.stop_requested)rc=-EPIPE;
  else{s->attach_session=session;atomic_store(&s->prepared_result,0);
   atomic_store_explicit(&s->prepared_phase,AP_ATTACH,memory_order_release);rc=prepared_wait(s,AP_ATTACH);}
 }
 pthread_mutex_unlock(&control);return rc;
}
int k7_echo_runtime_pause_prepared(struct k7_echo_runtime *s,uint64_t owner)
{
 if(!s)return 0;
 int err=pthread_mutex_lock(&control);if(err)return -err;
 int rc=0;unsigned phase=atomic_load_explicit(&s->prepared_phase,memory_order_acquire);
 if(s!=current||s->owner!=owner||s->joined)rc=-ESTALE;
 else if(!s->preopened||phase==AP_PAUSED)rc=0;
 /* Preparation failed before any physical session was attached. close()
  * already permits this state; do not make its model loan unreachable.
  * A failed attach retains attach_session and MUST NOT take this branch. */
 else if(phase==AP_FAILED&&!s->session&&!s->attach_session)rc=0;
 else if(phase!=AP_PREPARED&&phase!=AP_ATTACHED)rc=-EBUSY;
 else{atomic_store(&s->prepared_result,0);atomic_store_explicit(&s->prepared_phase,AP_PAUSE,memory_order_release);rc=prepared_wait(s,AP_PAUSE);}
 pthread_mutex_unlock(&control);return rc;
}
#endif
void k7_echo_runtime_tap(struct k7_echo_runtime *s,const struct k7_voice_frame *frame){
#line 1324
 if(!s||!frame||atomic_load(&s->stop))return;
 /* Exactly one tap producer; saturate rather than let loss identity wrap. */
 if(k7_voice_ring_put(&s->rx,frame)){
  unsigned faults=atomic_load(&s->rx_faults);
  if(faults!=UINT_MAX)atomic_store(&s->rx_faults,faults+1);
 }
}
int k7_echo_runtime_close(struct k7_echo_runtime **object){
 if(!object)return -EINVAL;
 struct k7_echo_runtime *s=*object;if(!s)return 0;
 int e=pthread_mutex_lock(&control);if(e)return -e;
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
 if(s->preopened){
  if(s!=current){pthread_mutex_unlock(&control);return -ESTALE;}
  unsigned phase=atomic_load_explicit(&s->prepared_phase,memory_order_acquire);
  if(s->session){
   struct k7_voice_session_status status={0};k7_voice_session_status(s->session,&status);
   if(phase!=AP_PAUSED){pthread_mutex_unlock(&control);return -EBUSY;}
   int retired=status.cleanup_error?status.cleanup_error:
    (!status.finished||!status.cleanup_ok)?-EBUSY:k7_voice_session_apm21_retirement_check(s->session);
   if(retired){pthread_mutex_unlock(&control);return retired;}
  }else if(s->attach_session||(phase!=AP_PREPARED&&phase!=AP_PAUSED&&phase!=AP_FAILED)){pthread_mutex_unlock(&control);return -EBUSY;}
 }
#endif
 atomic_store(&s->stop,true);
#line 1336
 if(!s->joined){e=pthread_join(s->thread,NULL);if(e){pthread_mutex_unlock(&control);return -e;}s->joined=true;}
 /* Caller already joined the sole tap producer. The frozen copy survives,
  * but no borrowed session/owner pointer escapes actual worker retirement. */
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 if(s->vad){
  (void)k7_raw_vad_get_stats(s->vad,&s->vad_reply);
  k7_raw_vad_destroy(s->vad);s->vad=NULL;s->vad_sink=NULL;s->vad_context=NULL;
 }
#endif
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
 ne_destroy(s->near);s->near=NULL;
#endif
 s->session=NULL;*object=NULL;pthread_mutex_unlock(&control);return 0;
}
static int request(struct k7_echo_runtime *s,unsigned cmd){
 if(!s||!s->session||s->joined||atomic_load(&s->done))return -ENODEV;
 if(atomic_load(&s->command))return -EBUSY;
 atomic_store_explicit(&s->command,cmd,memory_order_release);
 for(unsigned i=0;i<2000;i++){
  if(!atomic_load_explicit(&s->command,memory_order_acquire))return atomic_load(&s->command_result);
  if(atomic_load(&s->done))return -EPIPE;
  pause_ms();
 }
 return -ETIMEDOUT; /* command may still be pending; caller must inspect status */
}
#ifdef K7_ECHO_RAW_VAD_OBSERVER
int k7_echo_runtime_vad_stats(struct k7_echo_runtime*s,struct k7_raw_vad_stats*out){
 if(!s||!out)return -EINVAL;
 int e=pthread_mutex_lock(&control);if(e)return -e;
 int rc=s!=current?K7_VOICE_SESSION_STALE:s->joined?0:request(s,CMD_VAD_STATUS);
 if(!rc)*out=s->vad_reply;
 pthread_mutex_unlock(&control);return rc;
}
#endif
#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
int k7_echo_runtime_prompt_finish_owner(uint64_t owner,uint32_t generation){
 if(!owner||!generation)return -EINVAL;
 int e=pthread_mutex_lock(&control);if(e)return -e;struct k7_echo_runtime*s=current;int rc=0;
 if(!s||s->owner!=owner||s->joined||atomic_load(&s->done))rc=-ESTALE;
 else if(s->preopened&&s->prepared_binding.product_experiment==1){
  unsigned retired=atomic_load_explicit(&s->apm21_prompt_retired,memory_order_acquire);
  rc=atomic_load_explicit(&s->apm21_prompt_error,memory_order_acquire);
  if(!rc){
   struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
   struct k7_voice_tx_result tx;
   if(retired>generation||!v.running||!v.ready||v.finished||v.stop_requested||
      v.tx_state!=K7_VOICE_TX_IDLE||v.tx_generation!=generation||
      k7_voice_session_tx_result(s->session,generation,&tx)||tx.result||tx.discarded_words)rc=-ESTALE;
   else if(retired!=generation){unsigned pending=atomic_load_explicit(&s->apm21_prompt_release,memory_order_acquire);
    if(pending&&pending!=generation)rc=-ESTALE;
    else {atomic_store_explicit(&s->apm21_prompt_release,generation,memory_order_release);rc=-EAGAIN;}
   }
  }
 }
 pthread_mutex_unlock(&control);return rc;
}
#endif
int k7_echo_runtime_rearm_owner(uint64_t owner){
 if(!owner)return -EINVAL;
 int rc=pthread_mutex_lock(&control);if(rc)return -rc;
 struct k7_echo_runtime*s=current;
 rc=!s||s->owner!=owner||s->joined?-ESTALE:request(s,100);
 pthread_mutex_unlock(&control);return rc;
}
#endif
static uint32_t crc16_samples(const int16_t *pcm,unsigned n){
 uint32_t c=UINT32_MAX;for(unsigned i=0;i<n;i++)for(unsigned b=0;b<2;b++){
  c^=((uint16_t)pcm[i]>>(8*b))&255u;for(unsigned k=0;k<8;k++)c=(c>>1)^((0u-(c&1u))&UINT32_C(0xedb88320));
 }return ~c;
}
static int number(const char *s,unsigned *out){
 if(!s||!*s)return -EINVAL;
 unsigned n=0;for(;*s;s++){
  if(*s<'0'||*s>'9'||n>(UINT_MAX-(unsigned)(*s-'0'))/10)return -EINVAL;
  n=n*10+(unsigned)(*s-'0');
 }*out=n;return 0;
}

#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
static uint32_t capture_crc_byte(uint32_t c,unsigned byte){
 c^=byte;for(unsigned k=0;k<8;k++)c=(c>>1)^((0u-(c&1u))&UINT32_C(0xedb88320));return c;
}
static uint32_t capture_crc64(uint32_t c,uint64_t value){
 for(unsigned i=0;i<8;i++)c=capture_crc_byte(c,(unsigned)(value>>(i*8))&255u);
 return c;
}
/* Explicit LE scalar encoding; no padding, address or host endianness. */
static uint32_t capture_key(const struct k7_echo_runtime*s){
 uint32_t c=UINT32_MAX;const struct ne_identity*id=&s->capture.id;
#define PC_KEY(n) c=capture_crc64(c,(uint64_t)(n))
 PC_KEY(id->owner);PC_KEY(id->reference_instance);PC_KEY(id->reference_epoch);PC_KEY(id->clock_epoch);
 PC_KEY(id->dialogue_instance);PC_KEY(id->scope);PC_KEY(id->lease);PC_KEY(id->reply);PC_KEY(id->mapping_revision);
 PC_KEY(id->input_epoch);PC_KEY(id->tx_generation);PC_KEY(s->capture.mic_first);PC_KEY(s->capture.mic_next);
 PC_KEY(s->capture.ref_first);PC_KEY(s->capture.ref_next);PC_KEY(s->capture.first_serial);PC_KEY(s->capture.next_serial);
 PC_KEY(s->capture.frames);PC_KEY(s->capture.reason);PC_KEY((int64_t)s->capture.fault);PC_KEY(s->capture.zeros);
#undef PC_KEY
 return ~c;
}
static uint32_t capture_zero_crc(const struct k7_echo_runtime*s){
 uint32_t c=UINT32_MAX;for(unsigned i=0;i<s->capture.frames*320;i++){
  c=capture_crc_byte(c,(s->zeros[i/8]>>(i%8))&1u);c=capture_crc_byte(c,0);
 }return ~c;
}
static uint64_t capture_double_bits(double x){uint64_t u;memcpy(&u,&x,sizeof(u));return u;}
static uint32_t capture_float_bits(float x){uint32_t u;memcpy(&u,&x,sizeof(u));return u;}
static int capture_status(const struct k7_echo_runtime*s){
 unsigned cs=s?capture_state(s):PC_EMPTY;
 printf("PAIRED_CAPTURE state=%u ready=%u\n",cs,cs==PC_SEALED);
 if(cs!=PC_SEALED)return 0;
 const struct ne_identity*id=&s->capture.id;unsigned samples=s->capture.frames*320;
 printf("PAIRED_CAPTURE_SEALED key=%08lx owner=%llu ref_instance=%llu ref_epoch=%llu clock_epoch=%llu dialogue=%llu scope=%llu lease=%llu reply=%llu mapping=%llu input=%u tx=%u mic_first=%llu mic_next=%llu ref_first=%llu ref_next=%llu first_serial=%llu next_serial=%llu frames=%u samples=%u lag=39 reason=%u fault=%d zeros=%llu mic_crc=%08lx reference_crc=%08lx residual_crc=%08lx zero_crc=%08lx\n",
  (unsigned long)capture_key(s),(unsigned long long)id->owner,(unsigned long long)id->reference_instance,
  (unsigned long long)id->reference_epoch,(unsigned long long)id->clock_epoch,(unsigned long long)id->dialogue_instance,
  (unsigned long long)id->scope,(unsigned long long)id->lease,(unsigned long long)id->reply,
  (unsigned long long)id->mapping_revision,id->input_epoch,id->tx_generation,
  (unsigned long long)s->capture.mic_first,(unsigned long long)s->capture.mic_next,
  (unsigned long long)s->capture.ref_first,(unsigned long long)s->capture.ref_next,
  (unsigned long long)s->capture.first_serial,(unsigned long long)s->capture.next_serial,
  s->capture.frames,samples,s->capture.reason,s->capture.fault,(unsigned long long)s->capture.zeros,
  (unsigned long)crc16_samples(s->mic,samples),(unsigned long)crc16_samples(s->reference,samples),
  (unsigned long)crc16_samples(s->residual,samples),(unsigned long)capture_zero_crc(s));return 0;
}

static uint32_t capture_zero_page_crc(const struct k7_echo_runtime*s,unsigned first,unsigned count){
 uint32_t c=UINT32_MAX;for(unsigned i=first;i<first+count;i++){
  c=capture_crc_byte(c,(s->zeros[i/8]>>(i%8))&1u);c=capture_crc_byte(c,0);
 }return ~c;
}
static void capture_read_identity(const struct k7_echo_runtime*s,const char*kind,unsigned offset,unsigned count){
 const struct ne_identity*id=&s->capture.id;
 printf("PAIRED_CAPTURE_BEGIN key=%08lx kind=%s offset=%u count=%u owner=%llu ref_instance=%llu ref_epoch=%llu clock_epoch=%llu dialogue=%llu scope=%llu lease=%llu reply=%llu mapping=%llu input=%u tx=%u mic_first=%llu ref_first=%llu first_serial=%llu samples=%u\n",
  (unsigned long)capture_key(s),kind,offset,count,(unsigned long long)id->owner,
  (unsigned long long)id->reference_instance,(unsigned long long)id->reference_epoch,
  (unsigned long long)id->clock_epoch,(unsigned long long)id->dialogue_instance,
  (unsigned long long)id->scope,(unsigned long long)id->lease,(unsigned long long)id->reply,
  (unsigned long long)id->mapping_revision,id->input_epoch,id->tx_generation,
  (unsigned long long)s->capture.mic_first,(unsigned long long)s->capture.ref_first,
  (unsigned long long)s->capture.first_serial,s->capture.frames*320);
}
static int capture_dump(const struct k7_echo_runtime*s,int argc,char**argv){
 if(!s||capture_state(s)!=PC_SEALED)return -EBUSY;
 unsigned offset=0,n=0;int rc=number(argv[3],&offset);if(!rc)rc=number(argv[4],&n);
 bool zero=!strcmp(argv[2],"zero"),rows=!strcmp(argv[2],"rows"),bundle=!strcmp(argv[2],"bundle");
 const int16_t*pcm=!strcmp(argv[2],"mic")?s->mic:!strcmp(argv[2],"reference")?s->reference:!strcmp(argv[2],"residual")?s->residual:NULL;
 unsigned total=rows?s->capture.frames:s->capture.frames*320,limit=rows?16:bundle?12288:16384;
 if(!rc&&(argc!=5||!n||n>limit||offset>total||n>total-offset||(!pcm&&!zero&&!rows&&!bundle)))rc=-EINVAL;
 if(rc)return rc;
 uint32_t key=capture_key(s);capture_read_identity(s,argv[2],offset,n);
 if(rows){for(unsigned i=offset;i<offset+n;i++){
  const struct paired_capture_row*r=&s->capture.row[i];
  printf("PAIRED_CAPTURE_ROW key=%08lx i=%u mic_first=%llu ref_first=%llu serial=%llu model_rc=%d window=%llu window_first=%llu window_next=%llu p_bits=%08lx mic_power_bits=%016llx ref_power_bits=%016llx res_power_bits=%016llx correlation_bits=%016llx zeros=%u consecutive=%u candidate=%u clipped=%u\n",
   (unsigned long)key,i,(unsigned long long)r->mic_first,(unsigned long long)r->ref_first,
   (unsigned long long)r->serial,r->model_rc,(unsigned long long)r->window,(unsigned long long)r->window_first,
   (unsigned long long)r->window_next,(unsigned long)capture_float_bits(r->probability),
   (unsigned long long)capture_double_bits(r->mic_power),(unsigned long long)capture_double_bits(r->ref_power),
   (unsigned long long)capture_double_bits(r->res_power),(unsigned long long)capture_double_bits(r->correlation2),
   r->source_stop_zeros,r->consecutive,r->candidate,r->clipped);
 }}else for(unsigned stream=0;stream<(bundle?3u:1u);stream++){
  const char*kind=bundle?(const char*[]){"mic","reference","residual"}[stream]:argv[2];
  const int16_t*data=bundle?(const int16_t*[]){s->mic,s->reference,s->residual}[stream]:pcm;
  for(unsigned i=0;i<n;){unsigned count=n-i<512?n-i:512;
   printf("PAIRED_CAPTURE_PCM key=%08lx kind=%s offset=%u count=%u data=",(unsigned long)key,kind,offset+i,count);
   for(unsigned j=0;j<count;j++){unsigned at=offset+i+j;uint16_t v=zero?(s->zeros[at/8]>>(at%8))&1u:(uint16_t)data[at];printf("%02x%02x",v&255u,v>>8);}putchar('\n');i+=count;
  }
  printf("PAIRED_CAPTURE_BLOCK key=%08lx kind=%s offset=%u count=%u crc=%08lx\n",
   (unsigned long)key,kind,offset,n,(unsigned long)(zero?capture_zero_page_crc(s,offset,n):crc16_samples(data+offset,n)));
 }
 printf("PAIRED_CAPTURE_END key=%08lx kind=%s offset=%u count=%u streams=%u\n",(unsigned long)key,argv[2],offset,n,bundle?3u:rows?0u:1u);
 return 0;
}

#endif

int k7_echo_runtime_command(int argc,char **argv){
 int e=pthread_mutex_lock(&control);if(e)return 1;
 struct k7_echo_runtime *s=current;int rc=-EINVAL;
#if defined(K7_NEAR_PAIRED_CAPTURE) && K7_NEAR_PAIRED_CAPTURE
 if(argc==2&&!strcmp(argv[1],"echo-diag-paired-status"))rc=capture_status(s);
 else if(argc==5&&!strcmp(argv[1],"echo-diag-paired-dump"))rc=capture_dump(s,argc,argv);
 else
#endif
 if(argc==2&&!strcmp(argv[1],"echo-diag-enable")){
  rc=s&&s->session?-EBUSY:0;if(!rc)atomic_store(&requested,true);
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-disable")){
  rc=s&&s->session?-EBUSY:0;if(!rc)atomic_store(&requested,false);
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-tail-arm"))rc=request(s,CMD_TAIL_ARM);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-tail-clear"))rc=request(s,CMD_TAIL_CLEAR);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-arm"))rc=request(s,CMD_ARM);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-abort"))rc=request(s,CMD_ABORT);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-clear")){
  if(s&&!s->session&&s->joined){current=NULL;free_runtime(s);s=NULL;rc=0;}
  else rc=request(s,CMD_CLEAR);
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-status")){
  /* control prevents a NEW request while this CLI runs. A prior timed-out
   * command can still be executing on the worker. NONE is published release
   * only after all its writes; observe it BEFORE a fresh FROZEN state. */
  unsigned pending=s?atomic_load_explicit(&s->command,memory_order_acquire):0;
  unsigned st=s?state(s):READY,n=s?atomic_load(&s->frames)*320:0;
  printf("ECHO_DIAG requested=%u attached=%u state=%u samples=%u pending=%u hypothesis=1 alignment_verified=0 complete_turn=0\n",
   (unsigned)k7_echo_runtime_requested(),(unsigned)(s&&s->session),st,n,pending);
#if defined(K7_ECHO_RESIDUAL_EVIDENCE) && K7_ECHO_RESIDUAL_EVIDENCE
  if(s&&!pending&&st==FROZEN&&s->residual_mode)
   printf("ECHO_RESIDUAL_FROZEN processed_frames=%llu captured_frames=%u evidence_windows=%llu anchor_prefix_truncated=%u raw=0 residual=1 qualified=0 actions=0\n",
    (unsigned long long)s->processed_frames,atomic_load(&s->frames),
    (unsigned long long)s->evidence_windows,s->capture_anchor_overflow);
#endif
  if(s&&!pending&&st==FROZEN)printf("ECHO_DIAG_FROZEN owner=%llu generation=%u instance=%llu epoch=%u mic_first=%llu fifo_first=%llu zeros=%llu reason=%d anchors=%u mic_crc=%08lx reference_crc=%08lx residual_crc=%08lx\n",
   (unsigned long long)s->owner,s->generation,(unsigned long long)s->instance,s->ref_epoch,
   (unsigned long long)s->mic_first,(unsigned long long)s->fifo_first,(unsigned long long)s->zero_samples,
   s->finish_reason,s->anchor_count,(unsigned long)crc16_samples(s->mic,n),
   (unsigned long)crc16_samples(s->reference,n),(unsigned long)crc16_samples(s->residual,n));
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  if(s&&!pending&&st==FROZEN)printf("ECHO_CONTINUITY instance=%llu revision=%llu scope=%llu lease=%llu reply=%llu input=%u asr=%u tx=%u mic_next=%llu ref_next=%llu seals=%u cancel_observed_us=%llu cancel_processed_mic_next=%llu cancel_processed_ref_next=%llu hypothesis=1 qualified=0 actions=0\n",
   (unsigned long long)s->reply.instance,(unsigned long long)s->reply.revision,(unsigned long long)s->reply.scope_incarnation,
   (unsigned long long)s->reply.lease_serial,(unsigned long long)s->reply.reply,s->reply.input,s->reply.asr,s->reply.tx,
   (unsigned long long)s->processed_mic_next,(unsigned long long)s->processed_ref_next,s->seals,
   (unsigned long long)s->reply.cancel_observed_us,(unsigned long long)s->cancel_mic_next,(unsigned long long)s->cancel_ref_next);
#endif
  rc=0;
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-tail-status")){
  unsigned pending=s?atomic_load_explicit(&s->command,memory_order_acquire):0;
  unsigned st=s?tail_state(s):TAIL_READY,n=s?atomic_load(&s->tail_samples):0;
  printf("ECHO_TAIL state=%u samples=%u pending=%u raw=1 clean=0\n",st,n,pending);
  if(s&&!pending&&st==TAIL_FROZEN)printf("ECHO_TAIL_FROZEN owner=%llu generation=%u input_epoch=%u first=%llu next=%llu boundary=%llu before_us=%llu after_us=%llu complete=%u reason=%d crc=%08lx\n",
   (unsigned long long)s->owner,s->tail_generation,s->tail_epoch,(unsigned long long)s->tail_first,(unsigned long long)s->tail_next,
   (unsigned long long)s->tail_boundary,(unsigned long long)s->tail_before_us,(unsigned long long)s->tail_after_us,
   (unsigned)s->tail_complete,s->tail_reason,(unsigned long)crc16_samples(s->tail,n));
  rc=0;
 }else if(argc==4&&!strcmp(argv[1],"echo-diag-tail-dump")&&s){
  if(atomic_load_explicit(&s->command,memory_order_acquire)!=CMD_NONE)rc=-EBUSY;
  else if(tail_state(s)!=TAIL_FROZEN)rc=-EBUSY;
  else{unsigned offset=0,n=0,total=atomic_load(&s->tail_samples);
   rc=number(argv[2],&offset);if(!rc)rc=number(argv[3],&n);
   if(!rc&&(!n||n>1024||offset>total||n>total-offset))rc=-EINVAL;
   if(!rc)for(unsigned i=0;i<n;){unsigned count=n-i<64?n-i:64;
    printf("ECHO_TAIL_PCM owner=%llu generation=%u input_epoch=%u first=%llu offset=%u count=%u data=",
     (unsigned long long)s->owner,s->tail_generation,s->tail_epoch,(unsigned long long)s->tail_first,offset+i,count);
    for(unsigned j=0;j<count;j++){uint16_t v=(uint16_t)s->tail[offset+i+j];printf("%02x%02x",v&255u,v>>8);}putchar('\n');i+=count;
   }
  }
 }else if(s&&atomic_load_explicit(&s->command,memory_order_acquire)!=CMD_NONE&&
   ((argc==5&&!strcmp(argv[1],"echo-diag-dump"))||
    (argc==4&&!strcmp(argv[1],"echo-diag-anchors"))))rc=-EBUSY;
 else if(argc==5&&!strcmp(argv[1],"echo-diag-dump")&&s&&state(s)==FROZEN){
  unsigned offset=0,n=0;rc=number(argv[3],&offset);if(!rc)rc=number(argv[4],&n);
  unsigned total=atomic_load(&s->frames)*320;
  bool zero=!strcmp(argv[2],"zero");
  const int16_t *pcm=!strcmp(argv[2],"mic")?s->mic:!strcmp(argv[2],"reference")?s->reference:!strcmp(argv[2],"residual")?s->residual:NULL;
  if(!rc&&(!n||n>1024||offset>total||n>total-offset||(!pcm&&!zero)))rc=-EINVAL;
  if(!rc)for(unsigned i=0;i<n;){unsigned count=n-i<64?n-i:64;
   printf("ECHO_DIAG_PCM kind=%s owner=%llu generation=%u instance=%llu epoch=%u offset=%u count=%u data=",argv[2],
    (unsigned long long)s->owner,s->generation,(unsigned long long)s->instance,s->ref_epoch,offset+i,count);
   for(unsigned j=0;j<count;j++){unsigned at=offset+i+j;uint16_t v=zero?(s->zeros[at/8]>>(at%8))&1u:(uint16_t)pcm[at];printf("%02x%02x",v&255u,v>>8);}
   putchar('\n');i+=count;
  }
 }else if(argc==4&&!strcmp(argv[1],"echo-diag-anchors")&&s&&state(s)==FROZEN){
  unsigned offset=0,n=0;rc=number(argv[2],&offset);if(!rc)rc=number(argv[3],&n);
  if(!rc&&(!n||n>32||offset>s->anchor_count||n>s->anchor_count-offset))rc=-EINVAL;
  if(!rc)for(unsigned i=offset;i<offset+n;i++){const struct k7_voice_anchor *a=&s->anchors[i];
   printf("ECHO_DIAG_ANCHOR owner=%llu instance=%llu reference_epoch=%u i=%u event=%u epoch=%u generation=%u before_us=%llu after_us=%llu rx=%llu tx=%u rx_words=%u tx_words=%u discarded=%u flags=%u result=%d\n",
   (unsigned long long)s->owner,(unsigned long long)s->instance,s->ref_epoch,i,a->event,a->epoch,a->generation,(unsigned long long)a->before_us,(unsigned long long)a->after_us,
   (unsigned long long)a->rx_samples,a->tx_queued_samples,a->rx_fifo_words,a->tx_fifo_words,a->discarded_words,a->flags,a->result);
  }
 }
 printf("ECHO_DIAG_COMMAND result=%d\n",rc);fflush(stdout);pthread_mutex_unlock(&control);return rc?1:0;
}

#else
#include "echo_diag_runtime.h"
#include "echo_diag_worker.h"
#include "k7_aec.h"
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include "diag_bridge.h"
#include "k7_echo_reply_identity.h"
#endif

#ifdef K7_ECHO_RAW_VAD_OBSERVER
#include "raw_vad_observer.h"
#endif
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#define CAPTURE_FRAMES 1500u
#define CAPTURE_SAMPLES (CAPTURE_FRAMES*320u)
#define ANCHORS 4096u
#define HISTORY 64u
#define TAIL_PRE 5120u
#define TAIL_POST 16000u
#define TAIL_SAMPLES (67u*320u)
enum { TAIL_READY=0,TAIL_ARMED,TAIL_ACTIVE,TAIL_FROZEN };
enum { READY=0, ARMED, ACTIVE, FROZEN };
enum { CMD_NONE=0,CMD_ARM,CMD_ABORT,CMD_CLEAR,CMD_TAIL_ARM,CMD_TAIL_CLEAR
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 ,CMD_VAD_STATUS
#endif
 };
struct k7_echo_runtime {
 struct k7_voice_session *session; uint64_t owner;
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 struct k7_raw_vad *vad;
 k7_raw_vad_sink vad_sink;void *vad_context;
 struct k7_raw_vad_stats vad_reply;
 unsigned vad_rx_faults;uint64_t vad_tap_rejected;
#endif
 pthread_t thread; bool joined;
 atomic_bool stop,done;
 atomic_uint state,command,frames,rx_faults;
 atomic_int command_result;
 atomic_uint tail_state,tail_samples;
 uint32_t tail_epoch,tail_floor,tail_generation,tail_anchor_epoch;unsigned tail_faults;
 uint64_t tail_first,tail_next,tail_end,tail_boundary,tail_before_us,tail_after_us;
 int tail_reason;bool tail_complete;
 int16_t tail[TAIL_SAMPLES];
 struct k7_voice_ring rx;
 struct k7_echo_diag *pairer;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 struct diag_bridge *bridge;
 struct k7_echo_reply_identity reply;
 struct vv_confirmed_wake_context context;
 struct k7_aec_continuity_identity continuity;
 uint64_t bridge_lease,processed_mic_next,processed_ref_next,cancel_mic_next,cancel_ref_next;
 unsigned seals;
#else
 struct k7_aec *aec;
#endif

 struct k7_voice_frame history[HISTORY]; unsigned head,count;
 uint32_t history_epoch,anchor_epoch,floor,generation,ref_epoch;
 unsigned faults_at_arm,anchor_count; int finish_reason;
 uint64_t instance,mic_first,fifo_first,zero_samples;
 struct k7_voice_anchor start,anchors[ANCHORS]; bool have_start,pending_begin;
 struct k7_fifo_ref_record begin;
 int16_t mic[CAPTURE_SAMPLES],reference[CAPTURE_SAMPLES],residual[CAPTURE_SAMPLES];
 uint8_t zeros[CAPTURE_SAMPLES/8];
};
static pthread_mutex_t control=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool requested=ATOMIC_VAR_INIT(false);
static struct k7_echo_runtime *current;
static void pause_ms(void){struct timespec t={0,1000000};(void)nanosleep(&t,NULL);}
bool k7_echo_runtime_requested(void){return atomic_load(&requested);}
static unsigned state(struct k7_echo_runtime *s){return atomic_load_explicit(&s->state,memory_order_acquire);}
static void freeze(struct k7_echo_runtime *s,int why){
 unsigned st=state(s);if(st!=ACTIVE&&st!=ARMED)return;
 k7_echo_diag_invalidate(s->pairer);
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 (void)diag_bridge_invalidate(s->bridge,why);
#else
 (void)k7_aec_invalidate(s->aec,K7_AEC_BAD_REFERENCE);
#endif

 s->pending_begin=false;s->finish_reason=why;
 atomic_store_explicit(&s->state,FROZEN,memory_order_release);
}
/* Independent RAW snapshot. No render zeros, AEC reset or clean authority.
 * Worker owns all non-atomic fields; release FROZEN publishes immutable data. */
static unsigned tail_state(struct k7_echo_runtime*s){return atomic_load_explicit(&s->tail_state,memory_order_acquire);}
static void tail_freeze(struct k7_echo_runtime*s,int why){
 unsigned st=tail_state(s);if(st!=TAIL_ARMED&&st!=TAIL_ACTIVE)return;
 s->tail_reason=why;s->tail_complete=!why&&s->tail_next==s->tail_end&&atomic_load(&s->tail_samples)>=TAIL_PRE+TAIL_POST;
 atomic_store_explicit(&s->tail_state,TAIL_FROZEN,memory_order_release);
}
static void tail_frame(struct k7_echo_runtime*s,const struct k7_voice_frame*f){
 unsigned st=tail_state(s);if(st!=TAIL_ARMED&&st!=TAIL_ACTIVE)return;
 if(f->samples!=320||f->flags||f->epoch!=s->tail_epoch||f->first_sample>UINT64_MAX-320){tail_freeze(s,-EPIPE);return;}
 if(st!=TAIL_ACTIVE)return;
 if(f->first_sample+320<=s->tail_next)return; /* already copied from history */
 if(f->first_sample!=s->tail_next||s->tail_next>s->tail_end||s->tail_end-s->tail_next<320){tail_freeze(s,-EPIPE);return;}
 unsigned n=atomic_load(&s->tail_samples);
 if(n>TAIL_SAMPLES-320){tail_freeze(s,-ENOSPC);return;}
 memcpy(s->tail+n,f->pcm,320*sizeof(int16_t));s->tail_next+=320;
 atomic_store(&s->tail_samples,n+320);
 if(s->tail_next==s->tail_end){
  /* Anchors are drained before each MIC batch. An actual new TX/input epoch
   * can already exist while its queued event has not been consumed. Recheck
   * the pinned owner immediately before committing a successful snapshot.
   * A change later than the last sample may conservatively reject RAW. */
  struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
  if(atomic_load(&s->rx_faults)!=s->tail_faults)tail_freeze(s,-ENOSPC);
  else if(!v.ready||!v.running||v.finished||v.stop_requested||
          v.input_epoch!=s->tail_epoch||v.tx_generation!=s->tail_generation||
          v.anchor_epoch!=s->tail_anchor_epoch)tail_freeze(s,-EPIPE);
  else tail_freeze(s,0);
 }
}
static void tail_anchor(struct k7_echo_runtime*s,const struct k7_voice_anchor*a){
 unsigned st=tail_state(s);if(st!=TAIL_ARMED&&st!=TAIL_ACTIVE)return;
 if(a->flags||a->after_us<a->before_us||a->epoch!=k7_voice_session_anchor_epoch(s->session)){tail_freeze(s,-EPIPE);return;}
 if(st==TAIL_ACTIVE){
  if(a->event==K7_VOICE_ANCHOR_AMP_OFF&&a->result){tail_freeze(s,-EPIPE);return;}
  if(a->event==K7_VOICE_ANCHOR_TX_START&&a->generation!=s->tail_generation)tail_freeze(s,-EPIPE);
  return;
 }
 if(a->event!=K7_VOICE_ANCHOR_AMP_OFF||a->generation<=s->tail_floor)return;
 if(a->result||a->generation!=k7_voice_session_tx_generation(s->session)||
   k7_voice_session_input_epoch(s->session)!=s->tail_epoch){tail_freeze(s,-EPIPE);return;}
 uint64_t base=a->rx_samples-a->rx_samples%320;
 if(base<TAIL_PRE||a->rx_samples>UINT64_MAX-TAIL_POST-319){tail_freeze(s,-ENODATA);return;}
 uint64_t first=base-TAIL_PRE,end=((a->rx_samples+TAIL_POST+319)/320)*320;
 if(end<first||end-first>TAIL_SAMPLES||!s->count||s->history_epoch!=s->tail_epoch){tail_freeze(s,-ENODATA);return;}
 const struct k7_voice_frame*old=&s->history[s->head];
 const struct k7_voice_frame*last=&s->history[(s->head+s->count-1)%HISTORY];
 if(first<old->first_sample||first>last->first_sample){tail_freeze(s,-ENODATA);return;}
 s->tail_generation=a->generation;s->tail_first=s->tail_next=first;s->tail_end=end;
 s->tail_anchor_epoch=a->epoch;s->tail_boundary=a->rx_samples;s->tail_before_us=a->before_us;s->tail_after_us=a->after_us;
 atomic_store(&s->tail_samples,0);atomic_store_explicit(&s->tail_state,TAIL_ACTIVE,memory_order_release);
 for(unsigned i=0;i<s->count&&tail_state(s)==TAIL_ACTIVE;i++){
  const struct k7_voice_frame*f=&s->history[(s->head+i)%HISTORY];
  if(f->first_sample>=first)tail_frame(s,f);
 }
}
static int snapshot(struct k7_echo_runtime *s,struct k7_fifo_ref_status *r,
 struct k7_echo_diag_mic_id *m){
 struct k7_voice_session_status st; k7_voice_session_status(s->session,&st);
 if(!st.ready||!st.running||st.finished||st.stop_requested||!st.input_epoch)return -EPIPE;
 int rc=k7_voice_session_fifo_status(s->session,r);if(rc)return rc;
 *m=(struct k7_echo_diag_mic_id){s->owner,st.input_epoch};return 0;
}
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
/* Worker-only: mailbox lock is released inside snapshot before ANY session
 * access or DSP. No read-only identity can grant clean/model/alignment proof. */
static bool reply_same(const struct k7_echo_reply_identity*a,const struct k7_echo_reply_identity*b){
 return a->instance==b->instance&&a->scope_incarnation==b->scope_incarnation&&
 a->lease_serial==b->lease_serial&&a->owner==b->owner&&a->reply==b->reply&&
 a->asr==b->asr&&a->input==b->input&&a->tx==b->tx;
}
static int continuity_poll(struct k7_echo_runtime*s){
 if(state(s)!=ACTIVE)return 0;
 struct k7_echo_reply_identity v;int rc=k7_echo_reply_snapshot(&v);if(rc)return rc;
 if(!v.live||!v.bound||!reply_same(&v,&s->reply))return -EPIPE;
 /* Recheck real RX/reference before recording even a software boundary. */
 struct k7_voice_session_status st;k7_voice_session_status(s->session,&st);
 if(!st.ready||!st.running||st.finished||st.stop_requested||st.input_epoch!=v.input||
 !k7_voice_session_fifo_matches(s->session,s->continuity.reference_instance,
  (uint32_t)s->continuity.reference_epoch,v.tx)||
 k7_voice_session_anchor_epoch(s->session)!=s->continuity.clock_epoch)return -EPIPE;
 if(v.revision==s->reply.revision)return v.cancelled==s->reply.cancelled?0:-EPIPE;
 if(s->reply.revision==UINT64_MAX||v.revision!=s->reply.revision+1||
 !v.cancelled||s->reply.cancelled||!v.cancel_observed_us)return -EPIPE;
 /* The indices are processed-next at worker observation, not the physical
  * cancel instant, DAC tail or a qualified onset interval. No frame, no seal. */
 rc=diag_bridge_boundary(s->bridge,s->bridge_lease,&s->context,
  s->processed_mic_next,s->processed_ref_next,&s->bridge_lease);
 if(rc)return rc;
 s->cancel_mic_next=s->processed_mic_next;s->cancel_ref_next=s->processed_ref_next;
 s->seals++;s->reply=v;return 0;
}
static int continuity_begin(struct k7_echo_runtime*s,struct k7_echo_diag_mic_id m){
 struct k7_echo_reply_identity v;int rc=k7_echo_reply_snapshot(&v);if(rc)return rc;
 if(!v.live||!v.bound||v.cancelled||v.owner!=m.owner||v.input!=m.epoch||v.tx!=s->begin.generation)return -EPIPE;
 s->reply=v;s->context=(struct vv_confirmed_wake_context){.owner_epoch=m.owner,.input_epoch=m.epoch,
 .scope_incarnation=v.scope_incarnation,.lease_serial=v.lease_serial,.asr_epoch=v.asr,
 .reply_id=v.reply,.tx_generation=v.tx};
 s->continuity=(struct k7_aec_continuity_identity){.owner_epoch=m.owner,.mapping_epoch=v.instance,
 .reference_instance=s->begin.instance,.reference_epoch=s->begin.epoch,.input_epoch=m.epoch,
 .clock_epoch=s->start.epoch,.render_generation=v.tx};
 s->processed_mic_next=s->start.rx_samples;s->processed_ref_next=s->begin.first_enqueue;
 s->seals=0;s->cancel_mic_next=s->cancel_ref_next=0;
 return diag_bridge_begin(s->bridge,&s->context,&s->continuity,
  s->processed_mic_next,s->processed_ref_next,&s->bridge_lease);
}

#endif
static void paired(struct k7_echo_runtime *s){
 for(unsigned n=0;n<8&&state(s)==ACTIVE;n++){
  struct k7_fifo_ref_status r;struct k7_echo_diag_mic_id m;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  int rc=continuity_poll(s);
  if(!rc)rc=snapshot(s,&r,&m);
  struct k7_echo_diag_pair pair;
#else
  int rc=snapshot(s,&r,&m);struct k7_echo_diag_pair pair;
#endif
  if(!rc)rc=k7_echo_diag_pop(s->pairer,&r,m,&pair);
  if(!rc)return;
  if(rc<0){freeze(s,rc);return;}
  unsigned frame=atomic_load(&s->frames);
  if(frame>=CAPTURE_FRAMES){freeze(s,-EFBIG);return;}
  int16_t out[320];
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  struct k7_voice_frame f={.first_sample=pair.mic_first,.epoch=pair.mic_epoch,.samples=320},res;
  memcpy(f.pcm,pair.microphone,sizeof(f.pcm));
  rc=diag_bridge_process(s->bridge,s->bridge_lease,&s->continuity,&f,pair.reference,pair.fifo_first,&res);
  if(!rc)memcpy(out,res.pcm,sizeof(out));
#else
  rc=k7_aec_diagnostic_process(s->aec,pair.microphone,pair.reference,320,out);
#endif

  /* Recheck actual reference revocation after potentially slow DSP. */
  if(!rc&&!k7_voice_session_fifo_matches(s->session,pair.reference_instance,
       pair.reference_epoch,pair.generation))rc=-EPIPE;
  if(!rc&&k7_voice_session_input_epoch(s->session)!=pair.mic_epoch)rc=-EPIPE;
  if(rc){freeze(s,rc);return;}
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  struct k7_aec_stats stats;struct k7_aec_continuity_status cs;
  rc=diag_bridge_status(s->bridge,&cs,&stats);
  if(!rc&&(!cs.active||cs.qualified))rc=-EPROTO;
  if(!rc){s->processed_mic_next=cs.mic_next_sample;s->processed_ref_next=cs.reference_next_sample;}
  if(!rc)rc=continuity_poll(s);
#else
  struct k7_aec_stats stats;rc=k7_aec_get_stats(s->aec,&stats);
#endif

  if(rc||stats.ready||stats.frames||!pair.hypothesis_only){freeze(s,-EPROTO);return;}
  if(!frame){s->mic_first=pair.mic_first;s->fifo_first=pair.fifo_first;}
  memcpy(s->mic+frame*320,pair.microphone,sizeof(pair.microphone));
  memcpy(s->reference+frame*320,pair.reference,sizeof(pair.reference));
  memcpy(s->residual+frame*320,out,sizeof(out));
  memcpy(s->zeros+frame*40,pair.explicit_zero_bits,40);
  for(unsigned i=0;i<320;i++)s->zero_samples+=(pair.explicit_zero_bits[i/8]>>(i%8))&1u;
  atomic_store_explicit(&s->frames,frame+1,memory_order_release);
 }
}
#ifdef K7_ECHO_RAW_VAD_OBSERVER
/* Low-priority diagnostic worker only. The sole KWS producer still performs
 * the same bounded copy. RAW probability is not a barge-in event. */
static bool vad_recoverable(enum k7_raw_vad_failure f){
 return f==K7_RAW_VAD_FRAME||f==K7_RAW_VAD_CONTINUITY||
        f==K7_RAW_VAD_OWNER||f==K7_RAW_VAD_STALE||f==K7_RAW_VAD_TAP_LOSS;
}
static void vad_frame(struct k7_echo_runtime*s,const struct k7_voice_frame*f){
 if(!s->vad)return;
 unsigned faults=atomic_load(&s->rx_faults);
 if(faults!=s->vad_rx_faults){
  k7_raw_vad_invalidate(s->vad,K7_RAW_VAD_TAP_LOSS);
  s->vad_rx_faults=faults;
  if(faults!=UINT_MAX)(void)k7_raw_vad_rearm(s->vad);
 }
 struct k7_raw_vad_stats st;(void)k7_raw_vad_get_stats(s->vad,&st);
 if(!st.armed)return;
 struct k7_raw_vad_report report;int rc=k7_raw_vad_feed(s->vad,f,&report);
 if(atomic_load(&s->rx_faults)!=faults){
  k7_raw_vad_invalidate(s->vad,K7_RAW_VAD_TAP_LOSS);
  if(s->vad_tap_rejected!=UINT64_MAX)s->vad_tap_rejected++;
  return; /* No callback from an inference overlapping a lost tap. */
 }
 if(rc<0){
  (void)k7_raw_vad_get_stats(s->vad,&st);
  if(vad_recoverable(st.last_failure))(void)k7_raw_vad_rearm(s->vad);
 }else if(rc==1&&s->vad_sink)s->vad_sink(s->vad_context,&report);
}
#endif
static void microphone(struct k7_echo_runtime *s,const struct k7_voice_frame *f){
 tail_frame(s,f);
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 vad_frame(s,f);
#endif
 if(f->samples!=320||f->flags||!f->epoch||f->first_sample>UINT64_MAX-320){
  freeze(s,-EPROTO);s->count=s->head=0;return;
 }
 if(s->count){struct k7_voice_frame *last=&s->history[(s->head+s->count-1)%HISTORY];
  if(f->epoch!=last->epoch||f->first_sample!=last->first_sample+320){
   freeze(s,-EPIPE);tail_freeze(s,-EPIPE);s->head=s->count=0;
  }
 }
 s->history_epoch=f->epoch;
 if(s->count==HISTORY){s->head=(s->head+1)%HISTORY;s->count--;}
 s->history[(s->head+s->count)%HISTORY]=*f;s->count++;
 if(state(s)==ACTIVE){
  int rc=k7_echo_diag_microphone(s->pairer,(struct k7_echo_diag_mic_id){s->owner,f->epoch},
    f->first_sample,f->pcm,f->samples);
  if(rc)freeze(s,rc);else paired(s);
 }
}
static void try_begin(struct k7_echo_runtime *s){
 if(!s->pending_begin||state(s)!=ARMED||!s->have_start)return;
 if(s->start.generation!=s->begin.generation)return;
 struct k7_fifo_ref_status r;struct k7_echo_diag_mic_id m;int rc=snapshot(s,&r,&m);
 if(!rc&&(!s->count||s->history_epoch!=m.epoch||s->start.flags||s->start.result||
    s->start.epoch!=k7_voice_session_anchor_epoch(s->session)||
    s->start.after_us<s->start.before_us))rc=-EPIPE;
 /* This aligns software indices to TX_START's RX snapshot only. It is NOT
  * a DAC timestamp, acoustic delay estimate, or ALIGNMENT_VERIFIED assertion. */
 if(!rc)rc=k7_echo_diag_begin(s->pairer,&s->begin,&r,m,
                            s->begin.first_enqueue,s->start.rx_samples);
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 if(!rc)rc=continuity_begin(s,m);
#else
 if(!rc)rc=k7_aec_diagnostic_reset(s->aec);
#endif

 if(rc){freeze(s,rc);return;}
 s->generation=s->begin.generation;s->instance=s->begin.instance;s->ref_epoch=s->begin.epoch;
 s->anchors[0]=s->start;s->anchor_count=1;s->pending_begin=false;
 atomic_store_explicit(&s->state,ACTIVE,memory_order_release);
 for(unsigned i=0;i<s->count&&state(s)==ACTIVE;i++){
  const struct k7_voice_frame *f=&s->history[(s->head+i)%HISTORY];
  rc=k7_echo_diag_microphone(s->pairer,m,f->first_sample,f->pcm,f->samples);
  if(rc)freeze(s,rc);else paired(s);
 }
}
static void anchors(struct k7_echo_runtime *s){
 for(unsigned i=0;i<16;i++){
  struct k7_voice_anchor a;int rc=k7_voice_session_take_anchor(s->session,&a);
  if(!rc)break;
  if(rc<0){freeze(s,rc);tail_freeze(s,rc);s->have_start=false;break;}
  tail_anchor(s,&a);
  if(a.flags||a.epoch!=k7_voice_session_anchor_epoch(s->session)){
   freeze(s,-EPIPE);s->have_start=false;continue;
  }
  if(a.event==K7_VOICE_ANCHOR_TX_START){s->start=a;s->have_start=true;}
  if(state(s)==ACTIVE){
   if(s->anchor_count==ANCHORS){freeze(s,-ENOSPC);continue;}
   s->anchors[s->anchor_count++]=a;
  }
 }
}
static void references(struct k7_echo_runtime *s){
 try_begin(s);
 if(s->pending_begin){
  if(!k7_voice_session_fifo_matches(s->session,s->begin.instance,s->begin.epoch,s->begin.generation))freeze(s,-EPIPE);
  return;
 }
 for(unsigned i=0;i<16;i++){
  struct k7_fifo_ref_record r;int rc=k7_voice_session_take_fifo_reference(s->session,&r);
  if(!rc)break;
  if(rc<0){
   /* take announces every new reference epoch with EPIPE, including its
    * first healthy BEGIN. Before selecting a turn there is no old mapping
    * to invalidate; consume that BEGIN on the next poll. */
   if(state(s)==ARMED&&!s->pending_begin&&rc==-EPIPE){
    struct k7_fifo_ref_status v;
    if(!k7_voice_session_fifo_status(s->session,&v)&&v.generation>s->floor&&v.faults)freeze(s,rc);
   }else freeze(s,rc);
   break;
  }
  if(state(s)==ARMED&&r.kind==K7_FIFO_REF_BEGIN&&r.generation>s->floor){
   s->begin=r;s->pending_begin=true;try_begin(s);if(s->pending_begin)break;
  }else if(state(s)==ACTIVE){
   if(r.kind!=K7_FIFO_REF_PCM){freeze(s,-EPIPE);continue;}
   struct k7_fifo_ref_status status;rc=k7_voice_session_fifo_status(s->session,&status);
   if(!rc)rc=k7_echo_diag_reference(s->pairer,&r,&status);
   if(rc)freeze(s,rc);else paired(s);
  }
 }
}
static void command(struct k7_echo_runtime *s){
 unsigned cmd=atomic_load_explicit(&s->command,memory_order_acquire);if(!cmd)return;
 int rc=0;unsigned st=state(s);
 if(cmd==CMD_ARM){
  struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
  if(st!=READY||!v.ready||v.finished||v.stop_requested||v.tx_state!=K7_VOICE_TX_IDLE)rc=-EBUSY;
  else if(atomic_load(&s->rx_faults)==UINT_MAX)rc=-EOVERFLOW;
  else{s->floor=v.tx_generation;s->faults_at_arm=atomic_load(&s->rx_faults);
   s->generation=0;s->anchor_count=0;s->finish_reason=0;s->zero_samples=0;
   s->pending_begin=false;s->have_start=false;atomic_store(&s->frames,0);
   atomic_store_explicit(&s->state,ARMED,memory_order_release);}
 }else if(cmd==CMD_TAIL_ARM){
  struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
  if(tail_state(s)!=TAIL_READY||!v.ready||!v.running||v.finished||v.stop_requested||v.tx_state!=K7_VOICE_TX_IDLE||!v.input_epoch)rc=-EBUSY;
  else if(atomic_load(&s->rx_faults)==UINT_MAX)rc=-EOVERFLOW;
  else{s->tail_epoch=v.input_epoch;s->tail_floor=v.tx_generation;s->tail_faults=atomic_load(&s->rx_faults);
   s->tail_generation=0;s->tail_anchor_epoch=0;s->tail_first=s->tail_next=s->tail_end=s->tail_boundary=0;s->tail_before_us=s->tail_after_us=0;
   s->tail_reason=0;s->tail_complete=false;atomic_store(&s->tail_samples,0);atomic_store_explicit(&s->tail_state,TAIL_ARMED,memory_order_release);}
 }else if(cmd==CMD_TAIL_CLEAR){
  if(tail_state(s)==TAIL_ARMED||tail_state(s)==TAIL_ACTIVE)rc=-EBUSY;
  else{atomic_store(&s->tail_samples,0);s->tail_generation=0;s->tail_complete=false;s->tail_reason=0;
   atomic_store_explicit(&s->tail_state,TAIL_READY,memory_order_release);}
 }
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 else if(cmd==CMD_VAD_STATUS){
  if(!s->vad)rc=-ENODEV;else rc=k7_raw_vad_get_stats(s->vad,&s->vad_reply);
 }
#endif
 else if(cmd==CMD_ABORT){freeze(s,-ECANCELED);tail_freeze(s,-ECANCELED);}
 else if(cmd==CMD_CLEAR){
  if(st==ARMED||st==ACTIVE)rc=-EBUSY;
  else{atomic_store(&s->frames,0);s->anchor_count=0;s->generation=0;s->finish_reason=0;
   atomic_store_explicit(&s->state,READY,memory_order_release);}
 }else rc=-EINVAL;
 atomic_store(&s->command_result,rc);atomic_store_explicit(&s->command,CMD_NONE,memory_order_release);
}
static void *worker(void *arg){
 struct k7_echo_runtime *s=arg;
 while(!atomic_load(&s->stop)){
  command(s);
  if((state(s)==ARMED||state(s)==ACTIVE)&&atomic_load(&s->rx_faults)!=s->faults_at_arm)freeze(s,-ENOSPC);
  if((tail_state(s)==TAIL_ARMED||tail_state(s)==TAIL_ACTIVE)&&atomic_load(&s->rx_faults)!=s->tail_faults)tail_freeze(s,-ENOSPC);
  if(tail_state(s)==TAIL_ARMED||tail_state(s)==TAIL_ACTIVE){
   struct k7_voice_session_status v;k7_voice_session_status(s->session,&v);
   if(!v.ready||!v.running||v.finished||v.stop_requested||v.input_epoch!=s->tail_epoch)tail_freeze(s,-EPIPE);
  }
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  if(state(s)==ACTIVE){int rc=continuity_poll(s);if(rc)freeze(s,rc);}
#endif
  anchors(s);references(s);
  for(unsigned i=0;i<16;i++){struct k7_voice_frame f;int rc=k7_voice_ring_take(&s->rx,&f);if(rc<=0)break;microphone(s,&f);}
  try_begin(s);paired(s);pause_ms();
 }
 freeze(s,-ECANCELED);tail_freeze(s,-ECANCELED);atomic_store_explicit(&s->done,true,memory_order_release);return NULL;
}
static void free_runtime(struct k7_echo_runtime *s){
 if(!s)return;
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 k7_raw_vad_destroy(s->vad);
#endif
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 diag_bridge_destroy(s->bridge);
#else
 (void)k7_aec_destroy(s->aec);
#endif
 k7_echo_diag_destroy(s->pairer);
 memset(s,0,sizeof(*s));free(s);
}
static int open_core(struct k7_voice_session *session,uint64_t owner,
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 struct k7_vad *model,k7_raw_vad_sink sink,void *sink_context,
#endif
 struct k7_echo_runtime **out){
 if(!out)return -EINVAL;
 *out=NULL;if(!k7_echo_runtime_requested())return 0;
 if(!session||!owner||!k7_voice_session_fifo_enabled(session)||!k7_voice_session_anchors_enabled(session))return -EINVAL;
 int e=pthread_mutex_lock(&control);if(e)return -e;
 int rc=0;struct k7_echo_runtime *s=NULL;
 if(current){rc=-EBUSY;goto done;}
 s=calloc(1,sizeof(*s));if(!s){rc=-ENOMEM;goto done;}
 s->session=session;s->owner=owner;k7_voice_ring_init(&s->rx);
 rc=k7_echo_diag_create(&s->pairer);
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
 if(!rc)rc=diag_bridge_create(&s->bridge);
#else
 if(!rc)rc=k7_aec_create(NULL,&s->aec);
#endif

#ifdef K7_ECHO_RAW_VAD_OBSERVER
 if(!rc&&model){
  rc=k7_raw_vad_create(session,owner,model,&s->vad);
  s->vad_sink=sink;s->vad_context=sink_context;
 }
#endif
 if(rc)goto done;
 pthread_attr_t attr;e=pthread_attr_init(&attr);bool initialized=e==0;
 /* The pinned ARM MLAS SGEMM frame alone uses 71968 bytes. VAD runs
  * synchronously on this worker; retain headroom and verify target high-water. */
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 if(!e)e=pthread_attr_setstacksize(&attr,model?128u*1024u:32768u);
#else
 if(!e)e=pthread_attr_setstacksize(&attr,32768);
#endif
#ifdef __NuttX__
 if(!e)e=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
 if(!e)e=pthread_attr_setschedpolicy(&attr,SCHED_FIFO);
 struct sched_param priority={.sched_priority=80};
 if(!e)e=pthread_attr_setschedparam(&attr,&priority);
#ifdef CONFIG_SMP
 cpu_set_t cpus;CPU_ZERO(&cpus);
#if CONFIG_SMP_NCPUS > 7
 CPU_SET(7,&cpus);
#else
 CPU_SET(0,&cpus);
#endif
 if(!e)e=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);
#endif
#endif
 if(!e)e=pthread_create(&s->thread,&attr,worker,s);
 if(initialized)pthread_attr_destroy(&attr);
 if(e){rc=-e;goto done;}
 current=s;*out=s;s=NULL;
done:free_runtime(s);pthread_mutex_unlock(&control);return rc;
}
int k7_echo_runtime_open(struct k7_voice_session *session,uint64_t owner,struct k7_echo_runtime **out){
 return open_core(session,owner,
#ifdef K7_ECHO_RAW_VAD_OBSERVER
                  NULL,NULL,NULL,
#endif
                  out);
}
#ifdef K7_ECHO_RAW_VAD_OBSERVER
int k7_echo_runtime_open_vad(struct k7_voice_session *session,uint64_t owner,
 struct k7_vad *model,k7_raw_vad_sink sink,void *context,struct k7_echo_runtime **out){
 if(!model){if(out)*out=NULL;return -EINVAL;}
 return open_core(session,owner,model,sink,context,out);
}
#endif
void k7_echo_runtime_tap(struct k7_echo_runtime *s,const struct k7_voice_frame *frame){
 if(!s||!frame||atomic_load(&s->stop))return;
 /* Exactly one tap producer; saturate rather than let loss identity wrap. */
 if(k7_voice_ring_put(&s->rx,frame)){
  unsigned faults=atomic_load(&s->rx_faults);
  if(faults!=UINT_MAX)atomic_store(&s->rx_faults,faults+1);
 }
}
int k7_echo_runtime_close(struct k7_echo_runtime **object){
 if(!object)return -EINVAL;
 struct k7_echo_runtime *s=*object;if(!s)return 0;
 int e=pthread_mutex_lock(&control);if(e)return -e;
 atomic_store(&s->stop,true);
 if(!s->joined){e=pthread_join(s->thread,NULL);if(e){pthread_mutex_unlock(&control);return -e;}s->joined=true;}
 /* Caller already joined the sole tap producer. The frozen copy survives,
  * but no borrowed session/owner pointer escapes actual worker retirement. */
#ifdef K7_ECHO_RAW_VAD_OBSERVER
 if(s->vad){
  (void)k7_raw_vad_get_stats(s->vad,&s->vad_reply);
  k7_raw_vad_destroy(s->vad);s->vad=NULL;s->vad_sink=NULL;s->vad_context=NULL;
 }
#endif
 s->session=NULL;*object=NULL;pthread_mutex_unlock(&control);return 0;
}
static int request(struct k7_echo_runtime *s,unsigned cmd){
 if(!s||!s->session||s->joined||atomic_load(&s->done))return -ENODEV;
 if(atomic_load(&s->command))return -EBUSY;
 atomic_store_explicit(&s->command,cmd,memory_order_release);
 for(unsigned i=0;i<2000;i++){
  if(!atomic_load_explicit(&s->command,memory_order_acquire))return atomic_load(&s->command_result);
  if(atomic_load(&s->done))return -EPIPE;
  pause_ms();
 }
 return -ETIMEDOUT; /* command may still be pending; caller must inspect status */
}
#ifdef K7_ECHO_RAW_VAD_OBSERVER
int k7_echo_runtime_vad_stats(struct k7_echo_runtime*s,struct k7_raw_vad_stats*out){
 if(!s||!out)return -EINVAL;
 int e=pthread_mutex_lock(&control);if(e)return -e;
 int rc=s!=current?K7_VOICE_SESSION_STALE:s->joined?0:request(s,CMD_VAD_STATUS);
 if(!rc)*out=s->vad_reply;
 pthread_mutex_unlock(&control);return rc;
}
#endif
static uint32_t crc16_samples(const int16_t *pcm,unsigned n){
 uint32_t c=UINT32_MAX;for(unsigned i=0;i<n;i++)for(unsigned b=0;b<2;b++){
  c^=((uint16_t)pcm[i]>>(8*b))&255u;for(unsigned k=0;k<8;k++)c=(c>>1)^((0u-(c&1u))&UINT32_C(0xedb88320));
 }return ~c;
}
static int number(const char *s,unsigned *out){
 if(!s||!*s)return -EINVAL;
 unsigned n=0;for(;*s;s++){
  if(*s<'0'||*s>'9'||n>(UINT_MAX-(unsigned)(*s-'0'))/10)return -EINVAL;
  n=n*10+(unsigned)(*s-'0');
 }*out=n;return 0;
}
int k7_echo_runtime_command(int argc,char **argv){
 int e=pthread_mutex_lock(&control);if(e)return 1;
 struct k7_echo_runtime *s=current;int rc=-EINVAL;
 if(argc==2&&!strcmp(argv[1],"echo-diag-enable")){
  rc=s&&s->session?-EBUSY:0;if(!rc)atomic_store(&requested,true);
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-disable")){
  rc=s&&s->session?-EBUSY:0;if(!rc)atomic_store(&requested,false);
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-tail-arm"))rc=request(s,CMD_TAIL_ARM);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-tail-clear"))rc=request(s,CMD_TAIL_CLEAR);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-arm"))rc=request(s,CMD_ARM);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-abort"))rc=request(s,CMD_ABORT);
 else if(argc==2&&!strcmp(argv[1],"echo-diag-clear")){
  if(s&&!s->session&&s->joined){current=NULL;free_runtime(s);s=NULL;rc=0;}
  else rc=request(s,CMD_CLEAR);
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-status")){
  /* control prevents a NEW request while this CLI runs. A prior timed-out
   * command can still be executing on the worker. NONE is published release
   * only after all its writes; observe it BEFORE a fresh FROZEN state. */
  unsigned pending=s?atomic_load_explicit(&s->command,memory_order_acquire):0;
  unsigned st=s?state(s):READY,n=s?atomic_load(&s->frames)*320:0;
  printf("ECHO_DIAG requested=%u attached=%u state=%u samples=%u pending=%u hypothesis=1 alignment_verified=0 complete_turn=0\n",
   (unsigned)k7_echo_runtime_requested(),(unsigned)(s&&s->session),st,n,pending);
  if(s&&!pending&&st==FROZEN)printf("ECHO_DIAG_FROZEN owner=%llu generation=%u instance=%llu epoch=%u mic_first=%llu fifo_first=%llu zeros=%llu reason=%d anchors=%u mic_crc=%08lx reference_crc=%08lx residual_crc=%08lx\n",
   (unsigned long long)s->owner,s->generation,(unsigned long long)s->instance,s->ref_epoch,
   (unsigned long long)s->mic_first,(unsigned long long)s->fifo_first,(unsigned long long)s->zero_samples,
   s->finish_reason,s->anchor_count,(unsigned long)crc16_samples(s->mic,n),
   (unsigned long)crc16_samples(s->reference,n),(unsigned long)crc16_samples(s->residual,n));
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  if(s&&!pending&&st==FROZEN)printf("ECHO_CONTINUITY instance=%llu revision=%llu scope=%llu lease=%llu reply=%llu input=%u asr=%u tx=%u mic_next=%llu ref_next=%llu seals=%u cancel_observed_us=%llu cancel_processed_mic_next=%llu cancel_processed_ref_next=%llu hypothesis=1 qualified=0 actions=0\n",
   (unsigned long long)s->reply.instance,(unsigned long long)s->reply.revision,(unsigned long long)s->reply.scope_incarnation,
   (unsigned long long)s->reply.lease_serial,(unsigned long long)s->reply.reply,s->reply.input,s->reply.asr,s->reply.tx,
   (unsigned long long)s->processed_mic_next,(unsigned long long)s->processed_ref_next,s->seals,
   (unsigned long long)s->reply.cancel_observed_us,(unsigned long long)s->cancel_mic_next,(unsigned long long)s->cancel_ref_next);
#endif
  rc=0;
 }else if(argc==2&&!strcmp(argv[1],"echo-diag-tail-status")){
  unsigned pending=s?atomic_load_explicit(&s->command,memory_order_acquire):0;
  unsigned st=s?tail_state(s):TAIL_READY,n=s?atomic_load(&s->tail_samples):0;
  printf("ECHO_TAIL state=%u samples=%u pending=%u raw=1 clean=0\n",st,n,pending);
  if(s&&!pending&&st==TAIL_FROZEN)printf("ECHO_TAIL_FROZEN owner=%llu generation=%u input_epoch=%u first=%llu next=%llu boundary=%llu before_us=%llu after_us=%llu complete=%u reason=%d crc=%08lx\n",
   (unsigned long long)s->owner,s->tail_generation,s->tail_epoch,(unsigned long long)s->tail_first,(unsigned long long)s->tail_next,
   (unsigned long long)s->tail_boundary,(unsigned long long)s->tail_before_us,(unsigned long long)s->tail_after_us,
   (unsigned)s->tail_complete,s->tail_reason,(unsigned long)crc16_samples(s->tail,n));
  rc=0;
 }else if(argc==4&&!strcmp(argv[1],"echo-diag-tail-dump")&&s){
  if(atomic_load_explicit(&s->command,memory_order_acquire)!=CMD_NONE)rc=-EBUSY;
  else if(tail_state(s)!=TAIL_FROZEN)rc=-EBUSY;
  else{unsigned offset=0,n=0,total=atomic_load(&s->tail_samples);
   rc=number(argv[2],&offset);if(!rc)rc=number(argv[3],&n);
   if(!rc&&(!n||n>1024||offset>total||n>total-offset))rc=-EINVAL;
   if(!rc)for(unsigned i=0;i<n;){unsigned count=n-i<64?n-i:64;
    printf("ECHO_TAIL_PCM owner=%llu generation=%u input_epoch=%u first=%llu offset=%u count=%u data=",
     (unsigned long long)s->owner,s->tail_generation,s->tail_epoch,(unsigned long long)s->tail_first,offset+i,count);
    for(unsigned j=0;j<count;j++){uint16_t v=(uint16_t)s->tail[offset+i+j];printf("%02x%02x",v&255u,v>>8);}putchar('\n');i+=count;
   }
  }
 }else if(s&&atomic_load_explicit(&s->command,memory_order_acquire)!=CMD_NONE&&
   ((argc==5&&!strcmp(argv[1],"echo-diag-dump"))||
    (argc==4&&!strcmp(argv[1],"echo-diag-anchors"))))rc=-EBUSY;
 else if(argc==5&&!strcmp(argv[1],"echo-diag-dump")&&s&&state(s)==FROZEN){
  unsigned offset=0,n=0;rc=number(argv[3],&offset);if(!rc)rc=number(argv[4],&n);
  unsigned total=atomic_load(&s->frames)*320;
  bool zero=!strcmp(argv[2],"zero");
  const int16_t *pcm=!strcmp(argv[2],"mic")?s->mic:!strcmp(argv[2],"reference")?s->reference:!strcmp(argv[2],"residual")?s->residual:NULL;
  if(!rc&&(!n||n>1024||offset>total||n>total-offset||(!pcm&&!zero)))rc=-EINVAL;
  if(!rc)for(unsigned i=0;i<n;){unsigned count=n-i<64?n-i:64;
   printf("ECHO_DIAG_PCM kind=%s owner=%llu generation=%u instance=%llu epoch=%u offset=%u count=%u data=",argv[2],
    (unsigned long long)s->owner,s->generation,(unsigned long long)s->instance,s->ref_epoch,offset+i,count);
   for(unsigned j=0;j<count;j++){unsigned at=offset+i+j;uint16_t v=zero?(s->zeros[at/8]>>(at%8))&1u:(uint16_t)pcm[at];printf("%02x%02x",v&255u,v>>8);}
   putchar('\n');i+=count;
  }
 }else if(argc==4&&!strcmp(argv[1],"echo-diag-anchors")&&s&&state(s)==FROZEN){
  unsigned offset=0,n=0;rc=number(argv[2],&offset);if(!rc)rc=number(argv[3],&n);
  if(!rc&&(!n||n>32||offset>s->anchor_count||n>s->anchor_count-offset))rc=-EINVAL;
  if(!rc)for(unsigned i=offset;i<offset+n;i++){const struct k7_voice_anchor *a=&s->anchors[i];
   printf("ECHO_DIAG_ANCHOR owner=%llu instance=%llu reference_epoch=%u i=%u event=%u epoch=%u generation=%u before_us=%llu after_us=%llu rx=%llu tx=%u rx_words=%u tx_words=%u discarded=%u flags=%u result=%d\n",
   (unsigned long long)s->owner,(unsigned long long)s->instance,s->ref_epoch,i,a->event,a->epoch,a->generation,(unsigned long long)a->before_us,(unsigned long long)a->after_us,
   (unsigned long long)a->rx_samples,a->tx_queued_samples,a->rx_fifo_words,a->tx_fifo_words,a->discarded_words,a->flags,a->result);
  }
 }
 printf("ECHO_DIAG_COMMAND result=%d\n",rc);fflush(stdout);pthread_mutex_unlock(&control);return rc?1:0;
}

#endif
