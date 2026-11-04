/* Private synthetic ARM diagnostic; no audio device, cloud, VAD or affinity. */
#if defined(K7_APM21_OFFLINE_DIAG) && K7_APM21_OFFLINE_DIAG
#include "k7_apm21_consumer.h"
#include "vv_native_service.h"
#include "vv_speech_owner.h"
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DIAG_FRAMES 128u
#define DIAG_SAMPLES (DIAG_FRAMES * K7_APM21_FRAME_SAMPLES)
#define DIAG_DUMP_MAX 512u
#define DIAG_RESERVE (16u * 1024u * 1024u)
#define DIAG_SEED UINT32_C(0x59291db3)
static atomic_flag busy = ATOMIC_FLAG_INIT;
static atomic_bool cleanup_retained;
static const char lease;
static _Alignas(K7_APM21_CONTEXT_ALIGNMENT) unsigned char context_storage[K7_APM21_CONTEXT_BYTES];
static int16_t output_pcm[DIAG_SAMPLES];
static const int16_t zero_render[K7_APM21_FRAME_SAMPLES];
static int16_t mic_frame[K7_APM21_FRAME_SAMPLES];
static uint64_t run_sequence;
static struct {
  unsigned sealed, mode, completed;
  uint64_t run;
  uint32_t input_crc, output_crc;
  char key[25];
} capture;

struct heap_samples { struct mallinfo before, constructed, first, processed, destroyed; size_t used_max, free_min; unsigned valid; };
struct measurements { uint64_t construct_us, first_us, total_us, maximum_us, destroy_us; unsigned slow_calls; };

static uint64_t diag_now_us(void)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
      t.tv_nsec >= 1000000000 || (uint64_t)t.tv_sec > (UINT64_MAX-999999u)/1000000u) return UINT64_MAX;
  return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
