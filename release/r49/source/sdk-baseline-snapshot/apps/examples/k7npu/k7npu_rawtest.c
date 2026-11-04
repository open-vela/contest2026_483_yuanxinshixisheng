/* SPDX-License-Identifier: Apache-2.0 */
/* Native single-core, one-shot RK3576 bring-up. The separately licensed
 * experimental fixture is linked only with CONFIG_K7NPU_RAW_TEST.
 * PC register facts/init sequence: pinned Rockchip rknpu_job.c/rknpu_drv.c.
 * Never releases static buffers after uncertain DMA; requires RAM reboot.
 */
#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "rk3576_npu_mmu.h"
#include "rk3576_npu_window.h"
#include "k7npu_fixture_io.h"

static uint32_t g_raw_dt[1024] __attribute__((aligned(4096)));
static uint32_t g_raw_pt[1024] __attribute__((aligned(4096)));
static unsigned char g_raw_data[K7NPU_RAW_BYTES] __attribute__((aligned(4096)));
extern volatile int g_k7npu_mem_owner;

static uint32_t rd(uintptr_t address)
{ return *(volatile const uint32_t *)address; }
static void barrier(void)
{ __asm__ __volatile__("dsb sy" ::: "memory"); }
static void wr(uintptr_t address, uint32_t value)
{
  *(volatile uint32_t *)address = value;
  barrier();
}
static void delay(void)
{
  const struct timespec gap = {0, 100000};
  nanosleep(&gap, NULL);
}
static uint64_t microseconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
static int in_ram(const void *ptr, size_t bytes)
{
  uint64_t start = (uintptr_t)ptr;
  uint64_t end = (uint64_t)CONFIG_RAM_START + CONFIG_RAM_SIZE;
  return !(start & 4095) && start >= CONFIG_RAM_START &&
         start + bytes <= end && start + bytes <= UINT64_C(0x100000000);
}

struct raw_trial
{
  uint32_t raw;
  uint32_t tasks;
  uint32_t mmu[4];
  uint64_t elapsed;
  uint32_t before_clear;
  uint32_t after_clear;
  int submitted;
  int complete;
  int initialize_cbuf;
};

static int execute(void *arg)
{
  struct raw_trial *trial = arg;
  const uintptr_t base = 0x27700000;
  static const uintptr_t mmus[4] = {0x27702000,0x27702100,0x2770a000,0x2770a100};
  unsigned int retry, i;
  uint64_t start;

  /* The vendor driver acknowledges latched status before a new submission.
   * Entry has already checked idle cores and MMUs. A pending event alone is
   * not evidence of a running DMA job. Require DPU-done cleared so an old
   * completion cannot make this trial pass.
   */
  trial->before_clear = rd(base + 0x2c);
  wr(base + 0x24, 0x1ffff);
  trial->after_clear = rd(base + 0x2c);
  if (trial->after_clear & 0x300) return -EIO;

  /* Vendor commit enters slave mode before CPU pointer programming. CBUF
   * initialization is a cold-session operation, not a per-task reset.
   */
  wr(base + 0x10, 1);
  if (trial->initialize_cbuf)
    {
      wr(base + 0x1004, 0);
      wr(base + 0x1024, 0x80000000);
      wr(base + 0x1004, 1);
      wr(base + 0x1024, 0x80000000);
      wr(base + 0x1004, 0x1e);
    }
  /* rknpu_job_subcore_commit_pc writes these CPU-side on EVERY core0
   * submission before the command address. The first fixture words do not
   * replace this submission-time sequence.
   */
  wr(base + 0x1004, 0xe);
  wr(base + 0x3004, 0xe);

  wr(base + 0x10, 0x10000000); /* commands in the isolated IOVA window */
  wr(base + 0x14, (K7NPU_RAW_WORDS + 4 + 1) / 2 - 1);
  wr(base + 0x20, 0x300); /* poll DPU completion; GIC IRQ remains disabled */
  wr(base + 0x24, 0x300);
  wr(base + 0x30, 0x00070001); /* RK3576 16-bit task count, NOT RK3588 */
  wr(base + 0x34, 0); /* one task, no descriptor chain */
  start = microseconds();
  trial->submitted = 1;
  wr(base + 8, 1);
  wr(base + 8, 0);
  for (retry = 0; retry < 1000; retry++)
    {
      trial->raw = rd(base + 0x2c);
      trial->tasks = rd(base + 0x48);
      for (i = 0; i < 4; i++)
        {
          trial->mmu[i] = rd(mmus[i] + 4);
          if ((trial->mmu[i] & 2) || rd(mmus[i] + 0x14)) goto failed;
        }
      /* Vendor ISR completes on the masked DPU event, not the diagnostic
       * task counter. Actual K7 raw-command mode reports DPU done with count
       * zero. The event was cleared before this one task; MMU idle cleanup
       * and all 64 numeric outputs must still be verified independently.
       */
      if (trial->raw & 0x300)
        {
          trial->complete = 1;
          trial->elapsed = microseconds() - start;
          wr(base + 0x24, 0x1ffff);
          wr(base + 0x20, 0);
          barrier();
          return 0;
        }
      delay();
    }
failed:
  trial->elapsed = microseconds() - start;
  /* Do not detach mappings or recycle memory while completion is uncertain. */
  wr(base + 0x20, 0);
  return -ETIMEDOUT;
}

