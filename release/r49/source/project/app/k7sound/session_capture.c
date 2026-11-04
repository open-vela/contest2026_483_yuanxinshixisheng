/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "session_capture.h"
#include "voice_session.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SOURCE_MAX 480000u
#define MIC_MAX (92u * 16000u)
#define EVENT_MAX 16u
#define DUMP_MAX 4096u
enum capture_state { EMPTY, ARMED, ACTIVE, FROZEN };
struct gap_event { uint64_t rx_begin, rx_end; unsigned cursor, zeros; bool closed; };
struct capture {
  struct k7_voice_session *session;
  uint32_t generation;
  uint64_t owner_epoch;
  int16_t *source, *mic;
  unsigned source_count, mic_count, fifo_count, zeros, events;
  uint32_t source_crc, mic_crc, fifo_crc;
  uint64_t rx_first, rx_next;
  bool rx_started, source_truncated, mic_truncated, event_truncated, gap;
  int source_error, owner_error, fetch_result, stream_result;
  struct gap_event event[EVENT_MAX];
};
static struct capture trace;
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static atomic_int state;
static atomic_bool source_done, owner_done;
static atomic_uint callbacks;
static atomic_uintptr_t ready_owner;
static atomic_uint_fast64_t owner_epoch;
static atomic_bool tx_busy;
void k7_session_capture_owner_ready(struct k7_voice_session *s,bool ready)
{
  if(ready){
    if(atomic_load(&owner_epoch)==UINT64_MAX){atomic_store(&ready_owner,0);return;}
    atomic_store(&tx_busy,false);atomic_fetch_add(&owner_epoch,1);
    atomic_store_explicit(&ready_owner,(uintptr_t)s,memory_order_release);}
  else if(atomic_load(&ready_owner)==(uintptr_t)s)
    atomic_store_explicit(&ready_owner,0,memory_order_release);
}
void k7_session_capture_tx_busy(struct k7_voice_session *s,bool busy)
{if(atomic_load_explicit(&ready_owner,memory_order_acquire)==(uintptr_t)s)atomic_store(&tx_busy,busy);}
/* Four-bit lookup keeps the owner cost bounded without a large table. */
static uint32_t sample_crc(uint32_t crc, int16_t value)
{
  static const uint32_t table[16] = {
    0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
    0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
  uint16_t v = (uint16_t)value;
  for (unsigned b=0;b<2;b++) {
    crc ^= (v >> (8*b)) & 255u;
    crc = (crc >> 4) ^ table[crc & 15u];
    crc = (crc >> 4) ^ table[crc & 15u];
  }
  return crc;
}
static bool selected(struct k7_voice_session *s)
{
  int current=atomic_load_explicit(&state,memory_order_acquire);
  if(current!=ACTIVE)return false;
  atomic_fetch_add_explicit(&callbacks,1,memory_order_seq_cst);
  current=atomic_load_explicit(&state,memory_order_seq_cst);
  if(current==ACTIVE&&trace.session==s)return true;
  atomic_fetch_sub_explicit(&callbacks,1,memory_order_seq_cst);return false;
}
static void leave(void){atomic_fetch_sub_explicit(&callbacks,1,memory_order_seq_cst);}
static void finish_gap(void)
{
  if (trace.gap && trace.events) {
    struct gap_event *e=&trace.event[trace.events-1];
    e->rx_end=trace.rx_next;e->closed=true;
  }
  trace.gap=false;
}
/* Called only by the live owner after acquiring source_done. */
static void freeze(void)
{
  finish_gap();
  atomic_store_explicit(&state,FROZEN,memory_order_release);
}
int k7_session_capture_begin(struct k7_voice_session *s,uint32_t generation)
{
  if(!s||!generation)return -EINVAL;
  if(pthread_mutex_trylock(&control))return -EBUSY;
  int rc=0;
  if(atomic_load_explicit(&state,memory_order_acquire)==ARMED && trace.session==s &&
     trace.owner_epoch==atomic_load(&owner_epoch) && atomic_load(&ready_owner)==(uintptr_t)s) {
    trace.generation=generation;
    /* arm initializes completion flags before publishing ARMED. owner_exit is
     * post-pump and holds this same control lock, closing the admission race. */
    atomic_store_explicit(&state,ACTIVE,memory_order_release);rc=1;
  }
  pthread_mutex_unlock(&control);return rc;
}
void k7_session_capture_source(struct k7_voice_session *s,uint32_t generation,
                               const int16_t *pcm,unsigned count,uint32_t offset)
{
  if(!selected(s))return;
  if(generation!=trace.generation||atomic_load(&source_done)){leave();return;}
  if(!pcm||offset!=trace.source_count){trace.source_error=-EILSEQ;leave();return;}
  if(count>SOURCE_MAX-trace.source_count){trace.source_truncated=true;leave();return;}
  memcpy(trace.source+trace.source_count,pcm,count*sizeof(*pcm));
  for(unsigned i=0;i<count;i++)trace.source_crc=sample_crc(trace.source_crc,pcm[i]);
  trace.source_count+=count;leave();
}
void k7_session_capture_end(struct k7_voice_session *s,uint32_t generation,
                            int fetch_result,int stream_result)
{
  if(!selected(s))return;
  if(generation!=trace.generation){leave();return;}
  trace.fetch_result=fetch_result;trace.stream_result=stream_result;
  /* A single total order prevents the two completion participants from both
   * observing false (release/acquire alone permits that on ARM). */
  atomic_store_explicit(&source_done,true,memory_order_seq_cst);
  if(atomic_load_explicit(&owner_done,memory_order_seq_cst))
    atomic_store_explicit(&state,FROZEN,memory_order_release);
  leave();
}
void k7_session_capture_microphone(struct k7_voice_session *s,int16_t value,uint64_t index)
{
  if(!selected(s))return;
  if(atomic_load_explicit(&source_done,memory_order_acquire)){freeze();leave();return;}
  if(!trace.rx_started){trace.rx_started=true;trace.rx_first=trace.rx_next=index;}
  if(index!=trace.rx_next){trace.owner_error=-EILSEQ;trace.rx_next=index;}
  trace.rx_next=index+1;
  if(trace.mic_count==MIC_MAX){trace.mic_truncated=true;leave();return;}
  trace.mic[trace.mic_count++]=value;trace.mic_crc=sample_crc(trace.mic_crc,value);
  leave();
}
void k7_session_capture_fifo_source(struct k7_voice_session *s,int16_t value,uint64_t index)
{
  if(!selected(s))return;
  if(atomic_load(&owner_done)||k7_voice_session_tx_generation(s)!=trace.generation){leave();return;}
  finish_gap();
  if(index!=trace.fifo_count)trace.owner_error=-EILSEQ;
  trace.fifo_count++;trace.fifo_crc=sample_crc(trace.fifo_crc,value);leave();
}
void k7_session_capture_fifo_zero(struct k7_voice_session *s)
{
  if(!selected(s))return;
  if(atomic_load(&owner_done)||k7_voice_session_tx_generation(s)!=trace.generation){leave();return;}
  if(!trace.gap){
    if(trace.events==EVENT_MAX){trace.event_truncated=true;}
    else {struct gap_event *e=&trace.event[trace.events++];
      e->rx_begin=trace.rx_next;e->cursor=trace.fifo_count;trace.gap=true;}
  }
  if(trace.gap)trace.event[trace.events-1].zeros++;
  trace.zeros++;leave();
}
void k7_session_capture_owner_exit(struct k7_voice_session *s,int result)
{
  /* Called only after the physical run returned and ready(false) published.
   * Serializing with begin prevents exit between its ready check and ACTIVE.
   * No code holding control waits for the audio owner or takes its mutex. */
  pthread_mutex_lock(&control);
  if(!selected(s)){pthread_mutex_unlock(&control);return;}
  if(result&&!trace.owner_error)trace.owner_error=result;
  finish_gap();
  /* Last owner access to trace precedes release; the producer can freeze after
   * this point even if the physical owner is already gone. */
  atomic_store_explicit(&owner_done,true,memory_order_seq_cst);
  if(atomic_load_explicit(&source_done,memory_order_seq_cst))
    atomic_store_explicit(&state,FROZEN,memory_order_release);
  leave();pthread_mutex_unlock(&control);
}
static void wipe(void *p,size_t n)
{ volatile unsigned char *b=p;while(n--)*b++=0; }
static int number(const char *s,unsigned *out)
{
  if(!s||!*s||*s=='+'||*s=='-')return -EINVAL;
  unsigned n=0;for(;*s;s++){if(*s<'0'||*s>'9'||n>(MIC_MAX-(unsigned)(*s-'0'))/10)return -EINVAL;n=n*10+(unsigned)(*s-'0');}
  *out=n;return 0;
}
static void summary(void)
{
  int current=atomic_load_explicit(&state,memory_order_acquire);
  printf("CAPTURE_STATE state=%d frozen=%u\n",current,current==FROZEN);
  if(current!=FROZEN)return; /* Active non-atomic payload belongs to producers. */
  bool complete=!trace.source_error&&!trace.owner_error&&!trace.fetch_result&&!trace.stream_result&&
    !trace.source_truncated&&!trace.mic_truncated&&!trace.event_truncated&&trace.source_count&&
    trace.mic_count&&trace.fifo_count==trace.source_count&&trace.fifo_crc==trace.source_crc;
  printf("CAPTURE_RESULT generation=%u owner_epoch=%" PRIu64 " complete=%u source_samples=%u mic_samples=%u fifo_samples=%u zeros=%u events=%u source_crc=%08" PRIx32 " mic_crc=%08" PRIx32 " fifo_crc=%08" PRIx32 "\n",
    trace.generation,trace.owner_epoch,complete,trace.source_count,trace.mic_count,trace.fifo_count,trace.zeros,trace.events,
    trace.source_crc^UINT32_MAX,trace.mic_crc^UINT32_MAX,trace.fifo_crc^UINT32_MAX);
  printf("CAPTURE_END fetch_rc=%d stream_rc=%d source_error=%d owner_error=%d source_truncated=%u mic_truncated=%u event_truncated=%u rx_first=%" PRIu64 " rx_next=%" PRIu64 " rate=16000\n",
    trace.fetch_result,trace.stream_result,trace.source_error,trace.owner_error,trace.source_truncated,
    trace.mic_truncated,trace.event_truncated,trace.rx_first,trace.rx_next);
  for(unsigned i=0;i<trace.events;i++) {
    const struct gap_event *e=&trace.event[i];
    printf("CAPTURE_GAP index=%u source_cursor=%u zeros=%u rx_begin=%" PRIu64 " rx_end=%" PRIu64 " closed=%u\n",
      i,e->cursor,e->zeros,e->rx_begin,e->rx_end,e->closed);
  }
}
int k7_session_capture_command(int argc,char **argv)
{
  if(argc<2||!argv)return 1;
  pthread_mutex_lock(&control);
  int current=atomic_load_explicit(&state,memory_order_acquire),rc=-EINVAL;
  if(argc==2&&!strcmp(argv[1],"session-capture-status")){summary();rc=0;}
  else if(argc==2&&!strcmp(argv[1],"session-capture-arm")) {
    if(current!=EMPTY)rc=-EBUSY;
    else {
      uint64_t epoch=atomic_load(&owner_epoch);
      struct k7_voice_session *session=(struct k7_voice_session *)atomic_load_explicit(&ready_owner,memory_order_acquire);
      if(!session||atomic_load(&tx_busy)||epoch!=atomic_load(&owner_epoch)) {
        rc=-EBUSY;goto command_done;
      }
      int16_t *source=malloc(SOURCE_MAX*sizeof(*source)),*mic=malloc(MIC_MAX*sizeof(*mic));
      if(!source||!mic){free(source);free(mic);rc=-ENOMEM;}
      else if(atomic_load(&tx_busy)||epoch!=atomic_load(&owner_epoch)||atomic_load(&ready_owner)!=(uintptr_t)session){free(source);free(mic);rc=-EBUSY;}
      else {memset(&trace,0,sizeof(trace));trace.source=source;trace.mic=mic;trace.session=session;trace.owner_epoch=epoch;
        trace.source_crc=trace.mic_crc=trace.fifo_crc=UINT32_MAX;
        atomic_store(&source_done,false);atomic_store(&owner_done,false);
        atomic_store_explicit(&state,ARMED,memory_order_release);rc=0;}
    }
  } else if(argc==2&&!strcmp(argv[1],"session-capture-clear")) {
    if(current==ACTIVE)rc=-EBUSY;
    else {
      /* Retire admission before inspecting refs. Together with the callback's
       * SC claim/recheck, either this sees its ref or it sees EMPTY and never
       * touches trace. Never free first and publish EMPTY afterwards. */
      atomic_store_explicit(&state,EMPTY,memory_order_seq_cst);
      if(atomic_load_explicit(&callbacks,memory_order_seq_cst)){
        atomic_store_explicit(&state,current,memory_order_seq_cst);rc=-EBUSY;goto command_done;
      }
      if(trace.source){wipe(trace.source,SOURCE_MAX*sizeof(*trace.source));free(trace.source);}
      if(trace.mic){wipe(trace.mic,MIC_MAX*sizeof(*trace.mic));free(trace.mic);}
      memset(&trace,0,sizeof(trace));atomic_store_explicit(&state,EMPTY,memory_order_release);rc=0;}
  } else if(argc==5&&!strcmp(argv[1],"session-capture-dump")&&current==FROZEN) {
    bool source=!strcmp(argv[2],"source"),mic=!strcmp(argv[2],"mic");unsigned offset=0,count=0;
    unsigned available=source?trace.source_count:trace.mic_count;
    if((source||mic)&&!number(argv[3],&offset)&&!number(argv[4],&count)&&count&&count<=DUMP_MAX&&offset<=available&&count<=available-offset) {
      const int16_t *pcm=source?trace.source:trace.mic;
      for(unsigned at=0;at<count;at+=64){unsigned n=count-at;if(n>64)n=64;
        printf("CAPTURE_PCM generation=%u owner_epoch=%" PRIu64 " kind=%s offset=%u samples=%u data=",trace.generation,trace.owner_epoch,source?"source":"mic",offset+at,n);
        for(unsigned i=0;i<n;i++){uint16_t v=(uint16_t)pcm[offset+at+i];printf("%02x%02x",v&255,v>>8);}putchar('\n');}
      rc=0;
    }
  }
command_done:
  printf("CAPTURE_COMMAND result=%d\n",rc);fflush(stdout);pthread_mutex_unlock(&control);return rc?1:0;
}
