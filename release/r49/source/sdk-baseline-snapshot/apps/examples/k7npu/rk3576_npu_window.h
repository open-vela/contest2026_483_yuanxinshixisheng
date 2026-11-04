/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RK3576_NPU_WINDOW_H
#define RK3576_NPU_WINDOW_H
#include <stddef.h>
#include <stdint.h>

/* Construct one isolated <=4MiB window, physical addresses below 4GiB.
 * dt/pt are distinct 4KiB-aligned CPU buffers of 1024 uint32_t each.
 * Caller owns the physical buffers, keeps the directory outside the data
 * window, verifies the CPU/physical translation,
 * flushes tables/data, and installs them only after NPU/MMU initialization.
 * This function writes RAM tables only; it never installs them or starts DMA.
 */
int rk3576_npu_make_window(uint32_t *dt, uint32_t *pt, uint64_t pt_phys,
                          uint32_t iova, uint64_t data_phys, size_t bytes);
#endif
