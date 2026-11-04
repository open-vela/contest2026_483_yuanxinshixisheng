#include "k7_aec.h"
#include "speex/speex_echo.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct k7_aec {
  SpeexEchoState *state;
  unsigned char *arena;
  struct k7_aec_stats stats;
  uint64_t next;
  uint32_t epoch, generation;
  int initializing, fault, diagnostic_active;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  struct k7_aec_continuity_token continuity_token;
  uint64_t continuity_mic_next, continuity_reference_next, continuity_frames;
  int continuity_active;

#endif
  jmp_buf unwind;
};
/* Hooks have no context argument. A nonblocking guard makes their ownership
 * explicit even when multiple workers create separate AEC instances. */
static atomic_flag library_busy = ATOMIC_FLAG_INIT;
static struct k7_aec *active;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
static atomic_uint_fast64_t continuity_sequence;

#endif
static const int16_t silence[K7_AEC_FRAME_SAMPLES];
static int enter(struct k7_aec *ctx)
{
  if (atomic_flag_test_and_set_explicit(&library_busy, memory_order_acquire))
    return -EBUSY;
  active = ctx;
  return 0;
}
static void leave(void)
{
  active = NULL;
  atomic_flag_clear_explicit(&library_busy, memory_order_release);
}
static void fail_library(int code)
{
  /* These symbols are private to the optional module and called only through
   * guarded entry points. An unguarded upstream call is a programming error. */
  if (!active) abort();
  active->fault = code;
  longjmp(active->unwind, 1);
}
void *k7_aec_speex_alloc(int size)
{
  size_t aligned, count;
  void *result;
  if (!active || !active->initializing || size <= 0) {
    fail_library(-EFAULT);
    return NULL;
  }
  count = (size_t)size;
  aligned = (active->stats.arena_used + 15u) & ~(size_t)15u;
  if (aligned > active->stats.arena_bytes ||
      count > active->stats.arena_bytes - aligned) {
    fail_library(-ENOMEM);
    return NULL;
  }
  result = active->arena + aligned;
  memset(result, 0, count);
  active->stats.arena_used = aligned + count;
  active->stats.allocations++;
  return result;
}
void k7_aec_speex_free(void *ptr) { (void)ptr; }
void *k7_aec_speex_realloc(void *ptr, int size)
{
  (void)ptr; (void)size;
  fail_library(-EFAULT); /* Not part of the pinned echo/FFT closure. */
  return NULL;
}
void k7_aec_speex_fatal(const char *str, const char *file, int line)
{
  (void)str; (void)file; (void)line;
  fail_library(-EIO);
}
void k7_aec_speex_warning(const char *str)
{
  (void)str;
  if (active) active->stats.warnings++;
}
void k7_aec_speex_warning_int(const char *str, int value)
{
  (void)value; k7_aec_speex_warning(str);
}
static int invalidate(struct k7_aec *ctx, unsigned reason, int result)
{
  ctx->stats.ready = 0;
  ctx->diagnostic_active = 0;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  ctx->continuity_active = 0;

#endif
  ctx->stats.last_discontinuity = reason;
  ctx->stats.rejected++;
  return result;
}
int k7_aec_create(const struct k7_aec_config *config, struct k7_aec **out)
{
  uint32_t tail = config && config->tail_samples ? config->tail_samples : 1600u;
  size_t bytes = config && config->arena_bytes ? config->arena_bytes : K7_AEC_DEFAULT_ARENA_BYTES;
  struct k7_aec *ctx;
  int rate = K7_AEC_SAMPLE_RATE;
  if (!out) return -EINVAL;
  *out = NULL;
  if (tail < 320u || tail > 5120u || tail % 320u ||
      bytes < 1024u || bytes > K7_AEC_DEFAULT_ARENA_BYTES) return -EINVAL;
  ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return -ENOMEM;
  ctx->arena = calloc(1, bytes);
  if (!ctx->arena) { free(ctx); return -ENOMEM; }
  ctx->stats.arena_bytes = bytes;
  if (enter(ctx)) { free(ctx->arena); free(ctx); return -EBUSY; }
  if (setjmp(ctx->unwind)) {
    int error = ctx->fault;
    leave(); free(ctx->arena); free(ctx); return error;
  }
  ctx->initializing = 1;
  ctx->state = speex_echo_state_init(320, (int)tail);
  if (!ctx->state || speex_echo_ctl(ctx->state, SPEEX_ECHO_SET_SAMPLING_RATE, &rate))
    fail_library(-EIO);
  ctx->initializing = 0;
  leave();
  *out = ctx;
  return 0;
}
int k7_aec_align(struct k7_aec *ctx, uint32_t epoch, uint32_t generation,
                 uint64_t first, uint32_t flags)
{
  if (!ctx) return -EINVAL;
  ctx->diagnostic_active = 0;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  ctx->continuity_active = 0;

#endif
  if (!epoch || first > UINT64_MAX - 320u || flags != K7_AEC_ALIGNMENT_VERIFIED)
    return invalidate(ctx, K7_AEC_BAD_REFERENCE, -EINVAL);
  if (enter(ctx)) return -EBUSY;
  if (setjmp(ctx->unwind)) {
    leave(); return invalidate(ctx, K7_AEC_LIBRARY_FAULT, ctx->fault);
  }
  speex_echo_state_reset(ctx->state);
  ctx->epoch = epoch; ctx->generation = generation; ctx->next = first;
  ctx->stats.ready = 1; ctx->stats.resets++;
  ctx->stats.last_discontinuity = 0;
  leave();
  return 0;
}
int k7_aec_process(struct k7_aec *ctx, const struct k7_aec_frame *f, int16_t out[320])
{
  const int16_t *render;
  int16_t result[320];
  uint64_t warnings;
  unsigned i, kind;
  if (!ctx) return -EINVAL;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  ctx->continuity_active = 0;

#endif
  if (!f || !out || !f->mic || f->samples != 320)
    return invalidate(ctx, K7_AEC_BAD_REFERENCE, -EINVAL);
  if (f->flags & K7_AEC_CANCELLED_TAIL)
    return invalidate(ctx, K7_AEC_CANCEL_TAIL, -EPIPE);
  kind = f->flags & (K7_AEC_REFERENCE_VALID | K7_AEC_EXPLICIT_MUTED);
  if (!(f->flags & K7_AEC_ALIGNMENT_VERIFIED) || (f->flags & ~7u) ||
      (kind != K7_AEC_REFERENCE_VALID && kind != K7_AEC_EXPLICIT_MUTED) ||
      (kind == K7_AEC_REFERENCE_VALID && (!f->render || !f->render_generation)) ||
      (kind == K7_AEC_EXPLICIT_MUTED && (f->render || f->render_generation)))
    return invalidate(ctx, K7_AEC_BAD_REFERENCE, -EINVAL);
  if (!ctx->stats.ready) { ctx->stats.rejected++; return -EPIPE; }
  if (f->clock_epoch != ctx->epoch)
    return invalidate(ctx, K7_AEC_CLOCK_CHANGE, -EPIPE);
  if (f->render_generation != ctx->generation)
    return invalidate(ctx, K7_AEC_GENERATION_CHANGE, -EPIPE);
  if (f->mic_first_sample != ctx->next || f->render_first_sample != ctx->next ||
      f->mic_first_sample > UINT64_MAX - 320u)
    return invalidate(ctx, K7_AEC_GAP, -EPIPE);
  if (enter(ctx)) return -EBUSY;
  if (setjmp(ctx->unwind)) {
    leave(); return invalidate(ctx, K7_AEC_LIBRARY_FAULT, ctx->fault);
  }
  render = kind == K7_AEC_EXPLICIT_MUTED ? silence : f->render;
  for (i = 0; i < 320u; ++i) {
    if (f->mic[i] == INT16_MAX || f->mic[i] == INT16_MIN) ctx->stats.clipped_mic++;
    if (render[i] == INT16_MAX || render[i] == INT16_MIN) ctx->stats.clipped_render++;
  }
  warnings = ctx->stats.warnings;
  speex_echo_cancellation(ctx->state, f->mic, render, result);
  if (ctx->stats.warnings != warnings) {
    leave(); return invalidate(ctx, K7_AEC_LIBRARY_FAULT, -EIO);
  }
  ctx->next += 320u; ctx->stats.frames++;
  memcpy(out, result, sizeof(result));
  leave();
  return 0;
}
int k7_aec_invalidate(struct k7_aec *ctx, enum k7_aec_discontinuity reason)
{
  if (!ctx || reason < K7_AEC_GAP || reason > K7_AEC_LIBRARY_FAULT) return -EINVAL;
  ctx->stats.ready = 0; ctx->stats.last_discontinuity = (unsigned)reason;
  ctx->diagnostic_active = 0;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  ctx->continuity_active = 0;

#endif
  return 0;
}
int k7_aec_get_stats(struct k7_aec *ctx, struct k7_aec_stats *out)
{
  if (!ctx || !out) return -EINVAL;
  *out = ctx->stats;
  return 0;
}
int k7_aec_destroy(struct k7_aec *ctx)
{
  if (!ctx) return 0;
  /* All upstream allocations live in this context's arena, including FFTs. */
  free(ctx->arena); free(ctx);
  return 0;
}
int k7_aec_float_to_s16(const float *input, size_t count, int16_t *out)
{
  size_t i;
  if (!input || !out || !count || count > 320u) return -EINVAL;
  for (i = 0; i < count; ++i)
    if (!isfinite(input[i]) || input[i] < -1.0f || input[i] > 1.0f) return -ERANGE;
  for (i = 0; i < count; ++i)
    out[i] = input[i] == -1.0f ? INT16_MIN : (int16_t)(input[i] * 32767.0f);
  return 0;
}

