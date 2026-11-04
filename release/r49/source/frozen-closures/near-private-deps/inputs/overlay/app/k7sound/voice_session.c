/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "voice_session.h"
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
#include "k7_apm21_idle_events.h"
#endif
#line 3
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
#include "k7_apm21_prepared_lifecycle.h"
#endif
#line 3
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
#include "k7_apm21_live_wiring.h"
#endif
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
#include "session_capture.h"
#endif
#ifdef CONFIG_SMP
#include <sched.h>
#endif
struct k7_voice_session {
  struct k7_voice_session_config config;
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
  struct k7_apm21_idle_events idle_events;
#endif
  struct k7_fifo_reference *fifo_reference;
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
  struct k7_apm21_pio_tap *apm21_tap; /* fixed lifetime, separate SPSC drain */
#endif
  struct k7_voice_ring kws, asr, render;
  struct k7_voice_frame microphone_frame;
  struct k7_voice_packer render_packer;
  int16_t tx_pcm[K7_VOICE_TX_SAMPLES];
  pthread_mutex_t control;
  pthread_t worker;
  bool started, joined, joining; /* protected by control */
  k7_voice_session_run run;
  void *run_arg;
  atomic_bool stopping, running, ready, finished;
  atomic_int result;
  atomic_bool cleanup_ok;
  atomic_int cleanup_error;
  atomic_uint tx_state, generation, produced, consumed, cancel_generation;
  atomic_uint tx_prebuffer, tx_rebuffer, tx_pause_ms, tx_pauses;
  atomic_bool eof;
  atomic_uint input_epoch, asr_epoch, failed_asr_epoch;
  atomic_uint_fast64_t microphone_samples;
  atomic_uint completion_sequence;
  atomic_uint completed_generation, completed_queued, completed_discarded, completed_pauses;
  atomic_int completed_result;
  struct k7_voice_anchor anchors[K7_VOICE_ANCHOR_SLOTS];
  atomic_uint anchor_produced, anchor_consumed, anchor_epoch, anchor_overflows, anchor_faults;
  atomic_bool anchor_exhausted;
  uint32_t anchor_consumer_epoch; /* dedicated consumer only */
  uint64_t anchor_last_us, anchor_last_rx; /* PIO producer only */
  uint32_t anchor_last_generation, anchor_last_queued;
  bool anchor_seen, anchor_gap;
};
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
struct k7_apm21_pio_tap *k7_voice_session_apm21_tap(struct k7_voice_session *s)
{return s?s->apm21_tap:NULL;}
#endif
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
struct k7_apm21_idle_events *k7_voice_session_apm21_idle_events(struct k7_voice_session *s)
{return s&&s->config.timing_anchors?&s->idle_events:NULL;}
#if defined(K7_APM21_JOINT_CONTINUATION) && K7_APM21_JOINT_CONTINUATION
void k7_voice_session_apm21_idle_disabled(struct k7_voice_session *s,uint32_t generation,uint64_t rx_next,uint64_t before,uint64_t after,int read_rc,uint32_t xfer){
 if(!s||!s->config.timing_anchors)return;
 struct k7_voice_anchor a={.event=K7_VOICE_ANCHOR_TX_STOP,.generation=generation,.rx_samples=rx_next,.before_us=before,.after_us=after,.epoch=atomic_load_explicit(&s->anchor_epoch,memory_order_relaxed)};
 k7_apm21_idle_disabled(&s->idle_events,&a,atomic_load_explicit(&s->input_epoch,memory_order_relaxed),read_rc,xfer);
}
#endif
void k7_voice_session_apm21_idle_quiet(struct k7_voice_session *s,uint32_t generation,uint64_t rx_next,uint64_t before,uint64_t after,unsigned checks,uint32_t xfer){
 if(!s||!s->config.timing_anchors)return;
 struct k7_voice_anchor a={.generation=generation,.rx_samples=rx_next,.before_us=before,.after_us=after,
  .epoch=atomic_load_explicit(&s->anchor_epoch,memory_order_relaxed)};
 k7_apm21_idle_quiet(&s->idle_events,&a,atomic_load_explicit(&s->input_epoch,memory_order_relaxed),checks,xfer);
}
#endif
size_t k7_voice_session_storage_bytes(void) { return sizeof(struct k7_voice_session); }
static int lock(struct k7_voice_session *s)
{ if (!s) return -EINVAL; int rc = pthread_mutex_lock(&s->control); return rc ? -rc : 0; }
static int unlock(struct k7_voice_session *s, int rc)
{ int err = pthread_mutex_unlock(&s->control); return rc ? rc : err ? -err : 0; }
static uint32_t epoch_after(uint32_t epoch) { return epoch == UINT32_MAX ? 1 : epoch + 1; }
int k7_voice_session_create(const struct k7_voice_session_config *c, struct k7_voice_session **out)
{
  if (!out) return -EINVAL;
  *out = NULL;
  if (!c || c->tx_prebuffer_samples < 8 || c->tx_prebuffer_samples > K7_VOICE_TX_SAMPLES) return -EINVAL;
  struct k7_voice_session *s = calloc(1, sizeof(*s)); if (!s) return -ENOMEM;
  s->config = *c;
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
  k7_apm21_idle_init(&s->idle_events);
#endif
  int rc = pthread_mutex_init(&s->control, NULL); if (rc) { free(s); return -rc; }
  atomic_init(&s->stopping, false); atomic_init(&s->running, false);
  atomic_init(&s->ready, false); atomic_init(&s->finished, false); atomic_init(&s->result, 0);
  atomic_init(&s->cleanup_ok, false); atomic_init(&s->cleanup_error, 0);
  atomic_init(&s->tx_state, K7_VOICE_TX_IDLE); atomic_init(&s->generation, 0);
  atomic_init(&s->produced, 0); atomic_init(&s->consumed, 0); atomic_init(&s->cancel_generation, 0);
  atomic_init(&s->tx_prebuffer, c->tx_prebuffer_samples); atomic_init(&s->tx_rebuffer, 0);
  atomic_init(&s->tx_pause_ms, 0); atomic_init(&s->tx_pauses, 0);
  atomic_init(&s->eof, false); atomic_init(&s->input_epoch, 1);
  atomic_init(&s->asr_epoch, 0); atomic_init(&s->failed_asr_epoch, 0);
  atomic_init(&s->microphone_samples, 0); atomic_init(&s->completion_sequence, 0);
  atomic_init(&s->completed_generation, 0); atomic_init(&s->completed_queued, 0);
  atomic_init(&s->completed_discarded, 0); atomic_init(&s->completed_result, 0);
  atomic_init(&s->completed_pauses, 0);
  atomic_init(&s->anchor_produced, 0); atomic_init(&s->anchor_consumed, 0);
  atomic_init(&s->anchor_epoch, 1); atomic_init(&s->anchor_overflows, 0);
  atomic_init(&s->anchor_faults, 0); atomic_init(&s->anchor_exhausted, false);
  s->anchor_consumer_epoch = 1;
  k7_voice_ring_init(&s->kws); k7_voice_ring_init(&s->asr); k7_voice_ring_init(&s->render);
  if(c->fifo_reference){
    rc=k7_fifo_reference_create(&s->fifo_reference);
    if(rc){pthread_mutex_destroy(&s->control);free(s);return rc;}
  }
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
  if(c->fifo_reference){
    rc=k7_apm21_tap_create(&s->apm21_tap);
    if(rc){k7_fifo_reference_destroy(s->fifo_reference);pthread_mutex_destroy(&s->control);free(s);return rc;}
  }
#endif
  *out = s; return 0;
}
static void *worker(void *arg)
{
  struct k7_voice_session *s = arg;
  int rc = s->run(s->run_arg, s);
  /* Hardware ownership has ended; serialize final publication with a short
   * producer-side begin/write. No control lock is taken by the live pump. */
  pthread_mutex_lock(&s->control);
  k7_voice_session_ready(s, false);
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  k7_session_capture_owner_exit(s,rc);
#endif
  if (atomic_load(&s->tx_state) != K7_VOICE_TX_IDLE)
    k7_voice_session_tx_complete(s, rc ? rc : -ECANCELED, 0, 0);
  atomic_store(&s->result, rc);
  atomic_store_explicit(&s->running, false, memory_order_release);
  atomic_store_explicit(&s->finished, true, memory_order_release);
  pthread_mutex_unlock(&s->control);
  return NULL;
}
int k7_voice_session_start(struct k7_voice_session *s, k7_voice_session_run run, void *arg)
{
  if (!run) return -EINVAL;
  int rc = lock(s); if (rc) return rc;
  if (s->started || atomic_load(&s->stopping)) return unlock(s, -EALREADY);
  pthread_attr_t attr; int err = pthread_attr_init(&attr); if (err) return unlock(s, -err);
  err = pthread_attr_setstacksize(&attr, 32768);
#ifdef CONFIG_SMP
  if (!err) { cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(1, &cpus);
    err = pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus); }
