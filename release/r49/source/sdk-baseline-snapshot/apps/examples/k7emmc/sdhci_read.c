#include "sdhci_read.h"
#include <errno.h>
#include <string.h>

#define BUFFER 0x20u
#define STATE 0x24u
#define STATUS 0x30u
#define DATA_AVAILABLE (1u << 11)
#define READ_READY (1u << 5)
#define TRANSFER_END (1u << 1)
#define ERRORS 0xffff8000u

int vv_sdhci_read512(const struct vv_sdhci_io *io, uint8_t *buffer,
                     size_t capacity, uint32_t timeout_us,
                     struct vv_sdhci_result *result)
{
  uint64_t start;
  unsigned n;
  if (result) memset(result, 0, sizeof(*result));
  if (!io || !buffer || !result || capacity < 512 || !timeout_us ||
      !io->read32 || !io->write32 || !io->now_us || !io->pause)
    return -EINVAL;
  start = io->now_us(io->ctx);
  for (;;) {
    result->elapsed_us = io->now_us(io->ctx) - start;
    if (result->elapsed_us >= timeout_us) return -ETIMEDOUT;
    result->interrupt_status = io->read32(io->ctx, STATUS);
    result->present_state = io->read32(io->ctx, STATE);
    if (result->interrupt_status & ERRORS) return -EIO;
    if (!result->bytes_read &&
        (result->interrupt_status & READ_READY) &&
        (result->present_state & DATA_AVAILABLE)) {
      io->write32(io->ctx, STATUS, READ_READY);
      for (n = 0; n < 128; ++n) {
        uint32_t word;
        result->elapsed_us = io->now_us(io->ctx) - start;
        if (result->elapsed_us >= timeout_us) return -ETIMEDOUT;
        word = io->read32(io->ctx, BUFFER);
        buffer[4*n] = (uint8_t)word;
        buffer[4*n+1] = (uint8_t)(word >> 8);
        buffer[4*n+2] = (uint8_t)(word >> 16);
        buffer[4*n+3] = (uint8_t)(word >> 24);
        result->bytes_read += 4;
      }
      /* Re-read status on the next iteration: errors can arrive during PIO. */
    } else if (result->interrupt_status & TRANSFER_END) {
      if (result->bytes_read != 512) return -EIO;
      io->write32(io->ctx, STATUS, TRANSFER_END);
      return 0;
    }
    /* Including ready/status disagreement: never bypass deadline or yield. */
    io->pause(io->ctx);
  }
}
