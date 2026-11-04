/* SPDX-License-Identifier: Apache-2.0 */
#include "dwc3_transport_core.h"
#include <errno.h>
#include <stdbool.h>
#define DEPCMDPAR2(ep) (0xc800u+16u*(ep))
#define DEPCMD(ep) (0xc80cu+16u*(ep))
#define CMDACT (1u<<10)
int k7_dwc3_ep_command(const struct k7_dwc3_bus *b,unsigned int ep,
                      unsigned int command,uint32_t parameters,
                      const uint32_t args[3],uint32_t *completion)
{
  if (!b || !b->read || !b->write || !b->now_us || !b->delay_us ||
      !args || !completion || ep>=32 || command<1 || command>9 ||
      (parameters&0xffffu)) return -EINVAL;
  uint32_t reg=DEPCMD(ep);
  if (b->read(b->arg,reg)&CMDACT) return -EBUSY;
  /* Parameters are ordered PAR0/PAR1/PAR2 in the public interface. */
  for (unsigned int i=0;i<3;i++)
    b->write(b->arg,DEPCMDPAR2(ep)+4u*(2u-i),args[i]);
  b->write(b->arg,reg,parameters|CMDACT|command);
  uint64_t start=b->now_us(b->arg);
  /* Both time and iteration limits: a stalled timer must not hang the OS. */
  for (unsigned int attempts=0;attempts<1000;attempts++)
    {
      uint32_t value=b->read(b->arg,reg);
      if (!(value&CMDACT))
        {
          *completion=value;
          return ((value>>12)&15u) ? -EIO : 0;
        }
      if (b->now_us(b->arg)-start>=1000) break;
      b->delay_us(b->arg,1);
    }
  return -ETIMEDOUT;
}
int k7_dwc3_event_bytes(uint32_t count,size_t capacity,size_t *bytes)
{
  if (!bytes || !capacity || capacity>65532 || capacity%4) return -EINVAL;
  /* DWC3 event count low 16 bits represent bytes; upper bits are flags. */
  size_t size=count&0xffffu;
  if (size%4 || size>capacity) return -EOVERFLOW;
  *bytes=size;
  return 0;
}
int k7_dwc3_dma_range(uint64_t address,size_t bytes,uint64_t base,size_t capacity,
                     size_t alignment)
{
  if (!alignment || (alignment&(alignment-1)) || !bytes ||
      address%alignment || base>UINT64_MAX-capacity ||
      address<base || address-base>capacity || bytes>capacity-(address-base))
    return -EINVAL;
  return 0;
}
