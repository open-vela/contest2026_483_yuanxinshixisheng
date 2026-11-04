/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_DWC3_TRANSPORT_CORE_H
#define K7_DWC3_TRANSPORT_CORE_H
#include <stddef.h>
#include <stdint.h>
/* Controller offsets/fields verified against pinned Linux DWC3 register
 * definitions. This module owns no MMIO mapping or DMA allocation. */
struct k7_dwc3_bus
{
  void *arg;
  uint32_t (*read)(void *,uint32_t);
  void (*write)(void *,uint32_t,uint32_t);
  uint64_t (*now_us)(void *);
  void (*delay_us)(void *,unsigned int);
};
int k7_dwc3_ep_command(const struct k7_dwc3_bus *,unsigned int ep,
                      unsigned int command,uint32_t parameters,
                      const uint32_t args[3],uint32_t *completion);
/* Event count includes non-count flags. Reject an overrun or partial event. */
int k7_dwc3_event_bytes(uint32_t count,size_t capacity,size_t *bytes);
/* Only physical addresses from the caller's verified DMA aperture are valid.
 * No Apple IOVA constants or identity-mapping assumptions are accepted here. */
int k7_dwc3_dma_range(uint64_t address,size_t bytes,uint64_t base,size_t capacity,
                     size_t alignment);
#endif
