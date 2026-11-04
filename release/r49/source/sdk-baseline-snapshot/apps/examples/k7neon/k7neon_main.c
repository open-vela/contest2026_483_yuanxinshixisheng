/* Candidate only: APIs follow app/k7load, no SDK/build/hardware claims. */
#include <nuttx/config.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "probe.h"

static atomic_int once, go, stop;
static uint64_t work_end;
static struct worker_state {
  unsigned id, cpu, batches, errors, switches;
  atomic_uint epoch;
  atomic_int done;
} state[4];

static uint64_t now_ns(void)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 ||
      t.tv_nsec < 0 || t.tv_nsec >= 1000000000) return 0;
  return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static int checkpoint(void *arg)
{
  struct worker_state *s = arg;
  uint64_t t = now_ns();
  if (!t) { s->errors++; atomic_store(&stop, 1); return 1; }
  if (atomic_load(&stop) || t >= work_end) return 1;
  if (sched_getcpu() != (int)s->cpu) {
    s->errors++; atomic_store(&stop, 1); return 1;
  }
  unsigned before = atomic_load(&state[s->id ^ 1].epoch);
  atomic_fetch_add(&s->epoch, 1);
  if (sched_yield() != 0 || sched_getcpu() != (int)s->cpu) {
    s->errors++; atomic_store(&stop, 1); return 1;
  }
  if (atomic_load(&state[s->id ^ 1].epoch) != before) s->switches++;
  return 0;
}

static void *worker(void *arg)
{
  struct worker_state *s = arg;
  uint32_t input[16], output[16];
  probe_input(s->id, input);
  /* Coordinator owns deadline, published by release/acquire go. */
  for (unsigned wait = 0; !atomic_load(&go) && !atomic_load(&stop); ++wait)
    if (wait >= 1000 || usleep(1000) != 0) {
      s->errors++; atomic_store(&stop, 1); break;
    }
  for (unsigned batch = 0; !atomic_load(&stop) && batch < 100000; ++batch) {
    uint64_t t = now_ns();
    if (!t) { s->errors++; atomic_store(&stop, 1); break; }
    if (t >= work_end) break;
    unsigned steps = neon_probe_hold(input, output, checkpoint, s);
    s->batches++;
    if (!probe_match(s->id, steps, output)) {
      s->errors++; atomic_store(&stop, 1); break;
    }
  }
  atomic_store(&s->done, 1);
  return NULL;
}

int k7neon_main(int argc, char **argv)
{
  (void)argv;
  if (argc != 1 || atomic_exchange(&once, 1)) return 1;
  uint64_t start = now_ns();
  if (!start) return 1;
  unsigned created = 0;
  int rc = 0;
  work_end = start + UINT64_C(5000000000);
  for (unsigned i = 0; i < 4; ++i) {
    pthread_attr_t attr;
    pthread_t thread;
    cpu_set_t set;
    state[i].id = i;
    state[i].cpu = 4 + i / 2;
    CPU_ZERO(&set);
    CPU_SET(state[i].cpu, &set);
    rc = pthread_attr_init(&attr);
    if (rc) break;
    rc = pthread_attr_setstacksize(&attr, 8192);
    if (!rc) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!rc) rc = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    if (!rc) {
      rc = pthread_create(&thread, &attr, worker, &state[i]);
      if (!rc) ++created;
    }
    int destroy_rc = pthread_attr_destroy(&attr);
    if (!rc) rc = destroy_rc;
    if (rc) break;
  }
  if (rc || created != 4) atomic_store(&stop, 1);
  atomic_store(&go, 1);
  /* No unbounded join/semaphore wait. Detached threads have only static
   * references. Never reset/reuse state, including partial creation failure. */
  unsigned complete = 0;
  for (unsigned poll = 0; poll < 8000; ++poll) {
    complete = 0;
    for (unsigned i = 0; i < created; ++i) complete += !!atomic_load(&state[i].done);
    if (complete == created) break;
    uint64_t t = now_ns();
    if (!t || t < start || t - start >= UINT64_C(8000000000)) break;
    if (usleep(1000) != 0) { rc = 1; atomic_store(&stop, 1); }
  }
  atomic_store(&stop, 1);
  if (complete != created) {
    printf("NEON INCOMPLETE created=%u done=%u; state retained, no retry\n", created, complete);
    return 1; /* Do not access unfinished workers' non-atomic fields. */
  }
  for (unsigned i = 0; i < created; ++i) {
    printf("NEON id=%u cpu=%u batches=%u peer_progress=%u errors=%u\n",
           i, state[i].cpu, state[i].batches, state[i].switches, state[i].errors);
    if (!state[i].batches || !state[i].switches || state[i].errors) rc = 1;
  }
  if (created != 4) rc = 1;
  printf("NEON result=%s scope=d8-d15-low64 cpus=4,5\n", rc ? "FAIL" : "PASS");
  return !!rc;
}
