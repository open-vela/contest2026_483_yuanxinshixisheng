#ifndef VELAVISION_SDHCI_READ_H
#define VELAVISION_SDHCI_READ_H
#include <stdint.h>
#include <stddef.h>

/* Collect one already-issued 512-byte PIO read. Caller owns the controller,
 * has completed card initialization and command/R1 checking, and must reset
 * the data path after any error before issuing another command. No DMA.
 * Callbacks must return in bounded time; now_us is monotonic modulo 2^64.
 * Buffer contents are valid ONLY on return 0. This does not start commands.
 */
struct vv_sdhci_io {
  void *ctx;
  uint32_t (*read32)(void *, unsigned);
  void (*write32)(void *, unsigned, uint32_t);
  uint64_t (*now_us)(void *);
  void (*pause)(void *);
};
struct vv_sdhci_result {
  uint32_t interrupt_status;
  uint32_t present_state;
  size_t bytes_read;
  uint64_t elapsed_us;
};
int vv_sdhci_read512(const struct vv_sdhci_io *io, uint8_t *buffer,
                     size_t capacity, uint32_t timeout_us,
                     struct vv_sdhci_result *result);
#endif
