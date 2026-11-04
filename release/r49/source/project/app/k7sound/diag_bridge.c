#if defined(K7_ECHO_AEC10_ADAPTER) && K7_ECHO_AEC10_ADAPTER
#include "diag_bridge.h"
#include "k7_aec_speex10.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* One existing worker owns every call. These caches retain actual nominal
 * data, not reconstructed or padded render/MIC. Never hot-switch a backend. */
struct nominal_record {
 struct k7_voice_frame raw;
 struct k7_aec_fa_identity identity;
 uint64_t reference_first;
 int16_t reference[320];uint8_t zero[320];unsigned valid;
};
struct diag_bridge {
 struct k7_aec_speex10 *backend;
 struct k7_aec_frame_adapter adapter;
 struct k7_aec_fa_token adapter_token;
 struct k7_aec_continuity_token token;
 struct vv_confirmed_wake_context context;
 struct nominal_record cache[2];unsigned cache_next;
 struct k7_aec_fa_output work;
 struct diag_bridge_paired_output candidate;
 struct k7_voice_frame history[16];unsigned head,count;
 uint64_t published_mic_next,published_ref_next,boundary_mic,boundary_ref;
 int failure;bool active,sealed,post_seal_frame;
};
static atomic_uint_fast64_t bridge_sequence;
static int lease_next(uint64_t *out){
 uint_fast64_t n=atomic_load(&bridge_sequence);
 do{if(n>=UINT64_MAX)return -EOVERFLOW;}
 while(!atomic_compare_exchange_weak(&bridge_sequence,&n,n+1));
 *out=(uint64_t)n+1;return 0;
}
static bool context_same(const struct vv_confirmed_wake_context*a,const struct vv_confirmed_wake_context*b){
 return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&
 a->model_epoch==b->model_epoch&&a->mode_epoch==b->mode_epoch&&a->clean_epoch==b->clean_epoch&&
 a->alignment_epoch==b->alignment_epoch&&a->asr_epoch==b->asr_epoch&&
 a->scope_incarnation==b->scope_incarnation&&a->lease_serial==b->lease_serial&&
 a->reply_id==b->reply_id&&a->tx_generation==b->tx_generation;
}
static struct k7_aec_fa_identity fa_id(const struct k7_aec_continuity_identity*i){
 return (struct k7_aec_fa_identity){i->owner_epoch,i->mapping_epoch,i->reference_instance,
 i->reference_epoch,i->input_epoch,i->clock_epoch,i->render_generation};
}
static bool identity_same(const struct k7_aec_fa_identity*a,const struct k7_aec_fa_identity*b){
 return a->owner_epoch==b->owner_epoch&&a->mapping_epoch==b->mapping_epoch&&
 a->reference_instance==b->reference_instance&&a->reference_epoch==b->reference_epoch&&
 a->input_epoch==b->input_epoch&&a->clock_epoch==b->clock_epoch&&a->render_generation==b->render_generation;
}
static int check(struct diag_bridge*b,uint64_t lease){
 if(!b)return -EINVAL;
 if(!lease||lease!=b->token.serial)return -ESTALE;
 return b->failure?b->failure:!b->active?-EPIPE:0;
}
static int fail(struct diag_bridge*b,int rc){
 if(b->adapter.active)(void)k7_aec_fa_cancel(&b->adapter,&b->adapter_token);
 b->failure=rc;b->active=false;b->count=0;
 memset(b->cache,0,sizeof b->cache);return rc;
}
int diag_bridge_create(struct diag_bridge**out){
 if(!out)return -EINVAL;
 *out=NULL;
 struct diag_bridge*b=calloc(1,sizeof(*b));if(!b)return -ENOMEM;
 int rc=k7_aec_speex10_create(NULL,&b->backend);
 if(!rc){const struct k7_aec_fa_backend*d=k7_aec_speex10_descriptor();
  if(!d||d->delay_samples!=160||d->max_warmup_halves!=1)rc=-ENOTSUP;
  else rc=k7_aec_fa_init(&b->adapter,d,b->backend);}
 if(rc){if(b->backend)(void)k7_aec_speex10_destroy(&b->backend);free(b);return rc;}
 *out=b;return 0;
}
int diag_bridge_begin(struct diag_bridge*b,const struct vv_confirmed_wake_context*c,
 const struct k7_aec_continuity_identity*i,uint64_t mic,uint64_t ref,uint64_t*out){
 if(!b)return -EINVAL;
 if(b->adapter.active)(void)k7_aec_fa_cancel(&b->adapter,&b->adapter_token);
 b->active=false;b->token.serial=0;b->failure=0;b->head=b->count=b->cache_next=0;
 b->sealed=b->post_seal_frame=false;memset(b->cache,0,sizeof b->cache);
 if(!c||!i||!out||!c->reply_id||!c->scope_incarnation||!c->lease_serial||
 c->owner_epoch!=i->owner_epoch||c->input_epoch!=i->input_epoch||c->tx_generation!=i->render_generation)
  return fail(b,-EINVAL);
 uint64_t lease;int rc=lease_next(&lease);if(rc)return fail(b,rc);
 struct k7_aec_fa_identity id=fa_id(i);
 rc=k7_aec_fa_begin(&b->adapter,&id,mic,ref,&b->adapter_token);if(rc)return fail(b,rc);
 b->token=(struct k7_aec_continuity_token){lease,*i,mic,ref,K7_AEC_CONTINUITY_BEFORE_CANCEL};
 b->context=*c;b->published_mic_next=mic;b->published_ref_next=ref;
 b->boundary_mic=b->boundary_ref=0;b->active=true;*out=lease;return 0;
}
int diag_bridge_process_paired(struct diag_bridge*b,uint64_t lease,
 const struct k7_aec_continuity_identity*before,const struct k7_aec_continuity_identity*after,
 const struct k7_voice_frame*f,const int16_t ref[320],uint64_t ref_first,
 const uint8_t zero[320],unsigned reference_valid,struct diag_bridge_paired_output*out){
 int rc=check(b,lease);if(rc)return rc;
 if(before&&before->owner_epoch!=b->token.identity.owner_epoch)return -ESTALE;
 if(!before||!after||!f||!out||!ref||!zero||f->flags||f->samples!=320||f->epoch!=before->input_epoch)
  return fail(b,-EINVAL);
 struct k7_aec_fa_identity id=fa_id(before),last=fa_id(after);
 struct nominal_record*r=&b->cache[b->cache_next];
 r->raw=*f;r->identity=id;r->reference_first=ref_first;
 memcpy(r->reference,ref,sizeof r->reference);memcpy(r->zero,zero,sizeof r->zero);r->valid=1;
 struct k7_aec_fa_input in={id,last,f->pcm,ref,zero,f->first_sample,ref_first,320,reference_valid};
 rc=k7_aec_fa_process(&b->adapter,&b->adapter_token,&in,&b->work);
 if(rc<0)return fail(b,rc);
 b->cache_next=(b->cache_next+1)%2;
 if(!rc)return 0;
 const struct k7_aec_fa_output*w=&b->work;struct nominal_record*nominal=NULL;
 for(unsigned n=0;n<2;n++)if(b->cache[n].valid&&
  b->cache[n].raw.first_sample==w->nominal_mic_first&&
  b->cache[n].reference_first==w->nominal_reference_first&&
  identity_same(&b->cache[n].identity,&w->identity))nominal=&b->cache[n];
 if(!nominal||w->samples!=320||w->qualified||w->startup_unavailable_samples||
 w->nominal_mic_first!=b->published_mic_next||w->nominal_reference_first!=b->published_ref_next||
 memcmp(nominal->zero,w->nominal_reference_zero,320))return fail(b,-EPROTO);
 struct diag_bridge_paired_output*c=&b->candidate;
 memset(c,0,sizeof(*c));c->raw=nominal->raw;c->residual=nominal->raw;
 memcpy(c->residual.pcm,w->pcm,sizeof c->residual.pcm);
 memcpy(c->reference,nominal->reference,sizeof c->reference);memcpy(c->zero,nominal->zero,320);
 c->reference_first=nominal->reference_first;c->identity=w->identity;
 c->backend_state_epoch=w->backend_state_epoch;c->support=w->support;
 c->accepted_mic_next=w->available_after_mic_next;c->accepted_ref_next=w->available_after_reference_next;
 c->published_mic_next=w->nominal_mic_first+320;c->published_ref_next=w->nominal_reference_first+320;
 c->backend_delay_samples=w->backend_delay_samples;c->publication_delay_samples=w->publication_delay_samples;
 c->startup_unavailable_samples=w->startup_unavailable_samples;
 for(unsigned n=0;n<320;n++)c->source_stop_zero_samples+=c->zero[n];
 b->published_mic_next=c->published_mic_next;b->published_ref_next=c->published_ref_next;
 unsigned slot=(b->head+b->count)%16;
 if(b->count==16){slot=b->head;b->head=(b->head+1)%16;}else b->count++;
 b->history[slot]=c->residual;
 if(b->sealed&&c->published_mic_next>b->boundary_mic)b->post_seal_frame=true;
 *out=*c;return 1;
}
/* Old ABI cannot represent nominal delayed raw/ref/zero. Refuse accidental
 * use when the new explicitly selected path is active. Old branch is intact. */