int k7_aec_diagnostic_reset(struct k7_aec *ctx)
{
  if (!ctx) return -EINVAL;
  ctx->stats.ready = 0;
  ctx->diagnostic_active = 0;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  ctx->continuity_active = 0;

#endif
  if (enter(ctx)) return -EBUSY;
  if (setjmp(ctx->unwind)) {
    leave(); return invalidate(ctx, K7_AEC_LIBRARY_FAULT, ctx->fault);
  }
  speex_echo_state_reset(ctx->state);
  ctx->diagnostic_active = 1;
  leave();
  return 0;
}

int k7_aec_diagnostic_process(struct k7_aec *ctx, const int16_t *mic,
  const int16_t *reference, size_t samples, int16_t out[320])
{
  int16_t result[320];
  uint64_t warnings;
  if (!ctx) return -EINVAL;
  ctx->stats.ready = 0;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  ctx->continuity_active = 0;

#endif
  if (!mic || !reference || !out || samples != 320)
    return invalidate(ctx, K7_AEC_BAD_REFERENCE, -EINVAL);
  if (!ctx->diagnostic_active) return -EPIPE;
  if (enter(ctx)) return -EBUSY;
  if (setjmp(ctx->unwind)) {
    leave(); return invalidate(ctx, K7_AEC_LIBRARY_FAULT, ctx->fault);
  }
  warnings = ctx->stats.warnings;
  speex_echo_cancellation(ctx->state, mic, reference, result);
  if (ctx->stats.warnings != warnings) {
    leave(); return invalidate(ctx, K7_AEC_LIBRARY_FAULT, -EIO);
  }
  memcpy(out, result, sizeof(result));
  leave();
  return 0;
}
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY

