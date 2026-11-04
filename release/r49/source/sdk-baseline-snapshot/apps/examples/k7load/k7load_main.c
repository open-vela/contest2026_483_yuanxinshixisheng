/* Explicit 60-second experiment, no automatic startup or peripheral access.
 * CPU0 retains services; CPU4..7 run 2ms compute bursts and 2ms sleeps.
 * This is a synthetic integer workload, not an ASR/NPU performance result. */
#include <nuttx/config.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "workload.h"

static atomic_int started;
static atomic_int stop;
static atomic_uint total;
static sem_t done;
static struct result
{
  unsigned cpu;
  unsigned batches;
  unsigned mismatch;
  unsigned bad_result;
  unsigned sleep_errors;
  unsigned clock_errors;
  uint64_t elapsed_ns;
  uint64_t burst_ns;
} results[4];

static uint64_t now_ns(void)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
    return 0;
  return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static void *worker(void *arg)
{
  struct result *r = arg;
  uint64_t start = now_ns();
  uint64_t current = start;
  unsigned rounds = 0;
  if (!start)
    r->clock_errors++;
  while (!r->clock_errors && !atomic_load(&stop) &&
         current - start < UINT64_C(60000000000) && rounds++ < 1000000)
    {
      uint64_t burst = current;
      unsigned bounded = 0;
      do
        {
          r->mismatch += sched_getcpu() != (int)r->cpu;
          r->bad_result += k7load_batch() != K7LOAD_EXPECTED;
          r->batches++;
          atomic_fetch_add(&total, 1);
          uint64_t next = now_ns();
          if (!next || next < current)
            { r->clock_errors++; break; }
          current = next;
        }
      while (!atomic_load(&stop) && current - burst < UINT64_C(2000000) && ++bounded < 4096);
      r->burst_ns += current - burst;
      if (usleep(2000) != 0)
        r->sleep_errors++;
      uint64_t next = now_ns();
      if (!next || next < current)
        r->clock_errors++;
      else
        current = next;
    }
  r->elapsed_ns = current - start;
  sem_post(&done);
  return NULL;
}

int main(int argc, char **argv)
{
  (void)argv;
  if (argc != 1 || atomic_exchange(&started, 1))
    { printf("LOAD refused: once per RAM boot, no arguments\n"); return 1; }
  if (sem_init(&done, 0, 0) != 0)
    return 1;
  printf("LOAD start cpus=4,5,6,7 seconds=60 burst_us=2000 sleep_us=2000\n");
  fflush(stdout);
  int rc = 0;
  for (unsigned i = 0; i < 4; i++)
    {
      pthread_attr_t attr;
      pthread_t thread;
      cpu_set_t set;
      results[i].cpu = i + 4;
      CPU_ZERO(&set);
      CPU_SET(i + 4, &set);
      rc = pthread_attr_init(&attr);
      if (rc != 0) break;
      rc = pthread_attr_setstacksize(&attr, 8192);
      if (!rc) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      if (!rc) rc = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
      if (!rc) rc = pthread_create(&thread, &attr, worker, &results[i]);
      pthread_attr_destroy(&attr);
      if (rc != 0) break;
    }
  struct timespec deadline;
  if (!rc && clock_gettime(CLOCK_REALTIME, &deadline) != 0) rc = errno;
  if (!rc)
    {
      deadline.tv_sec += 63;
      for (unsigned i = 0; i < 4; i++)
        {
          do { rc = sem_timedwait(&done, &deadline); }
          while (rc != 0 && errno == EINTR);
          if (rc != 0) break;
        }
    }
  if (rc)
    {
      atomic_store(&stop, 1);
      printf("LOAD failed rc=%d state retained; no retry\n", rc);
      return 1;
    }
  unsigned sum = 0;
  for (unsigned i = 0; i < 4; i++)
    {
      struct result *r = &results[i];
      printf("LOAD cpu=%u batches=%u mismatch=%u bad_result=%u sleep_errors=%u clock_errors=%u elapsed_ns=%llu burst_ns=%llu\n",
             r->cpu, r->batches, r->mismatch, r->bad_result, r->sleep_errors,
             r->clock_errors, (unsigned long long)r->elapsed_ns,
             (unsigned long long)r->burst_ns);
      sum += r->batches;
      if (r->mismatch || r->bad_result || r->sleep_errors || r->clock_errors ||
          r->elapsed_ns < UINT64_C(60000000000) || !r->batches)
        rc = 1;
    }
  if (sum != atomic_load(&total)) rc = 1;
  printf("LOAD result=%s batches=%u\n", rc ? "FAIL" : "PASS", sum);
  return rc;
}