int diag_bridge_process(struct diag_bridge*b,uint64_t l,const struct k7_aec_continuity_identity*i,
 const struct k7_voice_frame*f,const int16_t r[320],uint64_t first,struct k7_voice_frame*out){
 (void)b;(void)l;(void)i;(void)f;(void)r;(void)first;(void)out;return -ENOTSUP;
}
int diag_bridge_aec10_status(struct diag_bridge*b,uint64_t lease,struct k7_aec_fa_status*out){
 int rc=check(b,lease);return rc?rc:k7_aec_fa_status(&b->adapter,out);
}
int diag_bridge_boundary(struct diag_bridge*b,uint64_t lease,const struct vv_confirmed_wake_context*c,
 uint64_t mic,uint64_t ref,uint64_t*out){
 int rc=check(b,lease);if(rc)return rc;if(!c||!out||!context_same(c,&b->context))return -ESTALE;
 if(b->sealed)return -EALREADY;
 struct k7_aec_fa_status st;rc=k7_aec_fa_status(&b->adapter,&st);if(rc)return fail(b,rc);
 if(st.processed_halves<2||mic!=st.mic_next||ref!=st.reference_next)return fail(b,-EPIPE);
 uint64_t next;rc=lease_next(&next);if(rc)return fail(b,rc);
 /* This is only a software boundary. No fa_cancel/reset/Speex call here. */
 b->token.serial=next;b->token.mic_first_sample=mic;b->token.reference_first_sample=ref;
 b->token.phase=K7_AEC_CONTINUITY_AFTER_CANCEL;b->sealed=true;
 b->boundary_mic=mic;b->boundary_ref=ref;*out=next;return 0;
}
int diag_bridge_seal(struct diag_bridge*b,uint64_t lease,const struct vv_barge_ticket*t,uint64_t*out){
 int rc=check(b,lease);if(rc)return rc;if(!t||!out)return -EINVAL;
 if(!t->id||!context_same(&t->context,&b->context))return -ESTALE;
 if(t->onset_first>=t->onset_next||t->onset_next!=b->published_mic_next)return fail(b,-EPIPE);
 struct k7_aec_fa_status st;rc=k7_aec_fa_status(&b->adapter,&st);if(rc)return fail(b,rc);
 return diag_bridge_boundary(b,lease,&t->context,st.mic_next,st.reference_next,out);
}
int diag_bridge_preroll(struct diag_bridge*b,uint64_t lease,struct diag_preroll*out){
 int rc=check(b,lease);if(rc)return rc;if(!out)return -EINVAL;
 if(!b->sealed||!b->post_seal_frame||b->count!=16)return -EAGAIN;
 struct diag_preroll r={.count=16,.qualified=0};
 for(unsigned n=0;n<16;n++){r.frames[n]=b->history[(b->head+n)%16];}
 *out=r;return 0;
}
int diag_bridge_tail_unqualified(void*a,const struct vv_barge_ticket*t,const struct k7_voice_frame*r,
 struct k7_voice_frame*out,enum vv_barge_material*m){(void)a;(void)t;(void)r;(void)out;(void)m;return -ENOTSUP;}
