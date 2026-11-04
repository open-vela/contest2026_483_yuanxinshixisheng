#include "k7_aec_speex10.h"
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include <errno.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct k7_aec_speex10 {
 SpeexEchoState *echo;
 SpeexPreprocessState *res;
 unsigned char *arena;
 struct k7_aec_speex10_stats stats;
 struct k7_aec_fa_identity identity;
 uint64_t previous_mic,previous_reference,older_mic,older_reference;
 unsigned initializing,valid,previous_valid,older_valid;
 int fault;
 jmp_buf unwind;
};
static atomic_flag library_busy=ATOMIC_FLAG_INIT;
static struct k7_aec_speex10 *active;
static int enter(struct k7_aec_speex10 *s)
{
 if(atomic_flag_test_and_set_explicit(&library_busy,memory_order_acquire))return -EBUSY;
 active=s;return 0;
}
static void leave(void)
{
 active=NULL;atomic_flag_clear_explicit(&library_busy,memory_order_release);
}
static void fault(int rc)
{
 if(!active)abort();
 active->fault=rc;longjmp(active->unwind,1);
}
void *k7_speex10_alloc(int bytes)
{
 size_t aligned,n;void *out;
 if(!active || !active->initializing || bytes<=0){fault(-EFAULT);return NULL;}
 n=(size_t)bytes;aligned=(active->stats.arena_used+15u)&~(size_t)15u;
 if(aligned>active->stats.arena_bytes || n>active->stats.arena_bytes-aligned){fault(-ENOMEM);return NULL;}
 out=active->arena+aligned;memset(out,0,n);
 active->stats.arena_used=aligned+n;active->stats.allocations++;return out;
}
void k7_speex10_free(void *p){(void)p;}
void *k7_speex10_realloc(void *p,int n){(void)p;(void)n;fault(-EFAULT);return NULL;}
void k7_speex10_fatal(const char *s,const char *file,int line)
{(void)s;(void)file;(void)line;fault(-EIO);}
void k7_speex10_warning(const char *s){(void)s;if(active)active->stats.warnings++;}
void k7_speex10_warning_int(const char *s,int n){(void)n;k7_speex10_warning(s);}
static int same(const struct k7_aec_fa_identity *a,const struct k7_aec_fa_identity *b)
{
 return a->owner_epoch==b->owner_epoch && a->mapping_epoch==b->mapping_epoch &&
  a->reference_instance==b->reference_instance && a->reference_epoch==b->reference_epoch &&
  a->input_epoch==b->input_epoch && a->clock_epoch==b->clock_epoch &&
  a->render_generation==b->render_generation;
}
static int valid_identity(const struct k7_aec_fa_identity *a)
{
 return a && a->owner_epoch && a->mapping_epoch && a->reference_instance &&
  a->reference_epoch && a->input_epoch && a->clock_epoch && a->render_generation;
}
static int reject(struct k7_aec_speex10 *s,int rc)
{
 s->valid=0;s->stats.ready=0;s->stats.last_error=rc;s->stats.rejected++;return rc;
}
static void invalidate(void *ctx)
{
 struct k7_aec_speex10 *s=ctx;
 if(s){s->valid=0;s->stats.ready=0;} /* No Speex reset, realloc or implicit flush. */
}
static int reset(void *ctx,const struct k7_aec_fa_identity *id,uint64_t epoch,
                 uint64_t mic,uint64_t reference)
{
 struct k7_aec_speex10 *s=ctx;int rc,rate=16000,off=0;
 SpeexEchoState *attached=NULL;
 if(!s)return -EINVAL;
 invalidate(s);
 if(!valid_identity(id) || !epoch || mic>UINT64_MAX-160 || reference>UINT64_MAX-160)
  return reject(s,-EINVAL);
 rc=enter(s);if(rc)return rc;
 if(setjmp(s->unwind)){
  rc=s->fault;s->initializing=0;s->echo=NULL;s->res=NULL;leave();return reject(s,rc);
 }
 /* Every explicit new lineage rebuilds BOTH states from the same bounded
  * arena. No public preprocess reset exists; MDF-only reset would be wrong. */
 s->echo=NULL;s->res=NULL;s->stats.arena_used=s->stats.allocations=0;
 s->stats.same_live_echo_attached=0;s->previous_valid=s->older_valid=0;
 s->initializing=1;
 s->echo=speex_echo_state_init(160,(int)s->stats.tail_samples);
 if(!s->echo || speex_echo_ctl(s->echo,SPEEX_ECHO_SET_SAMPLING_RATE,&rate))fault(-EIO);
 speex_echo_state_reset(s->echo);
 s->res=speex_preprocess_state_init(160,16000);
 if(!s->res || speex_preprocess_ctl(s->res,SPEEX_PREPROCESS_SET_AGC,&off) ||
    speex_preprocess_ctl(s->res,SPEEX_PREPROCESS_SET_VAD,&off) ||
    speex_preprocess_ctl(s->res,SPEEX_PREPROCESS_SET_ECHO_STATE,s->echo) ||
    speex_preprocess_ctl(s->res,SPEEX_PREPROCESS_GET_ECHO_STATE,&attached) ||
    attached!=s->echo)fault(-EIO);
 s->initializing=0;s->identity=*id;s->stats.state_epoch=epoch;
 s->stats.mic_next=mic;s->stats.reference_next=reference;
 s->stats.frames=s->stats.outputs=s->stats.warmup_frames=0;
 s->stats.same_live_echo_attached=1;s->stats.last_error=0;s->stats.resets++;
 s->valid=s->stats.ready=1;leave();return 0;
}
static int process160(void *ctx,const struct k7_aec_fa_half *h,
                      struct k7_aec_fa_backend_output *out)
{
 struct k7_aec_speex10 *s=ctx;struct k7_aec_fa_backend_output result;
 int16_t pcm[160];uint64_t warnings;int rc;
 if(!s || !h || !out)return -EINVAL;
 if(!s->valid)return -EPIPE;
 /* A foreign state/owner cannot mutate the current adaptation. */
 if(h->backend_state_epoch!=s->stats.state_epoch ||
    h->identity.owner_epoch!=s->identity.owner_epoch)return -ESTALE;
 if(!same(&h->identity,&s->identity))return reject(s,-EPIPE);
 if(!h->mic || !h->reference || !h->reference_zero || h->samples!=160)
  return reject(s,-EINVAL);
 if(h->mic_first>UINT64_MAX-160 || h->reference_first>UINT64_MAX-160)
  return reject(s,-EOVERFLOW);
 if(h->mic_first!=s->stats.mic_next || h->reference_first!=s->stats.reference_next)
  return reject(s,-EPIPE);
 for(unsigned i=0;i<160;i++)
  if(h->reference_zero[i]>1 || (h->reference_zero[i]&&h->reference[i]))return reject(s,-EINVAL);
 rc=enter(s);if(rc)return rc;
 if(setjmp(s->unwind)){rc=s->fault;leave();return reject(s,rc);}
 warnings=s->stats.warnings;
 speex_echo_cancellation(s->echo,h->mic,h->reference,pcm);
 (void)speex_preprocess_run(s->res,pcm);
 if(s->stats.warnings!=warnings){leave();return reject(s,-EIO);}
 s->stats.frames++;s->stats.mic_next=h->mic_first+160;
 s->stats.reference_next=h->reference_first+160;
 if(s->previous_valid){
  memset(&result,0,sizeof result);result.identity=s->identity;
  result.backend_state_epoch=s->stats.state_epoch;
  result.nominal_mic_first=s->previous_mic;
  result.nominal_reference_first=s->previous_reference;
  result.support=(struct k7_aec_fa_support){
   s->older_valid?s->older_mic:s->previous_mic,h->mic_first+160,
   s->older_valid?s->older_reference:s->previous_reference,h->reference_first+160,1};
  result.samples=160;memcpy(result.pcm,pcm,sizeof pcm);s->stats.outputs++;
 }else s->stats.warmup_frames++;
 unsigned had_previous=s->previous_valid;
 s->older_mic=s->previous_mic;s->older_reference=s->previous_reference;
 s->older_valid=had_previous;s->previous_mic=h->mic_first;
 s->previous_reference=h->reference_first;s->previous_valid=1;leave();
 if(!had_previous){out->samples=0;return K7_AEC_FA_WARMUP;}
 *out=result;return K7_AEC_FA_OUTPUT;
}
int k7_aec_speex10_create(const struct k7_aec_speex10_config *config,
                         struct k7_aec_speex10 **out)
{
 unsigned tail=config&&config->tail_samples?config->tail_samples:1600;
 size_t bytes=config&&config->arena_bytes?config->arena_bytes:K7_AEC_SPEEX10_ARENA_BYTES;
 struct k7_aec_speex10 *s;
 if(!out)return -EINVAL;
 *out=NULL;
 if(tail<320 || tail>5120 || tail%320 || bytes<1024 || bytes>K7_AEC_SPEEX10_ARENA_BYTES)
  return -EINVAL;
 s=calloc(1,sizeof(*s));if(!s)return -ENOMEM;
 s->arena=calloc(1,bytes);if(!s->arena){free(s);return -ENOMEM;}
 s->stats.arena_bytes=bytes;s->stats.tail_samples=tail;*out=s;return 0;
}
int k7_aec_speex10_destroy(struct k7_aec_speex10 **object)
{
 struct k7_aec_speex10 *s;volatile int rc;
 if(!object)return -EINVAL;
 s=*object;if(!s)return 0;
 rc=enter(s);if(rc)return rc;
 if(setjmp(s->unwind))rc=s->fault;
 else {
  /* Only fully initialized states are destructible. A failed init has only
   * arena storage and is reclaimed wholesale, without traversing partials. */
  if(s->res)speex_preprocess_state_destroy(s->res);
  if(s->echo)speex_echo_state_destroy(s->echo);
 }
 leave();free(s->arena);free(s);*object=NULL;return rc;
}
int k7_aec_speex10_get_stats(const struct k7_aec_speex10 *s,
                            struct k7_aec_speex10_stats *out)
{
 if(!s || !out)return -EINVAL;
 *out=s->stats;return 0;
}
const struct k7_aec_fa_backend *k7_aec_speex10_descriptor(void)
{
 static const struct k7_aec_fa_backend descriptor={K7_AEC_FA_BACKEND_ABI,
  "SpeexDSP-1.2.1-MDF160+same-live-RES160",160,1,reset,process160,invalidate};
 return &descriptor;
}
