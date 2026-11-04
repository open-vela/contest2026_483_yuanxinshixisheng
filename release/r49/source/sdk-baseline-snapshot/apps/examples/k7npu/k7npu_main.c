/* SPDX-License-Identifier: Apache-2.0 */
/* RK3576 NPU prerequisite snapshot and guarded probe. No register writes.
 * Field provenance: Rockchip kernel 77168c8d5ab82399f65a80e9f807b50ba37cf483,
 * clk-rk3576.c, pm_domains.c and rockchip,rk3576-cru.h.
 * PMU/CRU mappings currently supplied by RK3576_USB_DIAG in the K7 baseline.
 */
#ifndef K7NPU_TEST
#include <nuttx/config.h>
#endif
#ifdef CONFIG_K7NPU_RAW_TEST
int k7npu_rawtest(void (*emit)(const char *));
int k7npu_rawrepeat(void (*emit)(const char *));
#endif
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_K7NPU_MMU_TEST
int k7npu_mmutest(void (*emit)(const char *));
#endif

#define CRU UINT32_C(0x27200000)
#define PMU UINT32_C(0x27380000)
#define BIT(n) (UINT32_C(1) << (n))

struct npu_reg
{
  const char *name;
  uintptr_t address;
};

static const struct npu_reg g_regs[] =
{
  {"POWER_STATUS", PMU + 0x230},
  {"REPAIR_STATUS", PMU + 0x570},
  {"BUS_IDLE",     PMU + 0x128},
  {"BUS_ACK",      PMU + 0x120},
  {"GATE28",       CRU + 0x870},
  {"GATE29",       CRU + 0x874},
  {"GATE31",       CRU + 0x87c},
  {"GATE32",       CRU + 0x880},
  {"RESET28",      CRU + 0xa70},
  {"RESET29",      CRU + 0xa74},
  {"RESET31",      CRU + 0xa7c},
  {"RESET32",      CRU + 0xa80}
};

#ifdef K7NPU_TEST
extern uint32_t k7npu_read32(uintptr_t address);
#else
static uint32_t k7npu_read32(uintptr_t address)
{
  return *(volatile const uint32_t *)address;
}
#endif

static void emit(const char *text)
{
#ifdef K7NPU_TEST
  (void)text;
#else
  const struct timespec gap = {0, 1000000};
  while (*text)
    {
      putchar(*text++);
      fflush(stdout);
      nanosleep(&gap, NULL);
    }
#endif
}

int main(int argc, char **argv)
{
  uint32_t value[sizeof(g_regs) / sizeof(g_regs[0])];
  char line[112];
  unsigned int i;
  unsigned int power;
  unsigned int idle;
  unsigned int gated;
  unsigned int reset;

  if (argc != 2 || (strcmp(argv[1], "status") != 0 &&
                    strcmp(argv[1], "probe") != 0
#ifdef CONFIG_K7NPU_MMU_TEST
                    && strcmp(argv[1], "mmutest") != 0
#endif
#ifdef CONFIG_K7NPU_RAW_TEST
                    && strcmp(argv[1], "rawtest") != 0
                    && strcmp(argv[1], "rawrepeat") != 0
#endif
                   ))
    {
      emit("Usage: k7npu status | probe (fixed read-only registers)\n");
#ifdef CONFIG_K7NPU_MMU_TEST
      emit("       k7npu mmutest (idle MMU paging cycle, no NPU task)\n");
#endif
#ifdef CONFIG_K7NPU_RAW_TEST
      emit("       k7npu rawtest (one-shot experimental NPU matrix)\n");
      emit("       k7npu rawrepeat (8 input variants, bounded experimental session)\n");
#endif
      return 1;
    }

  emit("K7 NPU prerequisite snapshot v1 - READ ONLY\n");
  for (i = 0; i < sizeof(g_regs) / sizeof(g_regs[0]); i++)
    {
      value[i] = k7npu_read32(g_regs[i].address);
      snprintf(line, sizeof(line), "%s @%08" PRIxPTR "=%08" PRIx32 "\n",
               g_regs[i].name, g_regs[i].address, value[i]);
      emit(line);
    }

  /* Root power status is active-low; child repair bits are active-high. */
  power = !(value[0] & BIT(0)) &&
          ((value[1] & (BIT(22) | BIT(23) | BIT(24))) ==
           (BIT(22) | BIT(23) | BIT(24)));
  idle = (value[2] | value[3]) & (BIT(1) | BIT(2) | BIT(3) | BIT(4));
  gated = (value[4] & BIT(9)) | (value[5] & BIT(0)) |
          (value[6] & (BIT(4) | BIT(5))) |
          (value[7] & (BIT(0) | BIT(12)));
  reset = (value[8] & (BIT(9) | BIT(11) | BIT(12))) |
          (value[9] & (BIT(0) | BIT(2) | BIT(3))) |
          (value[10] & (BIT(1) | BIT(9))) |
          (value[11] & (BIT(0) | BIT(11) | BIT(12) | BIT(13)));
  snprintf(line, sizeof(line), "NPU flags power_on=%u bus_idle=%u gated=%u reset=%u\n",
           power, !!idle, !!gated, !!reset);
  emit(line);
  if (strcmp(argv[1], "status") != 0)
    {
      if (!power || idle || gated || reset)
        {
          emit("BLOCKED: power/clock/reset checks failed; no core reads.\n");
          return 1;
        }

      /* Fixed status/version offsets only. No command, reset, IRQ-clear,
       * descriptor or DMA writes. IOMMU status offset 4 is read-only.
       */
      for (i = 0; i < 2; i++)
        {
          uintptr_t base = UINT32_C(0x27700000) + i * UINT32_C(0x8000);
          static const uint16_t offsets[] = {0x0, 0x4, 0x8, 0x28, 0x2c, 0x48,
                                             0x2004, 0x2104};
          unsigned int j;
          for (j = 0; j < sizeof(offsets) / sizeof(offsets[0]); j++)
            {
              uintptr_t address = base + offsets[j];
              uint32_t raw = k7npu_read32(address);
              snprintf(line, sizeof(line), "CORE%u @%08" PRIxPTR
                       "=%08" PRIx32 "\n", i, address, raw);
              emit(line);
            }
        }
      emit("CORE/MMU read-only probe completed. No task submitted.\n");
#ifdef CONFIG_K7NPU_MMU_TEST
      if (strcmp(argv[1], "mmutest") == 0) return k7npu_mmutest(emit);
#endif
#ifdef CONFIG_K7NPU_RAW_TEST
      if (strcmp(argv[1], "rawtest") == 0) return k7npu_rawtest(emit);
      if (strcmp(argv[1], "rawrepeat") == 0) return k7npu_rawrepeat(emit);
#endif
    }
  else
    {
      emit("NPU core/MMU not accessed. No DMA or task submitted.\n");
    }
  emit("Inference unavailable: initialization/runtime still required.\n");
  return 0;
}