int diag_bridge_invalidate(struct diag_bridge*b,int reason){if(!b)return -EINVAL;return fail(b,reason?reason:-EPIPE);}
int diag_bridge_status(struct diag_bridge*b,struct k7_aec_continuity_status*c,struct k7_aec_stats*s){
 (void)b;(void)c;(void)s;return -ENOTSUP; /* Never fabricate old320 statistics. */
}
void diag_bridge_destroy(struct diag_bridge*b){if(b){
 if(b->adapter.active)(void)k7_aec_fa_cancel(&b->adapter,&b->adapter_token);
 (void)k7_aec_speex10_destroy(&b->backend);memset(b,0,sizeof(*b));free(b);}}

#else
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include "diag_bridge.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct diag_bridge {struct k7_aec *aec;struct k7_aec_continuity_token token;
 struct vv_confirmed_wake_context context;struct k7_voice_frame history[16];
 unsigned head,count;bool post_seal_frame;int failure;bool active,sealed;};
static bool same(const struct vv_confirmed_wake_context*a,const struct vv_confirmed_wake_context*b){
 return a->owner_epoch==b->owner_epoch&&a->input_epoch==b->input_epoch&&
 a->model_epoch==b->model_epoch&&a->mode_epoch==b->mode_epoch&&a->clean_epoch==b->clean_epoch&&
 a->alignment_epoch==b->alignment_epoch&&a->asr_epoch==b->asr_epoch&&
 a->scope_incarnation==b->scope_incarnation&&a->lease_serial==b->lease_serial&&
 a->reply_id==b->reply_id&&a->tx_generation==b->tx_generation;}
