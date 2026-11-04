#include "cancel_continuation.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct cancel_continuation {
 struct tail_reference_archive archive;
 uint64_t owner,mic_base,mic_next,mic_read,ref_read,ref_next;
 uint32_t input,clock;
 int error;
 bool begun,ended,muted;
 struct k7_voice_anchor mute;
 int16_t mic[CC_CAPACITY],ref[CC_CAPACITY];
};
static int fail(struct cancel_continuation*s,int rc){s->error=rc;return rc;}
int cc_create(uint64_t owner,uint32_t input,uint32_t clock,uint64_t first,uint32_t floor,struct cancel_continuation**out){
 if(!out||!owner||!input||!clock)return -EINVAL;
 *out=calloc(1,sizeof(**out));if(!*out)return -ENOMEM;
 struct cancel_continuation*s=*out;s->owner=owner;s->input=input;s->clock=clock;s->mic_base=s->mic_next=s->mic_read=first;
 tra_arm(&s->archive,owner,floor);return 0;
}
void cc_destroy(struct cancel_continuation*s){free(s);}
int cc_anchor(struct cancel_continuation*s,const struct k7_voice_anchor*a){
 if(!s||!a)return -EINVAL;
 if(s->error)return s->error;
 if(a->event==K7_VOICE_ANCHOR_SESSION_STOP)return fail(s,-EPIPE);
 if(a->generation<=s->archive.floor)return 0;
 if(a->flags||a->epoch!=s->clock||a->after_us<a->before_us)return fail(s,-EPIPE);
 if(a->event==K7_VOICE_ANCHOR_SESSION_STOP)return fail(s,-EPIPE);
 if(a->event==K7_VOICE_ANCHOR_TX_START&&s->begun&&a->generation!=s->archive.generation)return fail(s,-ESTALE);
 if(a->event==K7_VOICE_ANCHOR_AMP_OFF){
  if(a->result||s->muted||(s->begun&&a->generation!=s->archive.generation))return fail(s,-EPIPE);
  s->muted=true;s->mute=*a;
 }
 tra_anchor(&s->archive,a);
 if(atomic_load(&s->archive.state)==TRA_FAILED)return fail(s,s->archive.reason);
 return 0;
}
int cc_record(struct cancel_continuation*s,const struct k7_fifo_ref_record*r){
 if(!s||!r)return -EINVAL;
 if(s->error)return s->error;
 if(r->generation<=s->archive.floor)return 0;
 if(s->ended)return fail(s,-ESTALE);
 if(r->kind==K7_FIFO_REF_END&&(!s->archive.clear_seen||r->result!=s->archive.clear.result))return fail(s,-EPROTO);
 tra_record(&s->archive,r);
 unsigned state=atomic_load(&s->archive.state);
 if(state==TRA_FAILED)return fail(s,s->archive.reason);
 if(r->kind==K7_FIFO_REF_BEGIN){
  s->begun=true;s->ref_read=s->ref_next=r->first_enqueue;
  if(s->muted&&s->mute.generation!=r->generation)return fail(s,-ESTALE);
  return 0;
 }
 if(!s->begun||r->first_enqueue!=s->ref_next||s->ref_next<s->ref_read||
    s->ref_next-s->ref_read>CC_CAPACITY-r->samples)return fail(s,-ENOSPC);
 for(unsigned i=0;i<r->samples;i++)s->ref[(s->ref_next+i)%CC_CAPACITY]=r->pcm[i];
 s->ref_next+=r->samples;
 if(state==TRA_DONE){
  if(s->archive.uncertain_partial_pair)return fail(s,-ENOTSUP);
  uint64_t cut=s->archive.digital_complete_pair_next;
  if(cut<s->ref_read)return fail(s,-ERANGE); /* Cannot undo an AEC input. */
  s->ref_next=cut;s->ended=true;
 }
 return 0;
}
int cc_microphone(struct cancel_continuation*s,uint64_t owner,uint32_t input,uint64_t first,const int16_t*pcm,unsigned count){
 if(!s||!pcm||!count||count>320)return -EINVAL;
 if(s->error)return s->error;
 if(owner!=s->owner||input!=s->input||first!=s->mic_next||first>UINT64_MAX-count)return fail(s,-EPIPE);
 if(s->mic_next-s->mic_read>CC_CAPACITY-count)return fail(s,-ENOSPC);
 for(unsigned i=0;i<count;i++)s->mic[(first+i)%CC_CAPACITY]=pcm[i];
 s->mic_next+=count;return 0;
}
int cc_pop(struct cancel_continuation*s,struct cc_pair*out){
 if(!s||!out)return -EINVAL;
 if(s->error)return s->error;
 if(!s->begun||s->mic_next-s->mic_read<320)return 0;
 if(!s->ended&&(s->ref_next<s->ref_read||s->ref_next-s->ref_read<640))return 0;
 if(s->ended&&!s->muted)return 0;
 if(s->ended&&(s->mute.generation!=s->archive.generation||s->mute.rx_samples<s->mic_base))return fail(s,-ESTALE);
 if(s->ref_read>UINT64_MAX-320||s->mic_read>UINT64_MAX-320)return fail(s,-EOVERFLOW);
 memset(out,0,sizeof(*out));out->microphone_first=s->mic_read;out->reference_first=s->ref_read;out->input_epoch=s->input;out->hypothesis_only=true;
 for(unsigned i=0;i<320;i++){
  out->microphone[i]=s->mic[(s->mic_read+i)%CC_CAPACITY];
  if(s->ended&&s->ref_read+i>=s->ref_next)out->post_mute_samples++;
  else out->reference[i]=s->ref[(s->ref_read+i)%CC_CAPACITY];
 }
 s->mic_read+=320;s->ref_read+=320;return 1;
}
