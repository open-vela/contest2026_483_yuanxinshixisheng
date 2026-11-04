/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "rk3576_npu_mmu.h"
#include "rk3576_npu_window.h"

/* BSS-owned for the lifetime of this RAM boot; never freed on timeout. */
static uint32_t g_dt[1024] __attribute__((aligned(4096)));
static uint32_t g_pt[1024] __attribute__((aligned(4096)));
static unsigned char g_data[4096] __attribute__((aligned(4096)));
volatile int g_k7npu_mem_owner;
static int g_poisoned;

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

static int in_ram(const void *p)
{
  uintptr_t a = (uintptr_t)p;
  uint64_t end = (uint64_t)CONFIG_RAM_START + CONFIG_RAM_SIZE;
  return !(a & 4095) && a >= CONFIG_RAM_START &&
         (uint64_t)a + 4096 <= end && (uint64_t)a + 4096 <= UINT64_C(0x100000000);
}

int k7npu_mmutest(void (*emit)(const char *))
{
  const struct rk3576_npu_io io = {rd, wr, delay};
  struct rk3576_npu_mmu_result result;
  char line[128];
  unsigned int i;
  int ret = 1;
  if (__sync_lock_test_and_set(&g_k7npu_mem_owner, 1))
    { emit("BLOCKED: NPU memory test already running.\n"); return 1; }
  if (g_poisoned)
    { emit("BLOCKED: previous MMU cleanup failed; RAM reboot required.\n"); goto done; }
  for (i = 0; i < 2; i++)
    {
      uintptr_t base = 0x27700000 + i * 0x8000;
      if (rd(base) != 0x46495245 || rd(base + 4) != 0x00010002 ||
          rd(base + 0x28) != 0 || rd(base + 0x48) != 0)
        { emit("BLOCKED: expected idle K7 core signature not present.\n"); goto done; }
    }
  /* The configured flat DRAM mapping is identity. This explicit check is
   * required before using a CPU buffer address as a physical address.
   */
  if (!in_ram(g_dt) || !in_ram(g_pt) || !in_ram(g_data))
    { emit("BLOCKED: test buffers outside mapped low-32-bit DRAM.\n"); goto done; }
  ret = rk3576_npu_make_window(g_dt, g_pt, (uintptr_t)g_pt, 0x10000000,
                               (uintptr_t)g_data, sizeof(g_data));
  if (ret) goto done;
  memset(g_data, 0xa5, sizeof(g_data));
  up_clean_dcache((uintptr_t)g_dt, (uintptr_t)g_dt + sizeof(g_dt));
  up_clean_dcache((uintptr_t)g_pt, (uintptr_t)g_pt + sizeof(g_pt));
  up_clean_dcache((uintptr_t)g_data, (uintptr_t)g_data + sizeof(g_data));
  barrier();
  snprintf(line, sizeof(line), "MMUTEST DT=%08" PRIxPTR " PT=%08" PRIxPTR
           " DATA=%08" PRIxPTR " IOVA=10000000 size=4096\n",
           (uintptr_t)g_dt, (uintptr_t)g_pt, (uintptr_t)g_data);
  emit(line);
  ret = rk3576_npu_mmu_cycle(&io, (uint32_t)(uintptr_t)g_dt, &result);
  for (i = 0; i < 4; i++)
    {
      snprintf(line, sizeof(line), "MMU%u before=%08" PRIx32
               " enabled=%08" PRIx32 " after=%08" PRIx32 "\n",
               i, result.before[i], result.enabled[i], result.after[i]);
      emit(line);
    }
  g_poisoned = result.retained != 0;
  snprintf(line, sizeof(line), "MMUTEST result=%d touched=%x retained=%x\n",
           ret, result.touched, result.retained);
  emit(line);
  emit(ret == 0 ? "PASS: paging cycle restored. No task or DMA/page walk tested.\n" :
                 "FAIL: MMU cycle incomplete. No NPU task submitted.\n");
done:
  __sync_lock_release(&g_k7npu_mem_owner);
  return ret ? 1 : 0;
}
