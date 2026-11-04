/* Explicit one-shot diagnostic; does not initialize peripherals. */
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

static atomic_int started;
static atomic_int stop;
static atomic_uint counter;
static sem_t done;
static struct result
{
  int requested;
  int observed;
  unsigned samples;
  unsigned mismatches;
  unsigned sleep_errors;
  uint64_t elapsed_ns;
  uint64_t mpidr;
  uint64_t midr;
  unsigned gic_target;
} results[CONFIG_SMP_NCPUS];

static uint64_t now_ns(void)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
    return 0;
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void *worker(void *arg)
{
  struct result *r = arg;
  uint64_t begin = now_ns();
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (r->mpidr));
  __asm__ volatile ("mrs %0, midr_el1" : "=r" (r->midr));
  r->mpidr &= UINT64_C(0xff00ffffff);
  r->gic_target = *(volatile uint32_t *)(uintptr_t)0x2a701800 & 255;
  for (unsigned i = 0; i < 10000 && !atomic_load(&stop); i++)
    {
      r->observed = sched_getcpu();
      r->mismatches += r->observed != r->requested;
      r->samples++;
      atomic_fetch_add(&counter, 1);
      if (i % 1000 == 0 && usleep(1000) != 0)
        r->sleep_errors++;
    }
  r->elapsed_ns = now_ns() - begin;
  sem_post(&done);
  return NULL;
}

int main(int argc, char **argv)
{
  pthread_t thread;
  pthread_attr_t attr;
  struct timespec deadline;
  cpu_set_t set;
  int rc = 0;
  (void)argc;
  (void)argv;
  /* Static state survives a failed/timeout invocation. No detached worker
   * can access freed memory; a second invocation requires a new RAM boot. */
  if (atomic_exchange(&started, 1))
    {
      printf("SMP diagnostic already attempted; no blind retry\n");
      return 1;
    }
  if (sem_init(&done, 0, 0) != 0)
    return 1;
  for (unsigned cpu = 0; cpu < CONFIG_SMP_NCPUS; cpu++)
    {
      results[cpu].requested = cpu;
      CPU_ZERO(&set);
      CPU_SET(cpu, &set);
      rc = pthread_attr_init(&attr);
      if (rc != 0)
        break;
      rc = pthread_attr_setstacksize(&attr, 8192);
      if (rc == 0)
        rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      if (rc == 0)
        rc = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
      if (rc == 0)
        rc = pthread_create(&thread, &attr, worker, &results[cpu]);
      pthread_attr_destroy(&attr);
      if (rc != 0)
        break;
    }
  if (rc == 0 && clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    rc = errno;
  if (rc == 0)
    {
      deadline.tv_sec += 2;
      for (unsigned i = 0; i < CONFIG_SMP_NCPUS; i++)
        {
          do { rc = sem_timedwait(&done, &deadline); }
          while (rc != 0 && errno == EINTR);
          if (rc != 0)
            break;
        }
    }
  if (rc != 0)
    {
      atomic_store(&stop, 1);
      printf("SMP diagnostic failed rc=%d; state retained\n", rc);
      return 1;
    }
  for (unsigned i = 0; i < CONFIG_SMP_NCPUS; i++)
    {
      struct result *r = &results[i];
      printf("SMP cpu=%d observed=%d samples=%u mismatch=%u sleep_errors=%u elapsed_ns=%llu\n",
             r->requested, r->observed, r->samples, r->mismatches,
             r->sleep_errors, (unsigned long long)r->elapsed_ns);
      printf("SMP topology cpu=%u mpidr=%llx midr=%llx gic_target=%02x\n",
             i, (unsigned long long)r->mpidr, (unsigned long long)r->midr, r->gic_target);
      if (r->mpidr != ((uint64_t)(i / 4) << 8 | i % 4) ||
          r->gic_target != (1u << i))
        rc = 1;
      if (r->samples != 10000 || r->mismatches || r->sleep_errors ||
          r->elapsed_ns < 1000000)
        rc = 1;
    }
  if (atomic_load(&counter) != 10000 * CONFIG_SMP_NCPUS)
    rc = 1;
  printf("SMP result=%s shared_counter=%u expected=%u\n",
         rc ? "FAIL" : "PASS", atomic_load(&counter), 10000u * CONFIG_SMP_NCPUS);
  return rc;
}