#endif
  if (!err) {
    s->run = run; s->run_arg = arg; atomic_store(&s->running, true);
    err = pthread_create(&s->worker, &attr, worker, s);
    if (!err) s->started = true; else atomic_store(&s->running, false);
  }
  pthread_attr_destroy(&attr); return unlock(s, err ? -err : 0);
}
void k7_voice_session_stop(struct k7_voice_session *s)
{ if (s) atomic_store_explicit(&s->stopping, true, memory_order_release); }
int k7_voice_session_stop_requested(const struct k7_voice_session *s)
{ return !s || atomic_load_explicit(&s->stopping, memory_order_acquire); }
void k7_voice_session_ready(struct k7_voice_session *s, bool ready)
{
  if(!s)return;
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  k7_session_capture_owner_ready(s,ready);
#endif
  if(!ready)k7_fifo_reference_revoke(s->fifo_reference,K7_FIFO_REF_STOP);
  atomic_store_explicit(&s->ready, ready, memory_order_release);
}
void k7_voice_session_cleanup_complete(struct k7_voice_session *s, int error)
{
  if (!s) return;
  atomic_store_explicit(&s->cleanup_error, error, memory_order_relaxed);
  atomic_store_explicit(&s->cleanup_ok, error == 0, memory_order_release);
}
#if defined(K7_APM21_PREPARED_LIFECYCLE) && K7_APM21_PREPARED_LIFECYCLE
/* join()'s numeric return conflates run result and pthread/lock failures. This
 * private read proves actual joined state, not a guessed errno comparison. */
