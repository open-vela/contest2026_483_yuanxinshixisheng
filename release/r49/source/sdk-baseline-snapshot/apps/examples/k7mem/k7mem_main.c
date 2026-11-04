/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <nuttx/mm/k7_model_arena.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static mutex_t g_test_lock = NXMUTEX_INITIALIZER;

int main(int argc, char **argv)
{
  if (argc != 2 || (strcmp(argv[1], "status") && strcmp(argv[1], "test")))
    { puts("usage: k7mem status|test (test: 384MiB allocation, sparse page patterns)"); return 1; }
  if (!strcmp(argv[1], "status"))
    {
      printf("MODEL DDR base=%lx bytes=%lu free=%zu initialized=%u cpu_only=1\n",
        (unsigned long)K7_MODEL_ARENA_BASE, (unsigned long)K7_MODEL_ARENA_SIZE,
        k7_model_available(), k7_model_available() != 0);
      return 0;
    }
  if (nxmutex_trylock(&g_test_lock) < 0) { puts("MODEL DDR test busy"); return 1; }
  int ret = k7_model_arena_initialize();
  const size_t bytes = 384ul * 1024 * 1024;
  size_t before = k7_model_available();
  unsigned char *memory = ret == 0 ? k7_model_alloc(bytes) : NULL;
  if (!memory) { puts("MODEL DDR allocation failed"); nxmutex_unlock(&g_test_lock); return 1; }
  uintptr_t address = (uintptr_t)memory;
  if (address < K7_MODEL_ARENA_BASE || address > K7_MODEL_ARENA_BASE + K7_MODEL_ARENA_SIZE - bytes)
    { puts("MODEL DDR allocation outside arena"); k7_model_free(memory); nxmutex_unlock(&g_test_lock); return 1; }
  ret = 0;
  for (unsigned pass = 0; pass < 2 && ret == 0; ++pass)
    {
      for (size_t offset = 0; offset < bytes; offset += 4096)
        {
          volatile uint64_t *p = (volatile uint64_t *)(memory + offset);
          p[0] = ((uint64_t)address + offset) ^ (pass ? UINT64_MAX : 0);
          p[7] = ~p[0];
          up_clean_dcache((uintptr_t)p, (uintptr_t)p + 64);
        }
      for (size_t offset = 0; offset < bytes; offset += 4096)
        {
          volatile uint64_t *p = (volatile uint64_t *)(memory + offset);
          up_invalidate_dcache((uintptr_t)p, (uintptr_t)p + 64);
          uint64_t expected = ((uint64_t)address + offset) ^ (pass ? UINT64_MAX : 0);
          if (p[0] != expected || p[7] != ~expected)
            { printf("MODEL DDR mismatch offset=%zu pass=%u\n", offset, pass); ret = 1; break; }
        }
    }
  k7_model_free(memory);
  size_t after = k7_model_available();
  if (after != before) ret = 1;
  printf("MODEL DDR sparse_test=%s allocation=%zu page_samples=%zu passes=2 free_before=%zu free_after=%zu\n",
         ret == 0 ? "PASS" : "FAIL", bytes, bytes / 4096, before, after);
  nxmutex_unlock(&g_test_lock);
  return ret;
}