static int raw_matrix(void (*emit)(const char *), unsigned int variant)
{
  const struct rk3576_npu_io io = {rd, wr, delay};
  struct rk3576_npu_mmu_result mmu = {0};
  struct raw_trial trial = {0};
  char line[160];
  unsigned int i;
  int ret, bad = -1;
  trial.initialize_cbuf = variant == 0;
  for (i = 0; i < 2; i++)
    {
      uintptr_t base = 0x27700000 + i * 0x8000;
      uint32_t id = rd(base), version = rd(base + 4), op = rd(base + 8);
      uint32_t masked = rd(base + 0x28), raw = rd(base + 0x2c), tasks = rd(base + 0x48);
      snprintf(line, sizeof(line), "RAW PRE%u id=%08" PRIx32 " ver=%08" PRIx32
               " op=%08" PRIx32 " masked=%08" PRIx32 " raw=%08" PRIx32 " tasks=%08" PRIx32 "\n",
               i, id, version, op, masked, raw, tasks);
      emit(line);
      /* Actual K7 reset snapshot has raw=0x30000000 on both idle cores.
       * Upper raw bits are not the DPU completion mask used by the vendor
       * submission path. Do not reinterpret them as PC/DMA busy. OP, masked
       * status, task counter and all MMU idle/fault checks remain mandatory.
       */
      if (id != 0x46495245 || version != 0x10002 || op || masked || tasks)
        { emit("BLOCKED: core is not in the fresh idle state.\n"); return 1; }
    }
  if (!in_ram(g_raw_dt, sizeof(g_raw_dt)) || !in_ram(g_raw_pt, sizeof(g_raw_pt)) ||
      !in_ram(g_raw_data, sizeof(g_raw_data)))
    { emit("BLOCKED: raw buffers outside flat mapped DRAM.\n"); return 1; }
  if (k7npu_fixture_prepare(g_raw_data, sizeof(g_raw_data)) ||
      k7npu_fixture_variant(g_raw_data, variant) ||
      rk3576_npu_make_window(g_raw_dt, g_raw_pt, (uintptr_t)g_raw_pt, 0x10000000,
                             (uintptr_t)g_raw_data, sizeof(g_raw_data)))
    { emit("BLOCKED: fixture or address validation failed.\n"); return 1; }
  up_clean_dcache((uintptr_t)g_raw_dt, (uintptr_t)g_raw_dt + sizeof(g_raw_dt));
  up_clean_dcache((uintptr_t)g_raw_pt, (uintptr_t)g_raw_pt + sizeof(g_raw_pt));
  up_clean_dcache((uintptr_t)g_raw_data, (uintptr_t)g_raw_data + sizeof(g_raw_data));
  barrier();
  snprintf(line, sizeof(line), "RAWTEST M1 K64 N64 INT8->INT32 DT=%08" PRIxPTR
           " DATA=%08" PRIxPTR " bytes=%zu words=%u\n",
           (uintptr_t)g_raw_dt, (uintptr_t)g_raw_data, sizeof(g_raw_data), K7NPU_RAW_WORDS);
  emit(line);
  ret = rk3576_npu_mmu_run(&io, (uintptr_t)g_raw_dt, execute, &trial, &mmu);
  snprintf(line, sizeof(line), "RAW ACK before=%08" PRIx32 " after=%08" PRIx32 " submitted=%d\n",
           trial.before_clear, trial.after_clear, trial.submitted);
  emit(line);
  if (trial.complete)
    {
      up_invalidate_dcache((uintptr_t)g_raw_data + K7NPU_RAW_OUTPUT,
                          (uintptr_t)g_raw_data + K7NPU_RAW_OUTPUT + 8192);
      barrier();
      bad = k7npu_fixture_compare_variant(g_raw_data, variant);
    }
  snprintf(line, sizeof(line), "RAWTEST complete=%d raw=%08" PRIx32
           " tasks=%08" PRIx32 " elapsed_us=%" PRIu64 " mismatches=%d/64\n",
           trial.complete, trial.raw, trial.tasks, trial.elapsed, bad);
  emit(line);
  for (i = 0; i < 4; i++)
    {
      snprintf(line, sizeof(line), "RAW MMU%u during=%08" PRIx32 " after=%08" PRIx32 "\n",
               i, trial.mmu[i], mmu.after[i]);
      emit(line);
    }
  snprintf(line, sizeof(line), "RAW POINTERS cna=%08" PRIx32 " core=%08" PRIx32
           " dpu=%08" PRIx32 " rdma=%08" PRIx32 "\n",
           rd(0x27701004), rd(0x27703004), rd(0x27704004), rd(0x27705004));
  emit(line);
  snprintf(line, sizeof(line), "RAWTEST cleanup=%d retained=%x\n", ret, mmu.retained);
  emit(line);
  emit(ret == 0 && bad == 0 ? "PASS: one NPU matrix matched 64 CPU outputs. Not model inference.\n" :
                             "FAIL: raw matrix not verified; preserve logs and RAM reboot before retry.\n");
  return ret == 0 && bad == 0 ? 0 : 1;
}

static int raw_session(void (*emit)(const char *), unsigned int rounds)
{
  unsigned int i;
  char line[96];
  if (__sync_lock_test_and_set(&g_k7npu_mem_owner, 1))
    { emit("BLOCKED: memory test busy or raw session already used; RAM reboot required.\n"); return 1; }
  /* The lock remains owned until reboot. Within this bounded session, reuse
   * is allowed ONLY when the previous output comparison and all four MMU
   * cleanup checks succeeded. Any failure stops before modifying buffers.
   */
  for (i = 0; i < rounds; i++)
    {
      snprintf(line, sizeof(line), "RAW ROUND %u/%u variant=%u\n", i + 1, rounds, i);
      emit(line);
      if (raw_matrix(emit, i)) return 1;
    }
  snprintf(line, sizeof(line), "RAW SESSION PASS rounds=%u outputs=%u; model=0\n", rounds, rounds * 64);
  emit(line);
  return 0;
}

int k7npu_rawtest(void (*emit)(const char *))
{
  return raw_session(emit, 1);
}

int k7npu_rawrepeat(void (*emit)(const char *))
{
  return raw_session(emit, K7NPU_RAW_VARIANTS);
}
