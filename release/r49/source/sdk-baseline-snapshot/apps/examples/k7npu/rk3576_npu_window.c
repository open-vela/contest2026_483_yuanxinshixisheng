/* SPDX-License-Identifier: Apache-2.0 */
/* Rockchip IOMMU-v2 low-32-bit subset: 4KiB pages, 10/10/12 IOVA split.
 * Hardware format reference: pinned rockchip-iommu.c recorded in reference/.
 */
#include "rk3576_npu_window.h"
#include <errno.h>
#include <string.h>

int rk3576_npu_make_window(uint32_t *dt, uint32_t *pt, uint64_t pt_phys,
                          uint32_t iova, uint64_t data_phys, size_t bytes)
{
  size_t i;
  if (!dt || !pt || dt == pt || ((uintptr_t)dt & 4095) ||
      ((uintptr_t)pt & 4095) || (pt_phys & 4095) ||
      (data_phys & 4095) || (iova & 0x3fffff) ||
      bytes == 0 || (bytes & 4095))
    {
      return -EINVAL;
    }

  /* Deliberately reject high physical addresses until the BSP/DMA contract
   * supports them. Avoid silent truncation of RK3576's 40-bit DMA addresses.
   */
  if (bytes > 0x400000 || pt_phys > UINT32_MAX - 4095u ||
      data_phys > UINT32_MAX || bytes - 1 > UINT32_MAX - data_phys)
    {
      return -ERANGE;
    }

  if (pt_phys < data_phys + bytes && data_phys < pt_phys + 4096)
    {
      return -EINVAL;
    }

  memset(dt, 0, 4096);
  memset(pt, 0, 4096);
  dt[iova >> 22] = (uint32_t)pt_phys | 1u;
  for (i = 0; i < bytes / 4096; i++)
    {
      pt[i] = (uint32_t)(data_phys + i * 4096) | 7u;
    }
  return 0;
}
