#ifndef VELAVISION_SDHCI_CONTROL_H
#define VELAVISION_SDHCI_CONTROL_H
#include "sdhci_command.h"
#include "emmc_init.h"
struct vv_sdhci_control {
  struct vv_sdhci_host *host;
  uint8_t (*read8)(void *, unsigned);
  void (*write8)(void *, unsigned, uint8_t);
  uint32_t last_status, last_state;
  unsigned last_command;
};
int vv_sdhci_reset_lines(struct vv_sdhci_control *, uint32_t budget);
int vv_sdhci_init_command(void *, unsigned, uint32_t, enum vv_response,
                          uint32_t response[4], uint8_t *, uint32_t budget);
#endif