int k7_voice_session_apm21_retirement_check(struct k7_voice_session *s)
{
 int rc=lock(s);if(rc)return rc;
 if(!s->started||!s->joined||s->joining)return unlock(s,-EBUSY);
 int cleanup=atomic_load_explicit(&s->cleanup_error,memory_order_acquire);
 if(!atomic_load_explicit(&s->finished,memory_order_acquire)||
    !atomic_load_explicit(&s->cleanup_ok,memory_order_acquire)||cleanup)
  return unlock(s,cleanup?cleanup:-EIO);
 return unlock(s,0);
}
#endif
int k7_voice_session_join(struct k7_voice_session *s)
#line 161
{
  int rc = lock(s); if (rc) return rc;
  if (!s->started) return unlock(s, -EINVAL);
  if (!s->joined) {
    if (s->joining) return unlock(s, -EBUSY);
    s->joining = true;
    rc = unlock(s, 0); if (rc) return rc;
    int err = pthread_join(s->worker, NULL);
    rc = lock(s); if (rc) return rc;
    s->joining = false;
    if (err) return unlock(s, -err); /* retain object while ownership is uncertain */
    s->joined = true;
  }
  return unlock(s, atomic_load(&s->result));
}
int k7_voice_session_destroy(struct k7_voice_session *s)
{
  int rc = lock(s); if (rc) return rc;
  if (s->started && !s->joined) return unlock(s, -EBUSY);
  rc = unlock(s, 0); if (rc) return rc;
  int err = pthread_mutex_destroy(&s->control); if (err) return -err;
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
  k7_apm21_tap_destroy(s->apm21_tap);
#endif
  k7_fifo_reference_destroy(s->fifo_reference);
  volatile unsigned char *p = (volatile unsigned char *)s;
  for (size_t i = 0; i < sizeof(*s); ++i) p[i] = 0;
  free(s); return 0;
}
void k7_voice_session_status(const struct k7_voice_session *s, struct k7_voice_session_status *out)
{
  if (!s || !out) return;
  *out = (struct k7_voice_session_status){
    .running = atomic_load(&s->running), .ready = atomic_load(&s->ready),
    .stop_requested = atomic_load(&s->stopping), .finished = atomic_load(&s->finished),
    .result = atomic_load(&s->result), .microphone_samples = atomic_load(&s->microphone_samples),
    .input_epoch = atomic_load(&s->input_epoch), .asr_epoch = atomic_load(&s->asr_epoch),
    .asr_failed_epoch = atomic_load(&s->failed_asr_epoch),
    .kws_overflows = k7_voice_ring_overflows(&s->kws), .asr_overflows = k7_voice_ring_overflows(&s->asr),
    .render_overflows = k7_voice_ring_overflows(&s->render),
    .anchor_overflows = atomic_load(&s->anchor_overflows), .anchor_faults = atomic_load(&s->anchor_faults),
    .anchor_epoch = atomic_load(&s->anchor_epoch),
    .tx_state = (enum k7_voice_tx_state)atomic_load(&s->tx_state),
    .tx_generation = atomic_load(&s->generation), .tx_pauses = atomic_load(&s->tx_pauses),
    .cleanup_ok = atomic_load_explicit(&s->cleanup_ok, memory_order_acquire),
    .cleanup_error = atomic_load(&s->cleanup_error)
  };
}
static int tx_begin(struct k7_voice_session *s, uint32_t generation,
                     unsigned prebuffer, unsigned rebuffer, unsigned pause_ms)
{
  if (!generation) return -EINVAL;
  int rc = lock(s); if (rc) return rc;
  if (!atomic_load(&s->ready) || !atomic_load(&s->running) || atomic_load(&s->stopping))
    return unlock(s, -ENOTCONN);
  if (generation <= atomic_load(&s->generation)) return unlock(s, -EALREADY);
  unsigned expected = K7_VOICE_TX_IDLE;
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  if(atomic_load(&s->tx_state)==K7_VOICE_TX_IDLE)k7_session_capture_tx_busy(s,true);
#endif
  if (!atomic_compare_exchange_strong(&s->tx_state, &expected, K7_VOICE_TX_RESETTING)) return unlock(s, -EBUSY);
  atomic_store(&s->produced, 0); atomic_store(&s->consumed, 0);
  atomic_store(&s->cancel_generation, 0); atomic_store(&s->eof, false);
  atomic_store(&s->tx_prebuffer, prebuffer); atomic_store(&s->tx_rebuffer, rebuffer);
  atomic_store(&s->tx_pause_ms, pause_ms); atomic_store(&s->tx_pauses, 0);
  atomic_store(&s->generation, generation);
  (void)k7_voice_packer_init(&s->render_packer, &s->render, generation, K7_VOICE_FRAME_RENDER, 0);
  atomic_store_explicit(&s->tx_state, K7_VOICE_TX_PREFILL, memory_order_release);
  return unlock(s, 0);
}
int k7_voice_session_tx_begin(struct k7_voice_session *s, uint32_t generation)
{ return tx_begin(s, generation, s ? s->config.tx_prebuffer_samples : 0, 0, 0); }
static int tx_begin_stream_bounded(struct k7_voice_session *s, uint32_t generation,
                                   unsigned prebuffer, unsigned rebuffer,
                                   unsigned pause_ms, unsigned max_pause_ms)
{
  if (prebuffer < 8 || prebuffer > K7_VOICE_TX_SAMPLES || rebuffer < 8 ||
      rebuffer > K7_VOICE_TX_SAMPLES || !pause_ms || pause_ms > max_pause_ms) return -EINVAL;
  return tx_begin(s, generation, prebuffer, rebuffer, pause_ms);
}
int k7_voice_session_tx_begin_stream(struct k7_voice_session *s, uint32_t generation,
                                    unsigned prebuffer, unsigned rebuffer, unsigned pause_ms)
{ return tx_begin_stream_bounded(s, generation, prebuffer, rebuffer, pause_ms, 5000); }
int k7_voice_session_tx_begin_verified_stream(struct k7_voice_session *s, uint32_t generation,
                                             unsigned prebuffer, unsigned rebuffer, unsigned pause_ms)
{ return tx_begin_stream_bounded(s, generation, prebuffer, rebuffer, pause_ms, 20000); }
static int generation_open(struct k7_voice_session *s, uint32_t generation)
{
  if (!generation || atomic_load(&s->generation) != generation) return K7_VOICE_SESSION_STALE;
  if (!atomic_load(&s->running) || atomic_load(&s->stopping)) return -ECANCELED;
  unsigned state = atomic_load_explicit(&s->tx_state, memory_order_acquire);
  if (state != K7_VOICE_TX_PREFILL && state != K7_VOICE_TX_RUNNING &&
      state != K7_VOICE_TX_DRAINING && state != K7_VOICE_TX_PAUSED) return -EPIPE;
  if (atomic_load(&s->cancel_generation) == generation) return -ECANCELED;
  return 0;
}
int k7_voice_session_tx_write(struct k7_voice_session *s, uint32_t generation,
                              const int16_t *pcm, unsigned count)
{
  if (!pcm || !count || count > K7_VOICE_TX_SAMPLES) return -EINVAL;
  int rc = lock(s); if (rc) return rc;
  rc = generation_open(s, generation); if (rc) return unlock(s, rc);
  if (atomic_load(&s->eof)) return unlock(s, -EPIPE);
  uint32_t p = atomic_load_explicit(&s->produced, memory_order_relaxed);
  uint32_t c = atomic_load_explicit(&s->consumed, memory_order_acquire);
  if (p - c > K7_VOICE_TX_SAMPLES) return unlock(s, -EIO);
  if (count > K7_VOICE_TX_SAMPLES - (p - c)) return unlock(s, -EAGAIN);
  /* A single utterance is bounded before uint32 counter wrap; start another
   * generation for another reply. 74h of PCM is not an ordinary utterance. */
  if (count > UINT32_MAX - p) return unlock(s, -EFBIG);
  for (unsigned i = 0; i < count; ++i) s->tx_pcm[(p + i) % K7_VOICE_TX_SAMPLES] = pcm[i];
  atomic_store_explicit(&s->produced, p + count, memory_order_release);
  return unlock(s, 0);
}
int k7_voice_session_tx_end(struct k7_voice_session *s, uint32_t generation)
{
  int rc = lock(s); if (rc) return rc;
  rc = generation_open(s, generation);
  if (!rc) atomic_store_explicit(&s->eof, true, memory_order_release);
  return unlock(s, rc);
}
int k7_voice_session_tx_cancel(struct k7_voice_session *s, uint32_t generation)
{
  int rc = lock(s); if (rc) return rc;
  rc = generation_open(s, generation);
  if (!rc) atomic_store_explicit(&s->cancel_generation, generation, memory_order_release);
  return unlock(s, rc);
}
enum k7_voice_tx_action k7_voice_session_tx_action(const struct k7_voice_session *s)
{
  if (!s) return K7_VOICE_TX_WAIT;
  unsigned state = atomic_load_explicit(&s->tx_state, memory_order_acquire);
  if (state != K7_VOICE_TX_PREFILL && state != K7_VOICE_TX_RUNNING &&
      state != K7_VOICE_TX_DRAINING && state != K7_VOICE_TX_PAUSED) return K7_VOICE_TX_WAIT;
  if (atomic_load(&s->cancel_generation) == atomic_load(&s->generation)) return K7_VOICE_TX_CANCEL;
  if (state == K7_VOICE_TX_RUNNING || state == K7_VOICE_TX_DRAINING) return K7_VOICE_TX_CONTINUE;
  uint32_t p = atomic_load_explicit(&s->produced, memory_order_acquire);
  uint32_t c = atomic_load_explicit(&s->consumed, memory_order_relaxed);
  unsigned minimum = atomic_load(state == K7_VOICE_TX_PAUSED ? &s->tx_rebuffer : &s->tx_prebuffer);
  return p - c >= minimum || atomic_load_explicit(&s->eof, memory_order_acquire) ?
    K7_VOICE_TX_START : K7_VOICE_TX_WAIT;
}
int k7_voice_session_tx_started(struct k7_voice_session *s)
{
  if (!s) return -EINVAL;
  unsigned expected = atomic_load_explicit(&s->tx_state, memory_order_acquire);
  if (expected != K7_VOICE_TX_PREFILL && expected != K7_VOICE_TX_PAUSED) return -EPROTO;
  return atomic_compare_exchange_strong(&s->tx_state, &expected, K7_VOICE_TX_RUNNING) ? 0 : -EPROTO;
}
int k7_voice_session_tx_draining(struct k7_voice_session *s)
{
  if (!s) return -EINVAL;
  if (!atomic_load(&s->tx_pause_ms)) return -EAGAIN;
  /* Short network gaps are counted, not a reason to truncate a valid answer.
   * The caller retains its total-turn budget; PIO retains the per-gap timeout. */
  if (atomic_load(&s->tx_pauses) == UINT_MAX) return -EOVERFLOW;
  unsigned expected = K7_VOICE_TX_RUNNING;
  return atomic_compare_exchange_strong(&s->tx_state, &expected, K7_VOICE_TX_DRAINING) ? 0 : -EPROTO;
}
int k7_voice_session_tx_paused(struct k7_voice_session *s)
{
  if (!s) return -EINVAL;
  unsigned expected = K7_VOICE_TX_DRAINING;
  if (!atomic_compare_exchange_strong(&s->tx_state, &expected, K7_VOICE_TX_PAUSED)) return -EPROTO;
  atomic_fetch_add(&s->tx_pauses, 1); return 0;
}
unsigned k7_voice_session_tx_pause_timeout_ms(const struct k7_voice_session *s)
{ return s ? atomic_load(&s->tx_pause_ms) : 0; }
int k7_voice_session_tx_pull(struct k7_voice_session *s, int16_t *sample, uint64_t *index)
{
  if (!s || !sample || !index) return -EINVAL;
  if (atomic_load(&s->cancel_generation) == atomic_load(&s->generation)) return -ECANCELED;
  uint32_t c = atomic_load_explicit(&s->consumed, memory_order_relaxed);
  uint32_t p = atomic_load_explicit(&s->produced, memory_order_acquire);
  if (c == p) {
    bool eof = atomic_load_explicit(&s->eof, memory_order_acquire);
    p = atomic_load_explicit(&s->produced, memory_order_acquire);
    if (c == p) return eof ? 0 : -EAGAIN;
  }
  *sample = s->tx_pcm[c % K7_VOICE_TX_SAMPLES]; *index = c;
  atomic_store_explicit(&s->consumed, c + 1, memory_order_release);
  return 1;
}
int k7_voice_session_render(struct k7_voice_session *s, int16_t sample, uint64_t index)
{
  if (!s) return -EINVAL;
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  k7_session_capture_fifo_source(s,sample,index);
#endif
  return s->config.render_reference ? k7_voice_packer_sample(&s->render_packer, sample, index) : 0;
}
void k7_voice_session_tx_complete(struct k7_voice_session *s, int rc, unsigned queued, unsigned discarded)
{
  if (!s) return;
  if (s->config.render_reference) {
    int tail = k7_voice_packer_flush(&s->render_packer); if (!rc && tail) rc = tail;
  }
  atomic_fetch_add(&s->completion_sequence, 1); /* odd: completion being published */
  atomic_store(&s->completed_generation, atomic_load(&s->generation));
  atomic_store(&s->completed_queued, queued); atomic_store(&s->completed_discarded, discarded);
  atomic_store(&s->completed_result, rc);
  atomic_store(&s->completed_pauses, atomic_load(&s->tx_pauses));
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  k7_session_capture_tx_busy(s,false);
#endif
  atomic_store_explicit(&s->tx_state, K7_VOICE_TX_IDLE, memory_order_release);
  /* A visible completion must also permit the next begin. */
  atomic_fetch_add(&s->completion_sequence, 1);
}
int k7_voice_session_tx_result(const struct k7_voice_session *s, uint32_t generation,
                               struct k7_voice_tx_result *out)
{
  if (!s || !generation || !out) return -EINVAL;
  for (unsigned i = 0; i < 8; ++i) {
    unsigned a = atomic_load(&s->completion_sequence); if (a & 1u) continue;
    struct k7_voice_tx_result r = {.generation = atomic_load(&s->completed_generation),
      .result = atomic_load(&s->completed_result), .queued_samples = atomic_load(&s->completed_queued),
      .discarded_words = atomic_load(&s->completed_discarded), .pauses = atomic_load(&s->completed_pauses)};
    unsigned b = atomic_load(&s->completion_sequence);
    if (a != b) continue;
    if (r.generation == generation) { *out = r; return 0; }
    return atomic_load(&s->generation) == generation ? -EAGAIN : K7_VOICE_SESSION_STALE;
  }
  return -EAGAIN;
}
int k7_voice_session_asr_epoch(struct k7_voice_session *s, uint32_t epoch)
{
  if (!s) return -EINVAL;
  if (epoch && epoch == atomic_load(&s->failed_asr_epoch)) return -EALREADY;
  atomic_store_explicit(&s->asr_epoch, epoch, memory_order_release); return 0;
}
uint32_t k7_voice_session_input_epoch(const struct k7_voice_session *s)
{ return s ? atomic_load_explicit(&s->input_epoch, memory_order_acquire) : 0; }
int k7_voice_session_microphone(struct k7_voice_session *s, int16_t value, uint64_t index)
{
  if (!s || index != atomic_load_explicit(&s->microphone_samples, memory_order_relaxed) || index == UINT64_MAX)
    return -EILSEQ;
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  k7_session_capture_microphone(s,value,index);
#endif
#if defined(K7_APM21_LIVE_OBSERVER) && K7_APM21_LIVE_OBSERVER
  k7_apm21_tap_microphone(s->apm21_tap,value,index);
#endif
  struct k7_voice_frame *f = &s->microphone_frame;
  if (!f->samples) f->first_sample = index;
  f->pcm[f->samples++] = value;
  atomic_store_explicit(&s->microphone_samples, index + 1, memory_order_release);
  if (f->samples == K7_VOICE_FRAME_SAMPLES) {
    f->epoch = atomic_load(&s->input_epoch);
    int rc = k7_voice_ring_put(&s->kws, f);
    if (rc == -ENOSPC) atomic_store_explicit(&s->input_epoch, epoch_after(f->epoch), memory_order_release);
    else if (rc) return rc;
    uint32_t asr = atomic_load_explicit(&s->asr_epoch, memory_order_acquire);
    if (asr && asr != atomic_load(&s->failed_asr_epoch)) {
      f->epoch = asr; rc = k7_voice_ring_put(&s->asr, f);
      if (rc == -ENOSPC) atomic_store_explicit(&s->failed_asr_epoch, asr, memory_order_release);
      else if (rc) return rc;
    }
    f->samples = 0;
  }
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
  if(s->config.timing_anchors)k7_apm21_idle_microphone(&s->idle_events,value,index,
    atomic_load_explicit(&s->input_epoch,memory_order_relaxed),
    atomic_load_explicit(&s->anchor_epoch,memory_order_relaxed));
#endif
  return 0;
}
int k7_voice_session_take_kws(struct k7_voice_session *s, struct k7_voice_frame *out)
{
  if (!s || !out) return -EINVAL;
  bool stale = false;
  for (unsigned i = 0; i < K7_VOICE_RING_SLOTS; ++i) {
    int rc = k7_voice_ring_take(&s->kws, out); if (rc <= 0) return stale ? -EPIPE : rc;
    if (out->epoch == atomic_load_explicit(&s->input_epoch, memory_order_acquire)) return 1;
    stale = true;
  }
  return -EPIPE;
}
int k7_voice_session_take_asr(struct k7_voice_session *s, uint32_t epoch, struct k7_voice_frame *out)
{
  if (!s || !epoch || !out) return -EINVAL;
  if (atomic_load(&s->asr_epoch) != epoch) return K7_VOICE_SESSION_STALE;
  bool failed = atomic_load_explicit(&s->failed_asr_epoch, memory_order_acquire) == epoch;
  for (unsigned i = 0; i < K7_VOICE_RING_SLOTS; ++i) {
    int rc = k7_voice_ring_take(&s->asr, out); if (rc <= 0) return failed ? -ENOSPC : rc;
    if (!failed && out->epoch == epoch)
      return atomic_load_explicit(&s->failed_asr_epoch, memory_order_acquire) == epoch ? -ENOSPC : 1;
  }
  return failed ? -ENOSPC : -EPIPE;
}
int k7_voice_session_take_render(struct k7_voice_session *s, struct k7_voice_frame *out)
{ return s ? k7_voice_ring_take(&s->render, out) : -EINVAL; }
bool k7_voice_session_anchors_enabled(const struct k7_voice_session *s)
{ return s && s->config.timing_anchors; }
uint32_t k7_voice_session_tx_generation(const struct k7_voice_session *s)
{ return s ? atomic_load_explicit(&s->generation, memory_order_acquire) : 0; }
uint32_t k7_voice_session_anchor_epoch(const struct k7_voice_session *s)
{ return s ? atomic_load_explicit(&s->anchor_epoch, memory_order_acquire) : 0; }
static void anchor_invalidate(struct k7_voice_session *s)
{
  uint32_t e = atomic_load_explicit(&s->anchor_epoch, memory_order_relaxed);
  if (e == UINT32_MAX) atomic_store_explicit(&s->anchor_exhausted, true, memory_order_release);
  else atomic_store_explicit(&s->anchor_epoch, e + 1, memory_order_release);
  s->anchor_gap = true;
}
static void anchor_increment(atomic_uint *value)
{
  unsigned n = atomic_load_explicit(value, memory_order_relaxed);
  if (n != UINT32_MAX) atomic_store_explicit(value, n + 1, memory_order_relaxed);
}
int k7_voice_session_record_anchor(struct k7_voice_session *s, const struct k7_voice_anchor *in)
{
  if (!s || !in) return -EINVAL;
  if (!s->config.timing_anchors) return 0;
  if (atomic_load_explicit(&s->anchor_exhausted, memory_order_relaxed)) return -EOVERFLOW;
  struct k7_voice_anchor a = *in;
  /* Samples remain contiguous within a TX generation across a refill pause,
   * but the acoustic timeline does not. Invalidate mapping without changing
   * RX input_epoch, and preserve explicit pause/resume diagnostic records. */
  if (a.event == K7_VOICE_ANCHOR_TX_PAUSE || a.event == K7_VOICE_ANCHOR_TX_RESUME)
    anchor_invalidate(s);
  if (a.after_us < a.before_us || (s->anchor_seen && a.before_us < s->anchor_last_us))
    a.flags |= K7_VOICE_ANCHOR_CLOCK_INVALID;
  if ((s->anchor_seen && a.rx_samples < s->anchor_last_rx) ||
      (a.generation && (a.generation < s->anchor_last_generation ||
       (a.generation == s->anchor_last_generation && a.tx_queued_samples < s->anchor_last_queued))))
    a.flags |= K7_VOICE_ANCHOR_COUNT_INVALID;
  s->anchor_seen = true; s->anchor_last_us = a.after_us; s->anchor_last_rx = a.rx_samples;
  if (a.generation) { s->anchor_last_generation = a.generation; s->anchor_last_queued = a.tx_queued_samples; }
  if (a.flags & (K7_VOICE_ANCHOR_IO_ERROR | K7_VOICE_ANCHOR_CLOCK_INVALID | K7_VOICE_ANCHOR_COUNT_INVALID)) {
    anchor_increment(&s->anchor_faults); anchor_invalidate(s);
  }
  uint32_t p = atomic_load_explicit(&s->anchor_produced, memory_order_relaxed);
  uint32_t c = atomic_load_explicit(&s->anchor_consumed, memory_order_acquire);
  if (p - c >= K7_VOICE_ANCHOR_SLOTS) {
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
    k7_apm21_idle_fault(&s->idle_events,-ENOSPC);
#endif
    anchor_increment(&s->anchor_overflows); anchor_invalidate(s); return -ENOSPC;
  }
  a.epoch = atomic_load_explicit(&s->anchor_epoch, memory_order_relaxed);
  if (s->anchor_gap) a.flags |= K7_VOICE_ANCHOR_GAP;
#if defined(K7_APM21_IDLE_EVENTS) && K7_APM21_IDLE_EVENTS
  k7_apm21_idle_anchor(&s->idle_events,&a,atomic_load_explicit(&s->input_epoch,memory_order_relaxed));
#endif
  s->anchors[p % K7_VOICE_ANCHOR_SLOTS] = a;
  atomic_store_explicit(&s->anchor_produced, p + 1, memory_order_release);
  s->anchor_gap = false; return 0;
}
int k7_voice_session_take_anchor(struct k7_voice_session *s, struct k7_voice_anchor *out)
{
  if (!s || !out) return -EINVAL;
  if (!s->config.timing_anchors) return 0;
  if (atomic_load_explicit(&s->anchor_exhausted, memory_order_acquire)) return -EOVERFLOW;
  uint32_t e = atomic_load_explicit(&s->anchor_epoch, memory_order_acquire);
  uint32_t c = atomic_load_explicit(&s->anchor_consumed, memory_order_relaxed);
  uint32_t p = atomic_load_explicit(&s->anchor_produced, memory_order_acquire);
  if (s->anchor_consumer_epoch != e) {
    /* Discard only records predating this epoch; keep a new fault/gap record
     * for the next call so diagnostics can show why continuity was lost. */
    while (c != p && s->anchors[c % K7_VOICE_ANCHOR_SLOTS].epoch != e) c++;
    atomic_store_explicit(&s->anchor_consumed, c, memory_order_release);
    s->anchor_consumer_epoch = e; return -EPIPE;
  }
  if (c == p) return 0;
  *out = s->anchors[c % K7_VOICE_ANCHOR_SLOTS];
  atomic_store_explicit(&s->anchor_consumed, c + 1, memory_order_release);
  if (out->epoch != e || e != atomic_load_explicit(&s->anchor_epoch, memory_order_acquire)) return -EPIPE;
  return 1;
}

