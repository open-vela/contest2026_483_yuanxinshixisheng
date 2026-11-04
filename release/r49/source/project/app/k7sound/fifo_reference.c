/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fifo_reference.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
struct k7_fifo_reference {
  struct k7_fifo_ref_record ring[K7_FIFO_REF_SLOTS], pending;
  uint64_t instance; /* immutable after create */
  atomic_uint produced, consumed, epoch, generation, faults;
  atomic_bool eligible, exhausted;
  uint32_t consumer_epoch; /* consumer only */
  unsigned consumer_mode; /* 0 unset, 1 qualified take, 2 diagnostic archive */
  uint64_t next_enqueue, next_source, last_us; /* producer only */
  uint32_t last_generation;
  bool clock_seen, terminal_sent;
};
static atomic_uint_fast64_t instances = ATOMIC_VAR_INIT(0);
size_t k7_fifo_reference_storage_bytes(void) { return sizeof(struct k7_fifo_reference); }
int k7_fifo_reference_create(struct k7_fifo_reference **out)
{
  if(!out)return -EINVAL;
  *out=NULL;
  struct k7_fifo_reference *q=calloc(1,sizeof(*q)); if(!q)return -ENOMEM;
  uint_fast64_t id=atomic_load_explicit(&instances,memory_order_relaxed);
  for(;;){
    if(id==UINT64_MAX){free(q);return -EOVERFLOW;}
    if(atomic_compare_exchange_weak_explicit(&instances,&id,id+1,memory_order_relaxed,memory_order_relaxed))break;
  }
  /* Object is not published until initialized; instance is immutable. */
  q->instance=id+1;
  atomic_init(&q->produced,0);atomic_init(&q->consumed,0);atomic_init(&q->epoch,0);
  atomic_init(&q->generation,0);atomic_init(&q->faults,0);
  atomic_init(&q->eligible,false);atomic_init(&q->exhausted,false);
  *out=q;return 0;
}
void k7_fifo_reference_destroy(struct k7_fifo_reference *q)
{ if(q){volatile unsigned char *p=(volatile unsigned char *)q;for(size_t i=0;i<sizeof(*q);++i)p[i]=0;free(q);} }
static bool advance_epoch(struct k7_fifo_reference *q)
{
  unsigned e=atomic_load_explicit(&q->epoch,memory_order_relaxed);
  if(e==UINT32_MAX){atomic_store_explicit(&q->exhausted,true,memory_order_release);return false;}
  atomic_store_explicit(&q->epoch,e+1,memory_order_release);return true;
}
void k7_fifo_reference_revoke(struct k7_fifo_reference *q,unsigned faults)
{
  if(!q)return;
  atomic_fetch_or_explicit(&q->faults,faults,memory_order_relaxed);
  if(atomic_exchange_explicit(&q->eligible,false,memory_order_acq_rel)) (void)advance_epoch(q);
  if(faults&K7_FIFO_REF_OVERFLOW)atomic_store_explicit(&q->exhausted,true,memory_order_release);
}
static bool publish(struct k7_fifo_reference *q,struct k7_fifo_ref_record *r)
{
  unsigned p=atomic_load_explicit(&q->produced,memory_order_relaxed);
  unsigned c=atomic_load_explicit(&q->consumed,memory_order_acquire);
  if(p==UINT32_MAX || p<c){k7_fifo_reference_revoke(q,K7_FIFO_REF_OVERFLOW);return false;}
  if(p-c>=K7_FIFO_REF_SLOTS){k7_fifo_reference_revoke(q,K7_FIFO_REF_QUEUE);return false;}
  r->instance=q->instance;r->epoch=atomic_load_explicit(&q->epoch,memory_order_relaxed);
  r->generation=atomic_load_explicit(&q->generation,memory_order_relaxed);
  r->faults=atomic_load_explicit(&q->faults,memory_order_relaxed);
  q->ring[p%K7_FIFO_REF_SLOTS]=*r;
  atomic_store_explicit(&q->produced,p+1,memory_order_release);return true;
}
void k7_fifo_reference_begin(struct k7_fifo_reference *q,uint32_t generation)
{
  if(!q || atomic_load_explicit(&q->exhausted,memory_order_relaxed))return;
  k7_fifo_reference_revoke(q,K7_FIFO_REF_STOP);
  if(!generation || generation<=q->last_generation){k7_fifo_reference_revoke(q,K7_FIFO_REF_COUNT);return;}
  if(!advance_epoch(q))return;
  q->last_generation=generation;q->next_source=0;q->terminal_sent=false;
  memset(&q->pending,0,sizeof(q->pending));
  atomic_store_explicit(&q->generation,generation,memory_order_relaxed);
  atomic_store_explicit(&q->faults,0,memory_order_relaxed);
  struct k7_fifo_ref_record begin={.kind=K7_FIFO_REF_BEGIN,.first_enqueue=q->next_enqueue};
  if(publish(q,&begin))atomic_store_explicit(&q->eligible,true,memory_order_release);
}
void k7_fifo_reference_pair(struct k7_fifo_reference *q,uint32_t generation,
  int16_t sample,bool zero,uint64_t index,uint64_t before,uint64_t after)
{
  if(!q || atomic_load_explicit(&q->exhausted,memory_order_relaxed))return;
  if(q->next_enqueue==UINT64_MAX){k7_fifo_reference_revoke(q,K7_FIFO_REF_OVERFLOW);return;}
  uint64_t enqueue=q->next_enqueue++;
  if(!atomic_load_explicit(&q->eligible,memory_order_relaxed))return;
  if(!zero&&q->next_source==UINT64_MAX){k7_fifo_reference_revoke(q,K7_FIFO_REF_OVERFLOW);return;}
  if(generation!=q->last_generation || (!zero&&index!=q->next_source) || (zero&&sample!=0)){
    k7_fifo_reference_revoke(q,K7_FIFO_REF_COUNT);return;
  }
  if(after==UINT64_MAX||before==UINT64_MAX){k7_fifo_reference_revoke(q,K7_FIFO_REF_CLOCK|K7_FIFO_REF_OVERFLOW);return;}
  if(after<before || (q->clock_seen&&before<q->last_us)){
    k7_fifo_reference_revoke(q,K7_FIFO_REF_CLOCK);return;
  }
  q->clock_seen=true;q->last_us=after;
  struct k7_fifo_ref_record *r=&q->pending;
  if(!r->samples){r->kind=K7_FIFO_REF_PCM;r->first_enqueue=enqueue;r->first_source=q->next_source;r->before_us=before;}
  unsigned n=r->samples;
  r->pcm[n]=sample;r->after_us=after;
  if(zero)r->zero_bits[n/8]|=(uint8_t)(1u<<(n%8));
  else {q->next_source++;r->source_samples++;}
  r->samples++;
  if(r->samples==K7_FIFO_REF_SAMPLES){(void)publish(q,r);memset(r,0,sizeof(*r));}
}
void k7_fifo_reference_end(struct k7_fifo_reference *q,int result,unsigned discarded,bool empty)
{
  if(!q || q->terminal_sent || !q->last_generation)return;
  k7_fifo_reference_revoke(q,K7_FIFO_REF_CLEAR|(discarded?K7_FIFO_REF_COUNT:0));
  q->terminal_sent=true;
  if(atomic_load_explicit(&q->exhausted,memory_order_relaxed))return;
  struct k7_fifo_ref_record end=q->pending;
  if(!end.samples){end.first_enqueue=q->next_enqueue;end.first_source=q->next_source;}
  end.kind=K7_FIFO_REF_END;end.result=result;end.discarded_words=discarded;end.fifo_empty=empty;
  (void)publish(q,&end);memset(&q->pending,0,sizeof(q->pending));
}
int k7_fifo_reference_status(const struct k7_fifo_reference *q,struct k7_fifo_ref_status *out)
{
  if(!q||!out)return -EINVAL;
  unsigned e=atomic_load_explicit(&q->epoch,memory_order_acquire);
  bool eligible=atomic_load_explicit(&q->eligible,memory_order_acquire);
  *out=(struct k7_fifo_ref_status){.instance=q->instance,.epoch=e,
    .generation=atomic_load_explicit(&q->generation,memory_order_relaxed),
    .faults=atomic_load_explicit(&q->faults,memory_order_relaxed),
    .eligible=eligible,
    .exhausted=atomic_load_explicit(&q->exhausted,memory_order_acquire)};
  return e==atomic_load_explicit(&q->epoch,memory_order_acquire)&&
    eligible==atomic_load_explicit(&q->eligible,memory_order_acquire)?0:-EAGAIN;
}
bool k7_fifo_reference_matches(const struct k7_fifo_reference *q,uint64_t instance,unsigned epoch,unsigned generation)
{
  struct k7_fifo_ref_status s;
  return q&&!k7_fifo_reference_status(q,&s)&&s.eligible&&!s.exhausted&&
    instance==s.instance&&epoch==s.epoch&&generation==s.generation;
}
int k7_fifo_reference_take(struct k7_fifo_reference *q,struct k7_fifo_ref_record *out)
{
  if(!q||!out)return -EINVAL;
  if(q->consumer_mode==2)return -EPERM;
  q->consumer_mode=1;
  if(atomic_load_explicit(&q->exhausted,memory_order_acquire))return -EOVERFLOW;
  unsigned e=atomic_load_explicit(&q->epoch,memory_order_acquire);
  unsigned c=atomic_load_explicit(&q->consumed,memory_order_relaxed);
  unsigned p=atomic_load_explicit(&q->produced,memory_order_acquire);
  if(q->consumer_epoch!=e){
    while(c!=p&&q->ring[c%K7_FIFO_REF_SLOTS].epoch!=e)c++;
    atomic_store_explicit(&q->consumed,c,memory_order_release);q->consumer_epoch=e;return -EPIPE;
  }
  if(c==p)return 0;
  *out=q->ring[c%K7_FIFO_REF_SLOTS];
  atomic_store_explicit(&q->consumed,c+1,memory_order_release);
  return out->epoch==e&&e==atomic_load_explicit(&q->epoch,memory_order_acquire)?1:-EPIPE;
}

int k7_fifo_reference_take_archive(struct k7_fifo_reference *q,struct k7_fifo_ref_record *out)
{
  if(!q||!out)return -EINVAL;
  if(q->consumer_mode==1)return -EPERM;
  q->consumer_mode=2;
  if(atomic_load_explicit(&q->exhausted,memory_order_acquire))return -EOVERFLOW;
  unsigned c=atomic_load_explicit(&q->consumed,memory_order_relaxed);
  unsigned p=atomic_load_explicit(&q->produced,memory_order_acquire);
  if(p<c||p-c>K7_FIFO_REF_SLOTS)return -EPROTO;
  if(c==p)return 0;
  *out=q->ring[c%K7_FIFO_REF_SLOTS];
  atomic_store_explicit(&q->consumed,c+1,memory_order_release);
  return 1; /* historical bytes only, including revoked identities */
}
