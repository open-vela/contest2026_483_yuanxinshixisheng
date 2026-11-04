#include "sdhci_command.h"
#include <errno.h>
#include <string.h>

#define PRESENT 0x24u
#define INT_STATUS 0x30u
#define ERRORS 0xffff8000u
#define R1_ERRORS (~UINT32_C(0x0206bf7f))

int vv_sdhci_read_sector(struct vv_sdhci_host *h, uint64_t lba,
                         uint8_t *buffer, size_t capacity, uint32_t timeout_us,
                         struct vv_sdhci_command_result *r)
{
  struct vv_sdhci_io *io;
  uint64_t start, elapsed;
  int rc;
  if (r) memset(r, 0, sizeof(*r));
  if (!h || !r || !buffer || capacity < 512 || !timeout_us ||
      !h->write16 || !h->io.read32 || !h->io.write32 ||
      !h->io.now_us || !h->io.pause) return -EINVAL;
  if (!h->ready || h->needs_recovery) return -EAGAIN;
  if (!h->sector_count || lba >= h->sector_count || lba > UINT32_MAX)
    return -ERANGE;
  io = &h->io;
  start = io->now_us(io->ctx);
  for (;;) {
    r->elapsed_us = io->now_us(io->ctx) - start;
    if (r->elapsed_us >= timeout_us) return -ETIMEDOUT;
    r->data.present_state = io->read32(io->ctx, PRESENT);
    if (!(r->data.present_state & 3u)) break; /* CMD/DATA inhibit */
    io->pause(io->ctx);
  }
  /* Clear stale command/data status, then program a single non-DMA read.
   * Do not clear unrelated card insertion/removal/SDIO event status.
   */
  io->write32(io->ctx, INT_STATUS, ERRORS | 0x33u);
  h->write16(io->ctx, 0x04, 512);
  h->write16(io->ctx, 0x06, 1);
  io->write32(io->ctx, 0x08, (uint32_t)lba);
  h->write16(io->ctx, 0x0c, 0x12); /* READ | BLOCK_COUNT; DMA/MULTI off */
  r->elapsed_us = io->now_us(io->ctx) - start;
  if (r->elapsed_us >= timeout_us) return -ETIMEDOUT;
  h->needs_recovery = 1;
  r->command_issued = 1;
  h->write16(io->ctx, 0x0e, 0x113a); /* CMD17, data, index/CRC, short R1 */
  for (;;) {
    r->elapsed_us = io->now_us(io->ctx) - start;
    if (r->elapsed_us >= timeout_us) return -ETIMEDOUT;
    r->data.interrupt_status = io->read32(io->ctx, INT_STATUS);
    r->data.present_state = io->read32(io->ctx, PRESENT);
    if (r->data.interrupt_status & ERRORS) return -EIO;
    if (r->data.interrupt_status & 1u) break;
    io->pause(io->ctx);
  }
  r->r1 = io->read32(io->ctx, 0x10);
  if (r->r1 & R1_ERRORS) return -EIO;
  io->write32(io->ctx, INT_STATUS, 1u);
  elapsed = io->now_us(io->ctx) - start;
  if (elapsed >= timeout_us) { r->elapsed_us = elapsed; return -ETIMEDOUT; }
  rc = vv_sdhci_read512(io, buffer, capacity, timeout_us - (uint32_t)elapsed,
                         &r->data);
  r->elapsed_us = io->now_us(io->ctx) - start;
  if (r->elapsed_us >= timeout_us) rc = -ETIMEDOUT;
  if (!rc) h->needs_recovery = 0;
  return rc;
}