static int check(struct diag_bridge*b,uint64_t lease){
 if(!b)return -EINVAL;
 if(!lease||lease!=b->token.serial)return -ESTALE;
 return b->failure?b->failure:!b->active?-EPIPE:0;}
static int fail(struct diag_bridge*b,int rc){b->failure=rc;b->count=0;return rc;}
int diag_bridge_create(struct diag_bridge**out){
 if(!out)return -EINVAL;
 *out=NULL;
 struct diag_bridge*b=calloc(1,sizeof(*b));if(!b)return -ENOMEM;
 int rc=k7_aec_create(NULL,&b->aec);if(rc){free(b);return rc;}*out=b;return 0;}
int diag_bridge_begin(struct diag_bridge*b,const struct vv_confirmed_wake_context*c,
 const struct k7_aec_continuity_identity*i,uint64_t mic,uint64_t ref,uint64_t*out){
 if(!b)return -EINVAL;
 b->active=false;b->count=b->head=0;b->failure=0;b->sealed=false;b->post_seal_frame=false;
 if(!c||!i||!out||!c->reply_id||!c->scope_incarnation||!c->lease_serial||
 c->owner_epoch!=i->owner_epoch||c->input_epoch!=i->input_epoch||c->tx_generation!=i->render_generation)
 return fail(b,-EINVAL);
 struct k7_aec_continuity_token t;int rc=k7_aec_diagnostic_continuity_begin(b->aec,i,mic,ref,&t);
 if(rc)return fail(b,rc);
 b->context=*c;b->token=t;b->active=true;*out=t.serial;return 0;}
