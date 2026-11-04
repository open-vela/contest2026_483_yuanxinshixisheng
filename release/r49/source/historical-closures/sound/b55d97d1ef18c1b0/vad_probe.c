#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include "vad_probe.h"
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
#include "raw_vad_owner.h"
#endif
#include "vad_probe_asset.h"
#include "k7_vad_ort.h"
#include "vv_native_service.h"
#include "k7boot_aliyun_sequence.h"
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WINDOWS 128u
#define TOTAL_SAMPLES (64u+WINDOWS*512u)
#define RESERVE_BYTES (16u*1024u*1024u)
static atomic_flag active=ATOMIC_FLAG_INIT;
static const int16_t zeros[320];
struct heap_peak {size_t sampled_used_max,sampled_free_min,global_maxused;};
static uint64_t now_us(void)
{
  struct timespec t;
  if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||
     (uint64_t)t.tv_sec>(UINT64_MAX-999999u)/1000000u)return UINT64_MAX;
  return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
static struct mallinfo snapshot(struct heap_peak *p,const char *stage)
{
  struct mallinfo m=mallinfo();
  if((size_t)m.uordblks>p->sampled_used_max)p->sampled_used_max=m.uordblks;
  if((size_t)m.fordblks<p->sampled_free_min)p->sampled_free_min=m.fordblks;
  if((size_t)m.usmblks>p->global_maxused)p->global_maxused=m.usmblks;
  if(stage)printf("VAD_HEAP stage=%s total=%zu used=%zu free=%zu maxfree=%zu global_maxused=%zu\n",
      stage,(size_t)m.arena,(size_t)m.uordblks,(size_t)m.fordblks,(size_t)m.mxordblk,(size_t)m.usmblks);
  return m;
}
static int idle_service(void)
{
  struct vv_native_service_status s;
  int error=vv_native_service_status(&s);
  return error?error:s.running?-EBUSY:0;
}
static int pass(struct k7_vad *vad,unsigned number,uint32_t bits[WINDOWS],struct heap_peak *peak)
{
  struct k7_vad_identity id={1,1,0};
  uint64_t total=0,maximum=0,all_total=0,all_max=0;unsigned windows=0;
  int error=k7_vad_reset(vad,id,0,0);
  if(error)return error;
  snapshot(peak,number==1?"after_initial_reset":"after_explicit_reset");
  for(unsigned first=0,frame=0;first<TOTAL_SAMPLES;first+=320u,frame++){
    struct k7_vad_frame input={id,frame,first,zeros,320};
    struct k7_vad_observation output;
    uint64_t begin=now_us();
    if(begin==UINT64_MAX)return -EIO;
    int got=k7_vad_feed(vad,&input,&output);
    uint64_t end=now_us();
    if(end==UINT64_MAX||end<begin)return -EIO;
    uint64_t elapsed=end-begin;all_total+=elapsed;if(elapsed>all_max)all_max=elapsed;
    if(got<0)return got;
    if(got){
      uint32_t probability;
      if(windows>=WINDOWS||output.window_id!=windows||output.completing_frame_id!=frame||
         output.input_first_sample!=(uint64_t)windows*512u||output.input_end_sample!=(uint64_t)windows*512u+576u||
         output.advance_first_sample!=(uint64_t)windows*512u+64u||output.advance_end_sample!=output.input_end_sample)
        return -EILSEQ;
      memcpy(&probability,&output.probability,sizeof(probability));
      if(number==1)bits[windows]=probability;
      else if(bits[windows]!=probability)return -EILSEQ;
      windows++;total+=elapsed;if(elapsed>maximum)maximum=elapsed;
    }
    struct mallinfo m=snapshot(peak,NULL);
    /* A sampled post-call floor, not a hard ORT allocator cap. */
    if((size_t)m.fordblks<RESERVE_BYTES)return -ENOMEM;
    error=idle_service();if(error)return error;
  }
  printf("VAD_PASS pass=%u frames=205 samples=%u windows=%u input_last_end=65600 total_window_feed_us=%" PRIu64 " max_window_feed_us=%" PRIu64 " total_all_feed_us=%" PRIu64 " max_all_feed_us=%" PRIu64 " reset_replay=%u\n",
      number,TOTAL_SAMPLES,windows,total,maximum,all_total,all_max,number==2);
  return windows==WINDOWS?0:-ENODATA;
}
int k7_vad_probe_command(int argc,char **argv)
{
  struct k7_vad_probe_lease lease={0};struct k7_vad_ort *backend=NULL;struct k7_vad *vad=NULL;
  struct heap_peak peak={0,SIZE_MAX,0};uint32_t probability_bits[WINDOWS];
  int error,release_error=0;uint64_t begin,end;bool admitted=false;unsigned completed=0;
  if(argc!=2||strcmp(argv[1],"vad-probe"))return 1;
  if(atomic_flag_test_and_set_explicit(&active,memory_order_acquire))return 1;
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
  bool resource=false;
  error=k7_raw_vad_owner_probe_begin();resource=!error;
  if(!error)error=idle_service();
#else
  error=idle_service();
#endif
  if(!error){error=k7boot_aliyun_vad_probe_acquire();admitted=!error;}
  if(error)goto done;
  struct mallinfo before=snapshot(&peak,"before");
  if((size_t)before.fordblks<RESERVE_BYTES){error=-ENOMEM;goto done;}
  error=k7_vad_probe_asset_acquire(&lease);if(error)goto done;
  snapshot(&peak,"asset_verified");
  struct k7_vad_model model={lease.data,lease.bytes,k7_vad_probe_hash,NULL};
  begin=now_us();if(begin==UINT64_MAX){error=-EIO;goto done;}
  error=k7_vad_ort_create(OrtGetApiBase(),&model,&backend);
  end=now_us();if(!error&&(end==UINT64_MAX||end<begin))error=-EIO;
  printf("VAD_CREATE result=%d elapsed_us=%" PRIu64 " model_bytes=%zu synthetic_zero_only=1 actions=0\n",
      error,end!=UINT64_MAX&&end>=begin?end-begin:0,lease.bytes);
  struct mallinfo created=snapshot(&peak,"after_model_create");
  if(!error&&(size_t)created.fordblks<RESERVE_BYTES)error=-ENOMEM;
  if(!error)error=idle_service();
  if(!error)error=k7_vad_create(k7_vad_ort_ops(),backend,&vad);
  if(!error){error=pass(vad,1,probability_bits,&peak);if(!error)completed=1;}
  snapshot(&peak,"after_pass1");
  if(!error){error=pass(vad,2,probability_bits,&peak);if(!error)completed=2;}
  snapshot(&peak,"after_reset_replay");
done:
  k7_vad_destroy(vad);k7_vad_ort_destroy(backend);
  if(lease.id)release_error=k7_vad_probe_asset_release(&lease);
  /* Sample retired resources BEFORE releasing boot prepare admission. */
  if(admitted){snapshot(&peak,"after_destroy");printf("VAD_PEAK sampled_used_max=%zu sampled_free_min=%zu global_maxused_observed=%zu transient_allocator_peak_verified=0\n",peak.sampled_used_max,peak.sampled_free_min,peak.global_maxused);}
  if(admitted){int gate_error=k7boot_aliyun_vad_probe_release();if(!release_error)release_error=gate_error;}
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
  /* Unknown resource/gate cleanup retains PROBE admission and blocks voice. */
  if(resource&&!release_error){int e=k7_raw_vad_owner_probe_end();if(!release_error)release_error=e;}
#endif
  if(!error&&release_error)error=release_error;
  printf("VAD_PROBE result=%d release=%d synthetic_zero_only=1 windows_per_pass=128 requested_passes=2 completed_passes=%u audio=0 cloud=0 actions=0\n",error,release_error,completed);
  atomic_flag_clear_explicit(&active,memory_order_release);return error?1:0;
}
