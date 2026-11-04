#include "k7_aec_frame_adapter.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>

static atomic_uint_fast64_t serial_sequence;
static int same(const struct k7_aec_fa_identity *a,
                const struct k7_aec_fa_identity *b)
{
 return a->owner_epoch==b->owner_epoch && a->mapping_epoch==b->mapping_epoch &&
  a->reference_instance==b->reference_instance &&
  a->reference_epoch==b->reference_epoch && a->input_epoch==b->input_epoch &&
  a->clock_epoch==b->clock_epoch && a->render_generation==b->render_generation;
}
static int identity_valid(const struct k7_aec_fa_identity *a)
{
 return a && a->owner_epoch && a->mapping_epoch && a->reference_instance &&
  a->reference_epoch && a->input_epoch && a->clock_epoch && a->render_generation;
}
static int serial(uint64_t *out)
{
 uint_fast64_t prior=atomic_load(&serial_sequence);
 do { if(prior>=UINT64_MAX)return -EOVERFLOW; }
 while(!atomic_compare_exchange_weak(&serial_sequence,&prior,prior+1));
 *out=(uint64_t)prior+1;return 0;
}
static void revoke(struct k7_aec_frame_adapter *a)
{
 if(a->backend_live)a->backend.invalidate(a->backend_context);
 a->backend_live=0;a->active=0;a->pending=0;
 memset(a->history,0,sizeof(a->history));
}
static int fail(struct k7_aec_frame_adapter *a,int rc)
{
 revoke(a);return rc;
}
static int current(const struct k7_aec_frame_adapter *a,
                   const struct k7_aec_fa_token *t)
{
 if(!a || !a->initialized)return -EINVAL;
 if(!t || !t->serial || t->serial!=a->token.serial ||
    !same(&t->identity,&a->token.identity))return -ESTALE;
 return a->active?0:-EPIPE;
}
int k7_aec_fa_init(struct k7_aec_frame_adapter *a,
 const struct k7_aec_fa_backend *b,void *context)
{
 if(!a || !b || b->abi!=K7_AEC_FA_BACKEND_ABI || !b->name || !*b->name ||
    !b->reset || !b->process160 || !b->invalidate ||
    b->delay_samples>K7_AEC_FA_MAX_DELAY_SAMPLES || b->delay_samples%160 ||
    b->max_warmup_halves>32 || b->max_warmup_halves<b->delay_samples/160)
  return -EINVAL;
 memset(a,0,sizeof(*a));a->backend=*b;a->backend_context=context;
 a->initialized=1;return 0;
}
int k7_aec_fa_begin(struct k7_aec_frame_adapter *a,
 const struct k7_aec_fa_identity *id,uint64_t mic,uint64_t ref,
 struct k7_aec_fa_token *out)
{
 uint64_t sequence;int rc;
 if(!a || !a->initialized)return -EINVAL;
 revoke(a);a->token.serial=0;
 if(!out || !identity_valid(id))return -EINVAL;
 if(mic>UINT64_MAX-320 || ref>UINT64_MAX-320)return -EOVERFLOW;
 rc=serial(&sequence);if(rc)return rc;
 rc=a->backend.reset(a->backend_context,id,sequence,mic,ref);
 a->backend_live=1;
 if(rc)return fail(a,rc<0?rc:-EPROTO);
 a->token=(struct k7_aec_fa_token){sequence,*id};
 a->mic_origin=a->mic_next=mic;a->reference_origin=a->reference_next=ref;
 a->halves=a->output_mic_next=a->output_reference_next=a->startup_unavailable=0;
 a->published_frames=0;a->published_mic_next=mic;a->published_reference_next=ref;
 a->warmup_halves=a->output_seen=0;a->active=1;*out=a->token;return 0;
}
static const uint8_t *zero_at(const struct k7_aec_frame_adapter *a,
                            uint64_t mic,uint64_t ref)
{
 for(unsigned i=0;i<K7_AEC_FA_HISTORY_HALVES;i++)
  if(a->history[i].valid && a->history[i].mic_first==mic &&
     a->history[i].reference_first==ref)return a->history[i].zero;
 return NULL;
}
static int support_valid(const struct k7_aec_frame_adapter *a,
 const struct k7_aec_fa_backend_output *o,const struct k7_aec_fa_half *h)
{
 unsigned delay=a->backend.delay_samples;
 if(o->samples!=160 || !same(&o->identity,&a->token.identity) ||
    o->backend_state_epoch!=a->token.serial ||
    h->mic_first-a->mic_origin<delay ||
    h->reference_first-a->reference_origin<delay)return 0;
 if(o->nominal_mic_first!=h->mic_first-delay ||
    o->nominal_reference_first!=h->reference_first-delay)return 0;
 return o->support.mic_first>=a->mic_origin &&
  o->support.mic_first<=o->nominal_mic_first &&
  o->support.mic_next>=o->nominal_mic_first+160 &&
  o->support.mic_next<=h->mic_first+160 &&
  o->support.reference_first>=a->reference_origin &&
  o->support.reference_first<=o->nominal_reference_first &&
  o->support.reference_next>=o->nominal_reference_first+160 &&
  o->support.reference_next<=h->reference_first+160 &&
  o->support.adaptive_history<=1;
}
static void combine(struct k7_aec_frame_adapter *a,
 const struct k7_aec_fa_backend_output *b,const uint8_t *zero,
 struct k7_aec_fa_output *out)
{
 const struct k7_aec_fa_backend_output *p=&a->pending_output;
 memset(out,0,sizeof(*out));out->identity=p->identity;
 out->backend_state_epoch=p->backend_state_epoch;
 out->nominal_mic_first=p->nominal_mic_first;
 out->nominal_reference_first=p->nominal_reference_first;
 out->samples=320;out->backend_delay_samples=a->backend.delay_samples;
 out->startup_unavailable_samples=a->startup_unavailable;
 out->support.mic_first=p->support.mic_first<b->support.mic_first?
  p->support.mic_first:b->support.mic_first;
 out->support.reference_first=p->support.reference_first<b->support.reference_first?
  p->support.reference_first:b->support.reference_first;
 out->support.mic_next=p->support.mic_next>b->support.mic_next?
  p->support.mic_next:b->support.mic_next;
 out->support.reference_next=p->support.reference_next>b->support.reference_next?
  p->support.reference_next:b->support.reference_next;
 out->support.adaptive_history=p->support.adaptive_history|b->support.adaptive_history;
 memcpy(out->pcm,p->pcm,sizeof(p->pcm));
 memcpy(out->pcm+160,b->pcm,sizeof(b->pcm));
 memcpy(out->nominal_reference_zero,a->pending_zero,160);
 memcpy(out->nominal_reference_zero+160,zero,160);
}
int k7_aec_fa_process(struct k7_aec_frame_adapter *a,
 const struct k7_aec_fa_token *token,const struct k7_aec_fa_input *f,
 struct k7_aec_fa_output *out)
{
 struct k7_aec_fa_output complete;unsigned ready=0;int rc=current(a,token);
 if(rc)return rc;
 if(f && f->before.owner_epoch!=a->token.identity.owner_epoch)return -ESTALE;
 if(!f || !out || !f->mic || !f->reference || !f->reference_zero ||
    f->samples!=320 || f->reference_valid!=1)return fail(a,-EINVAL);
 if(!same(&f->before,&a->token.identity) || !same(&f->after,&f->before))
  return fail(a,-EPIPE);
 if(f->mic_first>UINT64_MAX-320 || f->reference_first>UINT64_MAX-320 ||
    a->halves>UINT64_MAX-2)return fail(a,-EOVERFLOW);
 if(f->mic_first!=a->mic_next || f->reference_first!=a->reference_next)
  return fail(a,-EPIPE);
 for(unsigned i=0;i<320;i++)
  if(f->reference_zero[i]>1 || (f->reference_zero[i] && f->reference[i]))
   return fail(a,-EINVAL);
 for(unsigned half=0;half<2;half++){
  struct k7_aec_fa_half h={a->token.identity,a->token.serial,f->mic+160*half,
   f->reference+160*half,f->reference_zero+160*half,
   f->mic_first+160*half,f->reference_first+160*half,160};
  struct k7_aec_fa_backend_output b={0};
  struct k7_aec_fa_history *history=&a->history[a->halves%K7_AEC_FA_HISTORY_HALVES];
  history->valid=1;history->mic_first=h.mic_first;
  history->reference_first=h.reference_first;memcpy(history->zero,h.reference_zero,160);
  rc=a->backend.process160(a->backend_context,&h,&b);
  if(rc<0)return fail(a,rc);
  if(!same(&f->before,&a->token.identity) || !same(&f->after,&a->token.identity))
   return fail(a,-EPIPE);
  if(rc!=K7_AEC_FA_WARMUP && rc!=K7_AEC_FA_OUTPUT)return fail(a,-EPROTO);
  a->mic_next=h.mic_first+160;a->reference_next=h.reference_first+160;a->halves++;
  if(rc==K7_AEC_FA_WARMUP){
   if(b.samples || a->output_seen || ++a->warmup_halves>a->backend.max_warmup_halves)
    return fail(a,-EPROTO);
   continue;
  }
  if(!support_valid(a,&b,&h))return fail(a,-EPROTO);
  const uint8_t *zero=zero_at(a,b.nominal_mic_first,b.nominal_reference_first);
  if(!zero)return fail(a,-EPROTO);
  if(a->output_seen && (b.nominal_mic_first!=a->output_mic_next ||
     b.nominal_reference_first!=a->output_reference_next))return fail(a,-EPROTO);
  if(!a->output_seen)a->startup_unavailable=b.nominal_mic_first-a->mic_origin;
  a->output_seen=1;a->output_mic_next=b.nominal_mic_first+160;
  a->output_reference_next=b.nominal_reference_first+160;
  if(!a->pending){a->pending_output=b;memcpy(a->pending_zero,zero,160);a->pending=1;}
  else {
   if(ready || a->pending_output.nominal_mic_first+160!=b.nominal_mic_first ||
      a->pending_output.nominal_reference_first+160!=b.nominal_reference_first)
    return fail(a,-EPROTO);
   combine(a,&b,zero,&complete);a->pending=0;ready=1;
  }
 }
 if(!ready)return 0;
 complete.available_after_mic_next=a->mic_next;
 complete.available_after_reference_next=a->reference_next;
 complete.publication_delay_samples=(unsigned)(a->mic_next-
  (complete.nominal_mic_first+320));
 a->published_frames++;a->published_mic_next=complete.nominal_mic_first+320;
 a->published_reference_next=complete.nominal_reference_first+320;
 *out=complete;return 1;
}
int k7_aec_fa_cancel(struct k7_aec_frame_adapter *a,
 const struct k7_aec_fa_token *token)
{
 int rc=current(a,token);if(rc)return rc;revoke(a);return 0;
}
int k7_aec_fa_status(const struct k7_aec_frame_adapter *a,
 struct k7_aec_fa_status *out)
{
 if(!a || !a->initialized || !out)return -EINVAL;
 *out=(struct k7_aec_fa_status){.backend_name=a->backend.name,
  .mic_next=a->mic_next,.reference_next=a->reference_next,
  .processed_halves=a->halves,.startup_unavailable_samples=a->startup_unavailable,
  .published_frames=a->published_frames,.published_mic_next=a->published_mic_next,
  .published_reference_next=a->published_reference_next,.active=a->active,
  .pending_samples=a->pending?160u:0u,.warmup_halves=a->warmup_halves,
  .backend_delay_samples=a->backend.delay_samples};return 0;
}
