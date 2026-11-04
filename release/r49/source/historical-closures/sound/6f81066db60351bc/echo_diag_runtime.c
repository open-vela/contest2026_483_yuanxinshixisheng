#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
#include "k7_apm21_idle_events.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"PIO events require lock-free unsigned/int atomics");
void k7_apm21_idle_init(struct k7_apm21_idle_events *s){
 memset(s,0,sizeof(*s));atomic_init(&s->produced,0);atomic_init(&s->consumed,0);atomic_init(&s->fault,0);
}
void k7_apm21_idle_fault(struct k7_apm21_idle_events *s,int rc){
 if(!s)return;int expected=0;
 (void)atomic_compare_exchange_strong_explicit(&s->fault,&expected,rc<0?rc:-EPIPE,memory_order_release,memory_order_relaxed);
 s->active=s->pending=0; /* PIO only. No automatic recovery or new authority. */
}
static void emit(struct k7_apm21_idle_events *s,struct k7_apm21_idle_event *e){
 unsigned p=atomic_load_explicit(&s->produced,memory_order_relaxed);
 unsigned c=atomic_load_explicit(&s->consumed,memory_order_acquire);
 if(p-c>=K7_APM21_IDLE_SLOTS||s->serial==UINT64_MAX){k7_apm21_idle_fault(s,-ENOSPC);return;}
 e->serial=++s->serial;s->slots[p%K7_APM21_IDLE_SLOTS]=*e;
 atomic_store_explicit(&s->produced,p+1,memory_order_release);
}
void k7_apm21_idle_anchor(struct k7_apm21_idle_events *s,const struct k7_voice_anchor *a,uint32_t input){
 if(!s||!a||atomic_load_explicit(&s->fault,memory_order_relaxed))return;
 if(a->flags||!a->epoch||!input||a->after_us<a->before_us||a->before_us<s->last_us){k7_apm21_idle_fault(s,-EPIPE);return;}
 s->last_us=a->after_us;
 if(s->active&&(input!=s->input_epoch||a->epoch!=s->off.epoch||a->rx_samples!=s->next)){k7_apm21_idle_fault(s,-ESTALE);return;}
 if(a->event==K7_VOICE_ANCHOR_SESSION_STOP){k7_apm21_idle_fault(s,-ECANCELED);return;}
 if((a->event==K7_VOICE_ANCHOR_RX_FRAME||a->event==K7_VOICE_ANCHOR_TX_START)&&a->result){k7_apm21_idle_fault(s,-EPIPE);return;}
 if(a->event==K7_VOICE_ANCHOR_TX_START){
  if(s->active&&a->generation<=s->off.generation){k7_apm21_idle_fault(s,-ESTALE);return;}
  if(s->active){struct k7_apm21_idle_event e={.kind=K7_APM21_IDLE_END,.input_epoch=input,.off=s->off,.boundary=*a,.first=s->next,.next=s->next};emit(s,&e);}
  s->active=s->pending=0;return; /* Partial old spans are never padded. */
 }
 if(a->event==K7_VOICE_ANCHOR_AMP_OFF){
  if(a->result||!a->generation||s->active||a->rx_samples>UINT64_MAX-320){k7_apm21_idle_fault(s,-EPROTO);return;}
  s->active=1;s->ended=s->pending=s->quiet_seen=0;s->input_epoch=input;s->off=*a;s->next=a->rx_samples;
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
  s->disabled_seen=0;
#endif
  /* Even a frame-aligned boundary skips the next frame: one stereo half
   * may precede mute. RX FIFO age remains UNKNOWN, not fixed by this skip. */
  s->skip_until=(a->rx_samples/320+1)*320;
  memset(&s->partial,0,sizeof(s->partial));
  struct k7_apm21_idle_event e={.kind=K7_APM21_IDLE_OFF,.input_epoch=input,.off=*a,.boundary=*a,.first=a->rx_samples,.next=a->rx_samples};emit(s,&e);return;
 }
 if(!s->active)return;
 if(a->event==K7_VOICE_ANCHOR_TX_COMPLETE){
  if(a->generation!=s->off.generation||s->ended){k7_apm21_idle_fault(s,-ESTALE);return;}
  s->ended=1;struct k7_apm21_idle_event e={.kind=K7_APM21_IDLE_SOURCE_END,.input_epoch=input,.off=s->off,.boundary=*a,.first=s->next,.next=s->next};emit(s,&e);
 }
 if(a->event==K7_VOICE_ANCHOR_RX_FRAME&&s->pending){
  if(a->rx_samples!=s->partial.next){k7_apm21_idle_fault(s,-EPIPE);return;}
  s->partial.boundary=*a;emit(s,&s->partial);s->pending=0;memset(&s->partial,0,sizeof(s->partial));
 }
}
void k7_apm21_idle_microphone(struct k7_apm21_idle_events *s,int16_t pcm,uint64_t index,uint32_t input,uint32_t epoch){
 if(!s||!s->active||atomic_load_explicit(&s->fault,memory_order_relaxed))return;
 if(input!=s->input_epoch||epoch!=s->off.epoch||index!=s->next||index==UINT64_MAX||s->pending){k7_apm21_idle_fault(s,-EPIPE);return;}
 s->next=index+1;if(index<s->skip_until)return;
 if(!s->partial.samples){s->partial.kind=K7_APM21_IDLE_MIC;s->partial.input_epoch=input;s->partial.off=s->off;s->partial.first=index;}
 s->partial.pcm[s->partial.samples++]=pcm;s->partial.next=index+1;
 if(s->partial.samples==320)s->pending=1;
}
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
void k7_apm21_idle_disabled(struct k7_apm21_idle_events *s,const struct k7_voice_anchor *a,uint32_t input,int read_rc,uint32_t xfer){
 if(!s||!a||atomic_load_explicit(&s->fault,memory_order_relaxed))return;
 if(read_rc||!s->active||s->disabled_seen||s->quiet_seen||(xfer&15u)!=11u||input!=s->input_epoch||a->epoch!=s->off.epoch||a->generation!=s->off.generation||a->rx_samples!=s->next||a->flags||a->result||a->event!=K7_VOICE_ANCHOR_TX_STOP||a->after_us<a->before_us||a->before_us<s->last_us){k7_apm21_idle_fault(s,-ESTALE);return;}
 s->last_us=a->after_us;s->disabled_seen=1;
 struct k7_apm21_idle_event e={.kind=K7_APM21_IDLE_TX_DISABLED,.input_epoch=input,.off=s->off,.boundary=*a,.first=s->next,.next=s->next,.xfer=xfer};emit(s,&e);
}
#endif
void k7_apm21_idle_quiet(struct k7_apm21_idle_events *s,const struct k7_voice_anchor *a,uint32_t input,unsigned checks,uint32_t xfer){
 if(!s||!a||atomic_load_explicit(&s->fault,memory_order_relaxed))return;
 /* SAI_XFER low nibble: clocks=3, RXS=8, TXS=4 must be clear. */
 if(!s->active||s->quiet_seen||checks!=K7_APM21_IDLE_QUIET_CHECKS||(xfer&15u)!=11u||input!=s->input_epoch||a->epoch!=s->off.epoch||a->generation!=s->off.generation||a->rx_samples!=s->next||a->flags||a->result||a->after_us<a->before_us||a->before_us<s->last_us){k7_apm21_idle_fault(s,-ESTALE);return;}
 s->last_us=a->after_us;s->quiet_seen=1;
 struct k7_apm21_idle_event e={.kind=K7_APM21_IDLE_DIGITAL_QUIET,.input_epoch=input,.off=s->off,.boundary=*a,.first=s->next,.next=s->next,.quiet_checks=checks,.xfer=xfer};emit(s,&e);
}
int k7_apm21_idle_take(struct k7_apm21_idle_events *s,struct k7_apm21_idle_event *e){
 if(!s||!e)return -EINVAL;int rc=atomic_load_explicit(&s->fault,memory_order_acquire);if(rc)return rc;
 unsigned c=atomic_load_explicit(&s->consumed,memory_order_relaxed);
 unsigned p=atomic_load_explicit(&s->produced,memory_order_acquire);if(c==p)return 0;
 *e=s->slots[c%K7_APM21_IDLE_SLOTS];atomic_store_explicit(&s->consumed,c+1,memory_order_release);
 rc=atomic_load_explicit(&s->fault,memory_order_acquire);return rc?rc:1;
}
#endif
