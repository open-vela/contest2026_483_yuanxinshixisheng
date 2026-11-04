#ifndef VELAVISION_SDHCI_COMMAND_H
#define VELAVISION_SDHCI_COMMAND_H
#include "sdhci_read.h"

/* Exclusive polling backend, initialized 512-byte sector-addressed eMMC,
 * USER partition, PIO mode, clocks running, hardware data timeout configured,
 * interrupt status latching enabled.
 * Platform must verify these before setting ready. No IRQ handler may consume
 * this controller's status concurrently. This module never changes partitions.
 */
struct vv_sdhci_host {
  struct vv_sdhci_io io;
  void (*write16)(void *, unsigned, uint16_t);
  uint64_t sector_count;
  int ready;
  int needs_recovery;
};
struct vv_sdhci_command_result {
  struct vv_sdhci_result data;
  uint32_t r1;
  uint64_t elapsed_us;
  int command_issued;
};
int vv_sdhci_read_sector(struct vv_sdhci_host *host, uint64_t lba,
                         uint8_t *buffer, size_t capacity, uint32_t timeout_us,
                         struct vv_sdhci_command_result *result);
#endif
