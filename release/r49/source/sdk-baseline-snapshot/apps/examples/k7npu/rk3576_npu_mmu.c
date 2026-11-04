/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed RK3576 MMU lifecycle, independent implementation. Register semantics:
 * Rockchip kernel 77168c8d5ab82399f65a80e9f807b50ba37cf483 rockchip-iommu.c.
 */
#include "rk3576_npu_mmu.h"
#include <errno.h>
#include <string.h>

static const uintptr_t g_mmu[4] =
  {0x27702000, 0x27702100, 0x2770a000, 0x2770a100};

static int poll(const struct rk3576_npu_io *io, uintptr_t base,
                int paging, uint32_t *last)
{
  unsigned int retry;
  for (retry = 0; retry < 100; retry++)
    {
      *last = io->read(base + 4);
      if (*last & 2u) return -EIO;
      if ((*last & 0x1fu) == (paging ? 0x19u : 0x18u)) return 0;
      io->delay();
    }
  return -ETIMEDOUT;
}

int rk3576_npu_mmu_run(const struct rk3576_npu_io *io, uint32_t dt_phys,
                      int (*work)(void *), void *arg,
                      struct rk3576_npu_mmu_result *result)
{
  uint32_t saved[4];
  unsigned int i;
  int ret = 0;
  if (!io || !io->read || !io->write || !io->delay || !result ||
      !dt_phys || (dt_phys & 4095u)) return -EINVAL;
  memset(result, 0, sizeof(*result));

  /* Check every bank before the first write. Never adopt an active mapping,
   * clear a fault, or take ownership of a running job in this diagnostic.
   */
  for (i = 0; i < 4; i++)
    {
      result->before[i] = io->read(g_mmu[i] + 4);
      saved[i] = io->read(g_mmu[i]);
      if (result->before[i] != 0x18 || io->read(g_mmu[i] + 0x14) != 0)
        return -EBUSY;
    }

  for (i = 0; i < 4; i++)
    {
      result->touched |= 1u << i;
      io->write(g_mmu[i], dt_phys);
      if (io->read(g_mmu[i]) != dt_phys)
        { ret = -EIO; break; }
      io->write(g_mmu[i] + 8, 4); /* invalidate IOTLB */
      io->write(g_mmu[i] + 8, 0); /* enable paging, no request submitted */
      ret = poll(io, g_mmu[i], 1, &result->enabled[i]);
      if (ret) break;
    }

  if (ret == 0 && work && work(arg) != 0)
    {
      /* A timed-out device may still DMA. The caller's static buffers remain
       * valid and mapped; only a board reboot can release this ownership.
       */
      result->retained = result->touched;
      return -EIO;
    }

  /* Always attempt cleanup on every touched bank. Do not detach a table if
   * disable did not complete. Storage must remain owned until board reboot.
   */
  for (i = 0; i < 4; i++)
    {
      if (!(result->touched & (1u << i))) continue;
      io->write(g_mmu[i] + 8, 1);
      if (poll(io, g_mmu[i], 0, &result->after[i]))
        {
          result->retained |= 1u << i;
          ret = -EIO;
          continue;
        }
      io->write(g_mmu[i], saved[i]);
      io->write(g_mmu[i] + 8, 4);
      if (io->read(g_mmu[i]) != saved[i])
        {
          result->retained |= 1u << i;
          ret = -EIO;
        }
    }
  return ret;
}

int rk3576_npu_mmu_cycle(const struct rk3576_npu_io *io, uint32_t dt_phys,
                        struct rk3576_npu_mmu_result *result)
{
  return rk3576_npu_mmu_run(io, dt_phys, NULL, NULL, result);
}