int k7_voice_session_take_fifo_reference(struct k7_voice_session *s,struct k7_fifo_ref_record *r)
{ return !s||!r?-EINVAL:s->fifo_reference?k7_fifo_reference_take(s->fifo_reference,r):0; }
int k7_voice_session_take_fifo_archive(struct k7_voice_session *s,struct k7_fifo_ref_record *r)
{ return !s||!r?-EINVAL:s->fifo_reference?k7_fifo_reference_take_archive(s->fifo_reference,r):0; }
int k7_voice_session_fifo_status(const struct k7_voice_session *s,struct k7_fifo_ref_status *out)
{ return !s||!out?-EINVAL:s->fifo_reference?k7_fifo_reference_status(s->fifo_reference,out):-ENOTSUP; }
bool k7_voice_session_fifo_matches(const struct k7_voice_session *s,uint64_t i,uint32_t e,uint32_t g)
{ return s&&k7_fifo_reference_matches(s->fifo_reference,i,e,g); }
bool k7_voice_session_fifo_enabled(const struct k7_voice_session *s)
{ return s&&s->fifo_reference; }
void k7_voice_session_fifo_begin(struct k7_voice_session *s,uint32_t g)
{
 if(!s)return;
 if(g!=k7_voice_session_tx_generation(s)){k7_fifo_reference_revoke(s->fifo_reference,K7_FIFO_REF_COUNT);return;}
 k7_fifo_reference_begin(s->fifo_reference,g);
}
void k7_voice_session_fifo_pair(struct k7_voice_session *s,uint32_t g,int16_t v,bool z,uint64_t i,uint64_t b,uint64_t a)
{ if(s)k7_fifo_reference_pair(s->fifo_reference,g,v,z,i,b,a); }
void k7_voice_session_fifo_revoke(struct k7_voice_session *s,unsigned f)
{ if(s)k7_fifo_reference_revoke(s->fifo_reference,f); }
void k7_voice_session_fifo_end(struct k7_voice_session *s,int r,unsigned d,bool empty)
{ if(s)k7_fifo_reference_end(s->fifo_reference,r,d,empty); }
