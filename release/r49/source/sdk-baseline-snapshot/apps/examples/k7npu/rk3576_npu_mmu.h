/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RK3576_NPU_MMU_H
#define RK3576_NPU_MMU_H
#include <stdint.h>
#include <stddef.h>

struct rk3576_npu_io
{
  uint32_t (*read)(uintptr_t address);
  void (*write)(uintptr_t address, uint32_t value); /* includes MMIO barrier */
  void (*delay)(void); /* bounded polling interval */
};

struct rk3576_npu_mmu_result
{
  uint32_t before[4];
  uint32_t enabled[4];
  uint32_t after[4];
  unsigned int touched;
  unsigned int retained; /* cannot restore: retain tables until board reboot */
};

/* Idle-only register lifecycle test. Caller holds exclusive NPU ownership,
 * checks core/power/clock/reset, owns a cache-clean low-32-bit physical DT.
 * No NPU task starts and no DMA/page walk is validated by this operation.
 */
int rk3576_npu_mmu_cycle(const struct rk3576_npu_io *io, uint32_t dt_phys,
                        struct rk3576_npu_mmu_result *result);

/* Experimental workload hook. A nonzero callback result means DMA completion
 * is uncertain: no mapping is detached, and every table must remain owned
 * until board reboot. Exclusive ownership is held by the caller throughout.
 */
int rk3576_npu_mmu_run(const struct rk3576_npu_io *io, uint32_t dt_phys,
                      int (*work)(void *), void *arg,
                      struct rk3576_npu_mmu_result *result);
#endif