/* This optional path never establishes acoustic qualification. The existing
 * process path still rejects CANCELLED_TAIL and verified align still resets. */
static int continuity_identity_valid(const struct k7_aec_continuity_identity *i)
{
  return i && i->owner_epoch && i->mapping_epoch && i->reference_instance &&
    i->reference_epoch && i->input_epoch && i->clock_epoch && i->render_generation;
}
static int continuity_identity_same(const struct k7_aec_continuity_identity *a,
  const struct k7_aec_continuity_identity *b)
{
  return a->owner_epoch==b->owner_epoch && a->mapping_epoch==b->mapping_epoch &&
    a->reference_instance==b->reference_instance && a->reference_epoch==b->reference_epoch &&
    a->input_epoch==b->input_epoch && a->clock_epoch==b->clock_epoch &&
    a->render_generation==b->render_generation;
}
static int continuity_token_same(const struct k7_aec_continuity_token *a,
  const struct k7_aec_continuity_token *b)
{
  return a && a->serial && a->serial==b->serial && a->phase==b->phase &&
    a->mic_first_sample==b->mic_first_sample &&
    a->reference_first_sample==b->reference_first_sample &&
    continuity_identity_same(&a->identity,&b->identity);
}
static int continuity_serial(uint64_t *out)
{
  uint_fast64_t prior=atomic_load(&continuity_sequence);
  do { if(prior>=UINT64_MAX)return -EOVERFLOW; }
  while(!atomic_compare_exchange_weak(&continuity_sequence,&prior,prior+1));
  *out=prior+1;return 0;
}
int k7_aec_diagnostic_continuity_begin(struct k7_aec *ctx,
  const struct k7_aec_continuity_identity *id, uint64_t mic_first,
  uint64_t ref_first, struct k7_aec_continuity_token *out)
{
  uint64_t serial=0;
  int rc;
  if(!ctx)return -EINVAL;
  /* An explicit new begin revokes the previous stream, even on failure. */
  ctx->continuity_active=0;
  if(!out || !continuity_identity_valid(id) || mic_first>UINT64_MAX-320u ||
     ref_first>UINT64_MAX-320u)return invalidate(ctx,K7_AEC_BAD_REFERENCE,-EINVAL);
  rc=continuity_serial(&serial);
  if(rc)return invalidate(ctx,K7_AEC_LIBRARY_FAULT,rc);
  rc=k7_aec_diagnostic_reset(ctx);if(rc)return rc;
  ctx->continuity_token=(struct k7_aec_continuity_token){.serial=serial,
    .identity=*id,.mic_first_sample=mic_first,.reference_first_sample=ref_first,
    .phase=K7_AEC_CONTINUITY_BEFORE_CANCEL};
  ctx->continuity_mic_next=mic_first;ctx->continuity_reference_next=ref_first;
  ctx->continuity_frames=0;ctx->continuity_active=1;
  *out=ctx->continuity_token;return 0;
}
int k7_aec_diagnostic_continuity_process(struct k7_aec *ctx,
  const struct k7_aec_continuity_token *token,const struct k7_aec_continuity_frame *f,
  int16_t out[320])
{
  int16_t residual[320];uint64_t warnings;
  if(!ctx)return -EINVAL;
  /* Old handles and foreign streams cannot damage the current adaptation. */
  if(!continuity_token_same(token,&ctx->continuity_token))return -ESTALE;
  if(!ctx->continuity_active || !ctx->diagnostic_active)return -EPIPE;
  if(f && f->identity.owner_epoch!=token->identity.owner_epoch)return -ESTALE;
  /* With this current token and owner, a changed clock/input/reference lineage
   * is a discontinuity, not an unrelated stale request. It cannot be retried
   * under the previous identity after skipping a real change. */
  if(f && !continuity_identity_same(&f->identity,&token->identity))
    return invalidate(ctx,K7_AEC_BAD_REFERENCE,-EPIPE);
  if(!f || !out || !f->mic || !f->reference || f->samples!=320)
    return invalidate(ctx,K7_AEC_BAD_REFERENCE,-EINVAL);
  if(f->mic_first_sample!=ctx->continuity_mic_next ||
     f->reference_first_sample!=ctx->continuity_reference_next ||
     f->mic_first_sample>UINT64_MAX-320u || f->reference_first_sample>UINT64_MAX-320u ||
     ctx->continuity_frames==UINT64_MAX)
    return invalidate(ctx,K7_AEC_GAP,-EPIPE);
  if(enter(ctx))return -EBUSY;
  if(setjmp(ctx->unwind)){
    leave();return invalidate(ctx,K7_AEC_LIBRARY_FAULT,ctx->fault);
  }
  warnings=ctx->stats.warnings;
  speex_echo_cancellation(ctx->state,f->mic,f->reference,residual);
  if(warnings!=ctx->stats.warnings){
    leave();return invalidate(ctx,K7_AEC_LIBRARY_FAULT,-EIO);
  }
  ctx->continuity_mic_next+=320u;ctx->continuity_reference_next+=320u;
  ctx->continuity_frames++;
  /* stats.ready/frames are exclusively the verified legacy path. */
  ctx->stats.ready=0;
  memcpy(out,residual,sizeof(residual));leave();return 0;
}
int k7_aec_diagnostic_cancel_seal(struct k7_aec *ctx,
  const struct k7_aec_continuity_token *token,uint64_t mic_next,
  uint64_t ref_next,struct k7_aec_continuity_token *out)
{
  uint64_t serial=0;int rc;
  if(!ctx)return -EINVAL;
  if(!continuity_token_same(token,&ctx->continuity_token))return -ESTALE;
  if(!ctx->continuity_active || !ctx->diagnostic_active)return -EPIPE;
  if(token->phase!=K7_AEC_CONTINUITY_BEFORE_CANCEL)return -EALREADY;
  if(!out || !ctx->continuity_frames || mic_next!=ctx->continuity_mic_next ||
     ref_next!=ctx->continuity_reference_next || mic_next>UINT64_MAX-320u ||
     ref_next>UINT64_MAX-320u)return invalidate(ctx,K7_AEC_GAP,-EINVAL);
  rc=continuity_serial(&serial);
  if(rc)return invalidate(ctx,K7_AEC_LIBRARY_FAULT,rc);
  /* NO Speex call, reset, new reference, mute assertion or allocation here. */
  ctx->continuity_token.serial=serial;
  ctx->continuity_token.phase=K7_AEC_CONTINUITY_AFTER_CANCEL;
  ctx->continuity_token.mic_first_sample=mic_next;
  ctx->continuity_token.reference_first_sample=ref_next;
  ctx->stats.ready=0;
  *out=ctx->continuity_token;return 0;
}
int k7_aec_diagnostic_continuity_status(struct k7_aec *ctx,
  struct k7_aec_continuity_status *out)
{
  if(!ctx || !out)return -EINVAL;
  *out=(struct k7_aec_continuity_status){.token=ctx->continuity_token,
    .mic_next_sample=ctx->continuity_mic_next,
    .reference_next_sample=ctx->continuity_reference_next,
    .frames=ctx->continuity_frames,.active=ctx->continuity_active!=0,.qualified=0};
  return 0;
}

#endif
