#if defined(K7_APM21_CONSUMED_REFERENCE) && K7_APM21_CONSUMED_REFERENCE
#include "k7_apm21_reference_stage.h"
#include <errno.h>
#include <string.h>
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2,"PIO reference progress requires lock-free 64-bit atomics");
void k7_apm21_reference_init(struct k7_apm21_reference_stage *s){memset(s,0,sizeof(*s));atomic_init(&s->fault,0);atomic_init(&s->sealed,0);atomic_init(&s->published_next,0);atomic_init(&s->final_cut,0);}
int k7_apm21_reference_fault(struct k7_apm21_reference_stage *s,int rc){if(!s)return -EINVAL;int old=0;rc=rc<0?rc:-EPIPE;(void)atomic_compare_exchange_strong_explicit(&s->fault,&old,rc,memory_order_release,memory_order_relaxed);return old?old:rc;}
int k7_apm21_reference_begin(struct k7_apm21_reference_stage *s,uint64_t epoch,uint32_t gen,uint32_t input,uint32_t clock,uint64_t first,uint64_t now){
 if(!s||!epoch||!gen||!input||!clock||s->active)return -EINVAL;
 s->active=1;s->epoch=epoch;s->generation=gen;s->input=input;s->clock=clock;s->first=s->written=s->confirmed=first;s->last_us=now;
 atomic_store_explicit(&s->published_next,first,memory_order_release);return 0;
}
static int valid(struct k7_apm21_reference_stage *s,uint64_t now){
 if(!s||!s->active)return -EINVAL;int rc=atomic_load_explicit(&s->fault,memory_order_relaxed);if(rc)return rc;
 if(atomic_load_explicit(&s->sealed,memory_order_relaxed))return -EPIPE;
 if(now<s->last_us)return k7_apm21_reference_fault(s,-EPIPE);
 if(s->count&&now-s->time[s->head/160]>=K7_APM21_REFERENCE_LATE_US)return k7_apm21_reference_fault(s,-ETIMEDOUT);
 s->last_us=now;return 0;
}
int k7_apm21_reference_pair(struct k7_apm21_reference_stage *s,int16_t pcm,unsigned zero,uint64_t before,uint64_t after){
 int rc=valid(s,before);if(rc)return rc;
 if(after<before||zero>1||(zero&&pcm)||s->written==UINT64_MAX)return k7_apm21_reference_fault(s,-EPIPE);
 if(s->count==320)return k7_apm21_reference_fault(s,-ENOSPC);
 unsigned at=(s->head+s->count)%320;s->pcm[at]=pcm;if(zero)s->zero[at/8]|=(uint8_t)(1u<<(at%8));else s->zero[at/8]&=(uint8_t)~(1u<<(at%8));if(!(at%160))s->time[at/160]=before;s->after[at/160]=after;s->count++;s->written++;s->last_us=after;return 0;
}
int k7_apm21_reference_observe(struct k7_apm21_reference_stage *s,uint64_t epoch,uint32_t gen,uint32_t input,uint32_t clock,unsigned words,int observed,uint64_t now){
 int rc=valid(s,now);if(rc)return rc;
 if(observed||words>16||s->epoch!=epoch||s->generation!=gen||s->input!=input||s->clock!=clock)return k7_apm21_reference_fault(s,-ESTALE);
 unsigned remain=(words+1)/2;if(s->written-s->first<remain)return k7_apm21_reference_fault(s,-EPROTO);
 uint64_t confirmed=s->written-remain;if(confirmed<s->confirmed)return k7_apm21_reference_fault(s,-EPIPE);
 s->confirmed=confirmed;return 0;
}
int k7_apm21_reference_take(struct k7_apm21_reference_stage *s,struct k7_apm21_reference_block *b,uint64_t now){
 if(!b)return -EINVAL;int rc=valid(s,now);if(rc)return rc;
 uint64_t next=atomic_load_explicit(&s->published_next,memory_order_relaxed);
 if(next>s->confirmed)return k7_apm21_reference_fault(s,-EPIPE);
 if(s->count<160||s->confirmed-next<160)return 0;
 memset(b,0,sizeof(*b));b->first=next;b->before_us=s->time[s->head/160];b->after_us=s->after[s->head/160];
 for(unsigned n=0;n<160;n++){unsigned at=(s->head+n)%320;b->pcm[n]=s->pcm[at];if((s->zero[at/8]>>(at%8))&1u)b->zero_bits[n/8]|=1u<<(n%8);}
 s->head=(s->head+160)%320;s->count-=160;atomic_store_explicit(&s->published_next,next+160,memory_order_release);return 1;
}
int k7_apm21_reference_cut(struct k7_apm21_reference_stage *s,uint64_t epoch,uint32_t gen,uint32_t input,uint32_t clock,unsigned words,int observed,uint64_t now){
 int rc=k7_apm21_reference_observe(s,epoch,gen,input,clock,words,observed,now);if(rc)return rc;
 uint64_t cut=s->confirmed;atomic_store_explicit(&s->final_cut,cut,memory_order_release);
 if((words&1)||atomic_load_explicit(&s->published_next,memory_order_relaxed)>cut)return k7_apm21_reference_fault(s,-ENOTSUP);
 atomic_store_explicit(&s->sealed,1,memory_order_release);return 0;
}
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
int k7_apm21_reference_prefix(const struct k7_apm21_reference_stage *s,struct k7_apm21_reference_block *b,unsigned *samples){
 if(!s||!b||!samples||!s->active||!atomic_load_explicit(&s->sealed,memory_order_acquire))return -EINVAL;
 int rc=atomic_load_explicit(&s->fault,memory_order_relaxed);if(rc)return rc;
 uint64_t first=atomic_load_explicit(&s->published_next,memory_order_relaxed),cut=atomic_load_explicit(&s->final_cut,memory_order_relaxed);
 if(first>cut||cut>s->written||cut-first>=160||cut-first>s->count||s->written-first!=s->count)return -EPIPE;
 memset(b,0,sizeof(*b));b->first=first;*samples=(unsigned)(cut-first);
 if(*samples){b->before_us=s->time[s->head/160];b->after_us=s->after[s->head/160];}
 for(unsigned n=0;n<*samples;n++){unsigned at=(s->head+n)%320;b->pcm[n]=s->pcm[at];if((s->zero[at/8]>>(at%8))&1u)b->zero_bits[n/8]|=(uint8_t)(1u<<(n%8));}
 return 0;
}
#endif
#endif
