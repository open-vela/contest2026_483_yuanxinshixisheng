#include "tail_reference_archive.h"
#include <errno.h>
#include <string.h>
static unsigned state(struct tail_reference_archive*s){return atomic_load_explicit(&s->state,memory_order_acquire);}
void tra_arm(struct tail_reference_archive*s,uint64_t owner,uint32_t floor){
 memset(s,0,sizeof(*s));atomic_init(&s->state,TRA_ARMED);s->owner=owner;s->floor=floor;
}
void tra_fault(struct tail_reference_archive*s,int reason){
 unsigned st=state(s);if(st!=TRA_ARMED&&st!=TRA_RECORDING)return;
 s->reason=reason?reason:-EPIPE;atomic_store_explicit(&s->state,TRA_FAILED,memory_order_release);
}
void tra_anchor(struct tail_reference_archive*s,const struct k7_voice_anchor*a){
 unsigned st=state(s);if(st!=TRA_ARMED&&st!=TRA_RECORDING)return;
 if(a->generation<=s->floor)return;
 if(a->flags||a->after_us<a->before_us){tra_fault(s,-EPIPE);return;}
 if(a->event!=K7_VOICE_ANCHOR_TX_CLEAR)return;
 /* Existing PIO emits TX_CLEAR only after observed TX_IDLE, before CLR. The
  * FIFO count is transport evidence; codec shift register/DAC remain unknown. */
 if(s->clear_seen||(a->result&&a->result!=-ECANCELED)||a->tx_fifo_words>16||a->discarded_words>a->tx_fifo_words||
 (s->generation&&a->generation!=s->generation)){tra_fault(s,-EPROTO);return;}
 s->clear=*a;s->clear_seen=1;
}
void tra_record(struct tail_reference_archive*s,const struct k7_fifo_ref_record*r){
 unsigned st=state(s);if(st!=TRA_ARMED&&st!=TRA_RECORDING)return;
 if(r->generation<=s->floor)return;
 if(st==TRA_ARMED){
  if(r->kind!=K7_FIFO_REF_BEGIN||!r->instance||!r->epoch||r->faults||r->samples||
    (s->clear_seen&&s->clear.generation!=r->generation)){tra_fault(s,-EPROTO);return;}
  s->instance=r->instance;s->epoch=r->epoch;s->generation=r->generation;
  s->first=s->next=s->kept_first=r->first_enqueue;s->source_next=0;
  atomic_store_explicit(&s->state,TRA_RECORDING,memory_order_release);return;
 }
 bool end=r->kind==K7_FIFO_REF_END;
 if(r->instance!=s->instance||r->generation!=s->generation||
 (!end&&r->epoch!=s->epoch)||(end&&(s->epoch==UINT32_MAX||r->epoch!=s->epoch+1))||
 (!end&&r->kind!=K7_FIFO_REF_PCM)||r->first_enqueue!=s->next||r->first_source!=s->source_next||
 r->samples>320||(!end&&r->samples!=320)||s->next>UINT64_MAX-r->samples||
 (!end&&r->faults)||(end&&(r->faults&~(K7_FIFO_REF_CLEAR|K7_FIFO_REF_COUNT)))){tra_fault(s,-EPROTO);return;}
 if(r->samples&&(r->after_us<r->before_us||r->before_us<s->last_after_us)){tra_fault(s,-EPIPE);return;}
 unsigned source=0;
 for(unsigned j=0;j<r->samples;j++){
  unsigned zero=(r->zero_bits[j/8]>>(j%8))&1u;if(zero&&r->pcm[j]){tra_fault(s,-EPROTO);return;}
  source+=!zero;unsigned at=(unsigned)((s->next+j)%TRA_SAMPLES);s->pcm[at]=r->pcm[j];s->zeros[at]=(uint8_t)zero;
 }
 if(source!=r->source_samples||s->source_next>UINT64_MAX-source){tra_fault(s,-EPROTO);return;}
 s->next+=r->samples;s->source_next+=source;
 s->kept=(unsigned)((s->next-s->first)>TRA_SAMPLES?TRA_SAMPLES:s->next-s->first);s->kept_first=s->next-s->kept;
 if(r->samples){s->last_before_us=r->before_us;s->last_after_us=r->after_us;}
 if(!end)return;
 if(!s->clear_seen||s->clear.generation!=s->generation||s->clear.tx_queued_samples!=s->source_next||
 s->clear.discarded_words!=r->discarded_words||s->clear.before_us<s->last_after_us||
 s->next-s->first<s->clear.tx_fifo_words/2u+(s->clear.tx_fifo_words%2u)){tra_fault(s,-EPROTO);return;}
 /* floor(consumed_words/2): a partial stereo pair is never called complete.
  * These are bytes proven out of FIFO, NOT samples proven out of the DAC. */
 unsigned remaining=s->clear.tx_fifo_words;
 s->digital_complete_pair_next=s->next-remaining/2u-(remaining%2u);
 s->uncertain_partial_pair=remaining%2u;s->reason=0;
 atomic_store_explicit(&s->state,TRA_DONE,memory_order_release);
}