static int elapsed(uint64_t begin, uint64_t end, uint64_t *out)
{
  if (begin == UINT64_MAX || end == UINT64_MAX || end < begin) return -EIO;
  *out = end-begin; return 0;
}
static uint32_t crc_byte(uint32_t crc, unsigned char byte)
{
  crc ^= byte;
  for (unsigned i=0; i<8; ++i) crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
  return crc;
}
static uint32_t crc_sample(uint32_t crc, int16_t sample)
{
  uint16_t v=(uint16_t)sample;
  return crc_byte(crc_byte(crc,(unsigned char)v),(unsigned char)(v>>8));
}
static struct mallinfo heap_sample(struct heap_samples *h)
{
  struct mallinfo m=mallinfo();
  if ((size_t)m.uordblks>h->used_max) h->used_max=(size_t)m.uordblks;
  if ((size_t)m.fordblks<h->free_min) h->free_min=(size_t)m.fordblks;
  return m;
}
static int native_idle(void)
{
  struct vv_native_service_status s;
  int rc=vv_native_service_status(&s);
  return rc?rc:s.running?-EBUSY:0;
}
static int16_t signal_sample(unsigned mode, uint32_t index, uint32_t *rng)
{
  if (mode==1u) return (index==0u || index==487u || index==5295u || index==12951u)?12000:0;
  *rng ^= *rng<<13; *rng ^= *rng>>17; *rng ^= *rng<<5;
  /* Balanced fixed-amplitude PRBS; no signed right-shift dependency. */
  return (*rng&1u)?4096:-4096;
}
static void print_status(void)
{
  printf("APM_OFFLINE_STATUS sealed=%u mode=%u run=%" PRIu64 " completed=%u key=%s samples=%u rate=16000 frame=160 input_crc=%08" PRIx32 " output_crc=%08" PRIx32 " output_axis=api_call_order nominal_support=0 algorithmic_delay_samples=-1 physical_delay_measured=0\n",
         capture.sealed,capture.mode,capture.run,capture.completed,capture.sealed?capture.key:"none",capture.sealed?DIAG_SAMPLES:0u,capture.input_crc,capture.output_crc);
}
static void print_heap(const char *stage, const struct mallinfo *m)
{
  printf("APM_OFFLINE_HEAP stage=%s used=%zu free=%zu maxfree=%zu global_maxused=%zu\n",
         stage,(size_t)m->uordblks,(size_t)m->fordblks,(size_t)m->mxordblk,(size_t)m->usmblks);
}
static int run_case(unsigned mode)
{
  struct heap_samples h={0}; struct measurements t={0};
  struct k7_apm21_frame_report report={0};
  const struct k7_apm21_timing timing={1,0,K7_APM21_DELAY_OFFLINE_HYPOTHESIS};
  k7_apm21_context *context=NULL;
  uint32_t rng=DIAG_SEED, input_crc=UINT32_MAX, output_crc=UINT32_MAX;
  int32_t upstream=0; int rc=native_idle(); unsigned held=0;
  uint64_t begin=0,end=0;
  if (!rc) { rc=vv_speech_owner_native_claim(&lease); held=!rc; }
  if (!rc) rc=native_idle();
  if (!rc && run_sequence==UINT64_MAX) rc=-EOVERFLOW;
  if (rc) goto done;
  memset(&capture,0,sizeof(capture)); capture.mode=mode; capture.run=++run_sequence;
  h.free_min=SIZE_MAX; h.before=heap_sample(&h); h.valid|=1u;
  if ((size_t)h.before.fordblks<DIAG_RESERVE) { rc=-ENOMEM; goto done; }
  begin=diag_now_us();
  if (begin==UINT64_MAX) { rc=-EIO; goto done; }
  rc=k7_apm21_construct(context_storage,sizeof(context_storage),capture.run,&context,&upstream);
  end=diag_now_us();
  if (elapsed(begin,end,&t.construct_us) && !rc) rc=-EIO;
  h.constructed=heap_sample(&h); h.valid|=2u;
  if (rc) goto destroy;
  for (unsigned frame=0; frame<DIAG_FRAMES; ++frame) {
    if ((rc=native_idle())) break;
    if (!vv_speech_owner_native_available(&lease)) { rc=-ESTALE; break; }
    for (unsigned j=0; j<K7_APM21_FRAME_SAMPLES; ++j)
      mic_frame[j]=signal_sample(mode,frame*K7_APM21_FRAME_SAMPLES+j,&rng);
    begin=diag_now_us(); if (begin==UINT64_MAX) { rc=-EIO; break; }
    rc=k7_apm21_process_10ms(context,capture.run,frame,zero_render,mic_frame,&timing,
                            output_pcm+frame*K7_APM21_FRAME_SAMPLES,&report);
    end=diag_now_us(); uint64_t duration=0;
    if (elapsed(begin,end,&duration) && !rc) rc=-EIO;
    if (rc) break;
    if (report.stream_epoch!=capture.run || report.frame_ordinal!=frame ||
        report.stage!=K7_APM21_COMPLETE || report.upstream_rc ||
        report.api_output_samples!=K7_APM21_FRAME_SAMPLES || report.nominal_support_qualified ||
        report.algorithmic_output_delay_samples!=-1 || report.delay_source!=K7_APM21_DELAY_OFFLINE_HYPOTHESIS ||
        report.supplied_buffer_delay_ms!=0) { rc=-EPROTO; break; }
    if (duration>UINT64_MAX-t.total_us) { rc=-EOVERFLOW; break; }
    if (!frame) t.first_us=duration;
    t.total_us+=duration; if (duration>t.maximum_us) t.maximum_us=duration;
    if (duration>10000u) ++t.slow_calls;
    for (unsigned j=0; j<K7_APM21_FRAME_SAMPLES; ++j) {
      input_crc=crc_sample(input_crc,mic_frame[j]);
      output_crc=crc_sample(output_crc,output_pcm[frame*K7_APM21_FRAME_SAMPLES+j]);
    }
    ++capture.completed;
    struct mallinfo m=heap_sample(&h); if (!frame) { h.first=m; h.valid|=4u; }
    if ((size_t)m.fordblks<DIAG_RESERVE) { rc=-ENOMEM; break; }
  }
  h.processed=heap_sample(&h); h.valid|=8u;
destroy:
  begin=diag_now_us(); k7_apm21_destroy(context); context=NULL; end=diag_now_us();
  if (elapsed(begin,end,&t.destroy_us) && !rc) rc=-EIO;
  h.destroyed=heap_sample(&h); h.valid|=16u;
  capture.input_crc=input_crc^UINT32_MAX; capture.output_crc=output_crc^UINT32_MAX;
  if (!rc && capture.completed!=DIAG_FRAMES) rc=-ENODATA;
  if (!rc) {
    int n=snprintf(capture.key,sizeof(capture.key),"%016" PRIx64 "%08" PRIx32,capture.run,capture.output_crc);
    if (n!=24) rc=-EOVERFLOW; else capture.sealed=1;
  }
done:
  if (context) k7_apm21_destroy(context);
  if (held) vv_speech_owner_native_release(&lease);
  printf("APM_OFFLINE_RESULT rc=%d construct_upstream=%d last_stage=%d last_upstream=%d completed=%u requested=128 mode=%u run=%" PRIu64 " construct_us=%" PRIu64 " first_call_us=%" PRIu64 " process_total_us=%" PRIu64 " max_call_us=%" PRIu64 " destroy_us=%" PRIu64 " calls_over10ms=%u clock=monotonic cpu_affinity_changed=0 actual_ADC=0 actual_render=0 VAD=0 cloud=0 gimbal=0 synthetic_only=1\n",
         rc,upstream,report.stage,report.upstream_rc,capture.completed,mode,capture.run,t.construct_us,t.first_us,t.total_us,t.maximum_us,t.destroy_us,t.slow_calls);
  if (h.valid) {
    if (h.valid&1u) print_heap("before",&h.before);
    if (h.valid&2u) print_heap("after_construct_attempt",&h.constructed);
    if (h.valid&4u) print_heap("first_call",&h.first);
    if (h.valid&8u) print_heap("after_process",&h.processed);
    if (h.valid&16u) print_heap("after_destroy",&h.destroyed);
    printf("APM_OFFLINE_MEMORY sampled_used_max=%zu sampled_free_min=%zu allocator_peak_verified=0 per_frame_allocation_free_verified=0 wrapper_static_PCM_bytes=%zu\n",h.used_max,h.free_min,sizeof(output_pcm));
  }
  if (held) print_status();
  return rc?1:0;
}
/* APM runs only on this explicit large-stack temporary worker. No production
 * audio worker, scheduler priority or CPU affinity is changed. Static job
 * storage stays alive even if an unexpected join failure prevents reclamation. */