int diag_bridge_process(struct diag_bridge*b,uint64_t lease,const struct k7_aec_continuity_identity*i,
 const struct k7_voice_frame*f,const int16_t ref[320],uint64_t ref_first,struct k7_voice_frame*out){
 int rc=check(b,lease);if(rc)return rc;
 if(i&&i->owner_epoch!=b->token.identity.owner_epoch)return -ESTALE;
 if(!i||!f||!out||f->flags||f->samples!=320||f->epoch!=i->input_epoch)return fail(b,-EINVAL);
 struct k7_aec_continuity_frame v={*i,f->pcm,ref,f->first_sample,ref_first,320};
 struct k7_voice_frame result=*f;
 rc=k7_aec_diagnostic_continuity_process(b->aec,&b->token,&v,result.pcm);
 if(rc)return rc==-ESTALE?rc:fail(b,rc);
 unsigned slot=(b->head+b->count)%16;
 if(b->count==16){slot=b->head;b->head=(b->head+1)%16;}else b->count++;
 b->history[slot]=result;if(b->sealed)b->post_seal_frame=true;*out=result;return 0;}
int diag_bridge_seal(struct diag_bridge*b,uint64_t lease,const struct vv_barge_ticket*t,uint64_t*out){
 int rc=check(b,lease);if(rc)return rc;
 if(!t||!out)return -EINVAL;
 if(!t->id||!same(&b->context,&t->context))return -ESTALE;
 if(b->sealed)return -EALREADY;
 struct k7_aec_continuity_status st;rc=k7_aec_diagnostic_continuity_status(b->aec,&st);if(rc)return fail(b,rc);
 if(t->onset_first>=t->onset_next||t->onset_next!=st.mic_next_sample)return fail(b,-EPIPE);
 struct k7_aec_continuity_token token;
 rc=k7_aec_diagnostic_cancel_seal(b->aec,&b->token,st.mic_next_sample,st.reference_next_sample,&token);
 if(rc)return fail(b,rc);
 b->token=token;b->sealed=true;*out=token.serial;return 0;}
int diag_bridge_preroll(struct diag_bridge*b,uint64_t lease,struct diag_preroll*out){
 int rc=check(b,lease);if(rc)return rc;if(!out)return -EINVAL;
 if(!b->sealed||!b->post_seal_frame||b->count!=16)return -EAGAIN;
 struct diag_preroll r={.count=16,.qualified=0};
 for(unsigned n=0;n<16;n++)r.frames[n]=b->history[(b->head+n)%16];
 *out=r;return 0;}
int diag_bridge_tail_unqualified(void*a,const struct vv_barge_ticket*t,const struct k7_voice_frame*r,
 struct k7_voice_frame*out,enum vv_barge_material*m){(void)a;(void)t;(void)r;(void)out;(void)m;return -ENOTSUP;}
void diag_bridge_destroy(struct diag_bridge*b){if(b){(void)k7_aec_destroy(b->aec);memset(b,0,sizeof(*b));free(b);}}

int diag_bridge_boundary(struct diag_bridge*b,uint64_t lease,
 const struct vv_confirmed_wake_context*c,uint64_t mic,uint64_t ref,uint64_t*out){
 int rc=check(b,lease);if(rc)return rc;
 if(!c||!out||!same(&b->context,c))return -ESTALE;
 if(b->sealed)return -EALREADY;
 struct k7_aec_continuity_token t;
 rc=k7_aec_diagnostic_cancel_seal(b->aec,&b->token,mic,ref,&t);
 if(rc)return fail(b,rc);
 b->token=t;b->sealed=true;*out=t.serial;return 0;
}
int diag_bridge_invalidate(struct diag_bridge*b,int reason){
 if(!b)return -EINVAL;
 b->active=false;b->count=0;b->failure=reason?reason:-EPIPE;
 return k7_aec_invalidate(b->aec,K7_AEC_BAD_REFERENCE);
}
int diag_bridge_status(struct diag_bridge*b,struct k7_aec_continuity_status*c,struct k7_aec_stats*s){
 if(!b||!c||!s)return -EINVAL;
 int rc=k7_aec_diagnostic_continuity_status(b->aec,c);return rc?rc:k7_aec_get_stats(b->aec,s);
}

#endif

#endif /* original320 bridge preserved */
