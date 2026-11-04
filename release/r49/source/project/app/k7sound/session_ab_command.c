/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "session_ab_probe.h"
#include "duplex_session.h"
#include "../k7agent/cloud/include/vv_native_service.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "session_ab_pcm.inc"
#define CAPTURE_SAMPLES (16000u * 16u)
#define DUMP_MAX 4096u
static pthread_mutex_t ab_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool active, stop;
static struct k7_voice_session *retained_owner;
static int16_t *capture;
static struct k7_ab_result latest;
static unsigned codec_value[3], codec_valid;
static int codec_error;
bool k7_session_ab_active(void) { return atomic_load(&active); }
void k7_session_ab_codec(unsigned index, unsigned value, int rc)
{
  if (index >= 3) return;
  if (rc) { if (!codec_error) codec_error = rc; }
  else { codec_value[index] = value; codec_valid |= 1u << index; }
}
static uint64_t now_ms(void *unused)
{
  (void)unused; struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000 ||
      (uint64_t)t.tv_sec > (UINT64_MAX - 999u) / 1000u) return UINT64_MAX;
  return (uint64_t)t.tv_sec * 1000u + (unsigned long)t.tv_nsec / 1000000u;
}
static void yield_ms(void *unused)
{ (void)unused; struct timespec t = {0,1000000}; nanosleep(&t, NULL); }
static bool cancelled(void *unused) { (void)unused; return atomic_load(&stop); }
static int number(const char *text, unsigned *out)
{
  if (!text || !text[0] || text[0] == '-' || text[0] == '+') return -EINVAL;
  char *end; errno = 0; unsigned long v = strtoul(text, &end, 10);
  if (errno || *end || v > CAPTURE_SAMPLES) return -EINVAL;
  *out = (unsigned)v; return 0;
}
static void summary(int rc, const struct k7_voice_session_status *status)
{
  printf("AB_SOURCE samples=%u crc32=%08" PRIx32 " sha256=%s rate=16000 channels=1\n",
         K7_AB_PCM_SAMPLES, latest.source_crc, K7_AB_PCM_SHA);
  for (unsigned i = 0; i < 2; i++) {
    const struct k7_ab_pass *p = &latest.pass[i];
    printf("AB_PASS name=%c submitted=%u rendered=%u crc32=%08" PRIx32 " queued=%u discarded_words=%u pauses=%u tx_result=%d start_rx=%" PRIu64 " end_rx=%" PRIu64 "\n",
           'A' + i,p->submitted,p->rendered,p->crc,p->queued,p->discarded,p->pauses,p->tx_result,p->start_rx,p->end_rx);
  }
  for (unsigned i = 0; i < latest.events; i++) {
    const struct k7_voice_anchor *a = &latest.event[i];
    printf("AB_EVENT generation=%u event=%u us=%" PRIu64 " rx=%" PRIu64 " queued=%u fifo=%u discarded=%u epoch=%u flags=%u result=%d\n",
           a->generation,a->event,a->after_us,a->rx_samples,a->tx_queued_samples,a->tx_fifo_words,a->discarded_words,a->epoch,a->flags,a->result);
  }
  printf("AB_CODEC before_rx=1 valid=%u reg19=%02x reg1c=%02x reg2b=%02x rc=%d\n",
         codec_valid,codec_value[0],codec_value[1],codec_value[2],codec_error);
  printf("AB_RESULT result=%d capture_complete=%u truncated=%u rx_samples=%u rx_first=%" PRIu64 " rx_crc32=%08" PRIx32 " rx_gaps=%u event_gaps=%u cleanup_ok=%u cleanup_error=%d input_epoch=%u\n",
         rc,latest.capture_complete,latest.capture_truncated,latest.rx_samples,latest.rx_first,latest.rx_crc,latest.rx_gaps,latest.event_gaps,
         status->cleanup_ok,status->cleanup_error,status->input_epoch);
}
static int run(void)
{
  struct vv_native_service_status native;
  /* stopping is sticky after the native worker has exited; running remains
   * true until that worker releases its resources. Physical owner acquisition
   * below is still mandatory and a failed claim is never diagnostic success. */
  if (vv_native_service_status(&native) || native.running) return -EBUSY;
  pthread_mutex_lock(&ab_lock);
  if (atomic_load(&active) || retained_owner) { pthread_mutex_unlock(&ab_lock); return -EBUSY; }
  atomic_store(&active, true); atomic_store(&stop, false);
  free(capture); capture = NULL; memset(&latest, 0, sizeof(latest));
  memset(codec_value, 0, sizeof(codec_value)); codec_valid = 0; codec_error = 0;
  capture = malloc(CAPTURE_SAMPLES * sizeof(*capture));
  if (!capture) { atomic_store(&active, false); pthread_mutex_unlock(&ab_lock); return -ENOMEM; }
  pthread_mutex_unlock(&ab_lock);
  int rc = k7_session_ab_crc(k7_ab_pcm, sizeof(k7_ab_pcm)) == K7_AB_PCM_CRC ? 0 : -EILSEQ;
  struct k7_voice_session *s = NULL;
  const struct k7_voice_session_config cfg = {.tx_prebuffer_samples=8192,.render_reference=true,.timing_anchors=true};
  if (!rc) { rc = k7sound_session_start(&cfg, &s); if (rc) s = NULL; }
  struct k7_voice_session_status status = {0};
  if (!rc) {
    uint64_t began = now_ms(NULL);
    for (;;) {
      k7_voice_session_status(s, &status);
      if (status.finished) { rc = status.result ? status.result : -EIO; break; }
      if (cancelled(NULL)) { rc = -ECANCELED; break; }
      if (status.ready) break;
      uint64_t now = now_ms(NULL);
      if (began == UINT64_MAX || now == UINT64_MAX || now < began || now - began >= 5000) { rc = -ETIMEDOUT; break; }
      yield_ms(NULL);
    }
  }
  if (!rc) {
    const struct k7_ab_config test = {.pcm=k7_ab_pcm,.samples=K7_AB_PCM_SAMPLES,
      .cut={19200,38400},.prebuffer=8192,.rebuffer=16000,.gap_ms=700,.timeout_ms=14000};
    const struct k7_ab_io io = {NULL,now_ms,yield_ms,cancelled};
    rc = k7_session_ab_run(s,&test,&io,capture,CAPTURE_SAMPLES,&latest);
  }
  if (s) {
    k7_voice_session_stop(s); int joined = k7_voice_session_join(s);
    k7_voice_session_status(s,&status);
    if (joined && joined != -ECANCELED && !rc) rc = joined;
    if (!status.finished || !status.cleanup_ok || status.cleanup_error) {
      if (!rc) rc = status.cleanup_error ? status.cleanup_error : -EIO;
      retained_owner = s; /* unverified cleanup must never admit another owner */
    } else {
      int destroyed = k7_voice_session_destroy(s);
      if (destroyed) { retained_owner = s; if (!rc) rc = destroyed; }
    }
  }
  if (!rc && (codec_valid != 7 || codec_error)) rc = codec_error ? codec_error : -EIO;
  latest.result = rc;
  if (rc) latest.capture_complete = false;
  summary(rc,&status); fflush(stdout);
  pthread_mutex_lock(&ab_lock); atomic_store(&active,false); pthread_mutex_unlock(&ab_lock);
  return rc;
}
int k7_session_ab_command(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1],"session-ab-stop")) { atomic_store(&stop,true); return 0; }
  if (argc == 2 && !strcmp(argv[1],"session-ab")) {
    int rc=run(); if (rc) printf("AB refused_or_failed=%d\n",rc); return rc ? 1 : 0;
  }
  pthread_mutex_lock(&ab_lock);
  if (atomic_load(&active)) { pthread_mutex_unlock(&ab_lock); return 1; }
  if (argc == 2 && !strcmp(argv[1],"session-ab-clear")) {
    free(capture); capture=NULL; memset(&latest,0,sizeof(latest)); pthread_mutex_unlock(&ab_lock); return 0;
  }
  unsigned offset=0,count=0;
  if (argc != 4 || strcmp(argv[1],"session-ab-dump") || number(argv[2],&offset) || number(argv[3],&count) ||
      !count || count>DUMP_MAX || !capture || offset>latest.rx_samples || count>latest.rx_samples-offset) {
    pthread_mutex_unlock(&ab_lock); return 1;
  }
  /* Post-join only. Each invocation is bounded; no UART work during RX/TX. */
  for (unsigned at=0; at<count; at+=64) {
    unsigned n=count-at;if(n>64)n=64;
    printf("AB_RX offset=%u samples=%u data=",offset+at,n);
    for(unsigned i=0;i<n;i++) { uint16_t v=(uint16_t)capture[offset+at+i]; printf("%02x%02x",v&255,v>>8); }
    putchar('\n');
  }
  fflush(stdout); pthread_mutex_unlock(&ab_lock); return 0;
}