struct offline_job { unsigned mode; int result; };
static struct offline_job worker_job;
static void *worker_entry(void *arg)
{
  struct offline_job *job=arg;
  job->result=run_case(job->mode);
  return NULL;
}
static int run_in_worker(unsigned mode)
{
  pthread_attr_t attr; pthread_t worker;
  struct mallinfo before=mallinfo();
  int setup_rc=pthread_attr_init(&attr),attr_rc=0,join_rc=0,result=1;
  unsigned initialized=!setup_rc,created=0,joined=0,stage=1;
  if (!setup_rc) { stage=2; setup_rc=pthread_attr_setstacksize(&attr,256u*1024u); }
  if (!setup_rc) { stage=3; setup_rc=pthread_attr_setinheritsched(&attr,PTHREAD_INHERIT_SCHED); }
  if (!setup_rc) { stage=4; setup_rc=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE); }
  if (!setup_rc) {
    worker_job.mode=mode; worker_job.result=1; stage=5;
    setup_rc=pthread_create(&worker,&attr,worker_entry,&worker_job); created=!setup_rc;
  }
  if (initialized) {
    attr_rc=pthread_attr_destroy(&attr);
    if (attr_rc) atomic_store_explicit(&cleanup_retained,true,memory_order_release);
  }
  if (created) {
    stage=6; join_rc=pthread_join(worker,NULL); joined=!join_rc;
    if (join_rc) atomic_store_explicit(&cleanup_retained,true,memory_order_release);
    else result=worker_job.result;
  }
  struct mallinfo after=mallinfo();
  printf("APM_OFFLINE_WORKER stage=%u setup_rc=%d attr_cleanup_rc=%d join_rc=%d created=%u joined=%u requested_stack_bytes=262144 inherit_sched=1 affinity_set_calls=0 diagnostic_gate_retained=%u heap_before_used=%zu heap_before_free=%zu heap_after_used=%zu heap_after_free=%zu\n",
         stage,setup_rc,attr_rc,join_rc,created,joined,(unsigned)atomic_load_explicit(&cleanup_retained,memory_order_acquire),
         (size_t)before.uordblks,(size_t)before.fordblks,(size_t)after.uordblks,(size_t)after.fordblks);
  return setup_rc||attr_rc||join_rc?1:result;
}
static int parse_unsigned(const char *text, unsigned *value)
{
  if (!text || !*text) return -EINVAL;
  for (const char *p=text; *p; ++p) if (*p<'0' || *p>'9') return -EINVAL;
  errno=0; char *end=NULL; unsigned long n=strtoul(text,&end,10);
  if (errno || !end || *end || n>UINT32_MAX) return -ERANGE;
  *value=(unsigned)n; return 0;
}
static int dump(int argc, char **argv)
{
  unsigned offset=0,count=0;
  if (argc!=5 || !capture.sealed || strcmp(argv[2],capture.key) ||
      parse_unsigned(argv[3],&offset) || parse_unsigned(argv[4],&count) || !count ||
      count>DIAG_DUMP_MAX || offset>DIAG_SAMPLES || count>DIAG_SAMPLES-offset) return 1;
  printf("APM_OFFLINE_DUMP key=%s mode=%u start=%u count=%u total=%u output_crc=%08" PRIx32 " input_crc=%08" PRIx32 " sample_axis=api_call_order\n",capture.key,capture.mode,offset,count,DIAG_SAMPLES,capture.output_crc,capture.input_crc);
  for (unsigned i=0; i<count; i+=16u) {
    unsigned row=count-i<16u?count-i:16u;
    printf("APM_OFFLINE_PCM first=%u n=%u hex=",offset+i,row);
    for (unsigned j=0; j<row; ++j) { uint16_t v=(uint16_t)output_pcm[offset+i+j]; printf("%02x%02x",(unsigned)(v&255u),(unsigned)(v>>8)); }
    putchar('\n');
  }
  return 0;
}
int k7_apm21_offline_command(int argc, char **argv)
{
  if (atomic_flag_test_and_set_explicit(&busy,memory_order_acquire)) return 1;
  int rc=1;
  if (argc==3 && !strcmp(argv[1],"apm-offline-check")) {
    if (!strcmp(argv[2],"impulse")) rc=run_in_worker(1u);
    else if (!strcmp(argv[2],"prbs")) rc=run_in_worker(2u);
  } else if (argc==2 && !strcmp(argv[1],"apm-offline-status")) { print_status(); rc=0; }
  else if (argc==5 && !strcmp(argv[1],"apm-offline-dump")) rc=dump(argc,argv);
  if (!atomic_load_explicit(&cleanup_retained,memory_order_acquire))
    atomic_flag_clear_explicit(&busy,memory_order_release);
  return rc;
}
#endif
