#include "sdhci_control.h"
#include <errno.h>

int vv_sdhci_reset_lines(struct vv_sdhci_control *c, uint32_t budget)
{
  struct vv_sdhci_io *io = &c->host->io;
  uint64_t start = io->now_us(io->ctx);
  c->host->ready = 0;
  c->host->needs_recovery = 1;
  c->write8(io->ctx, 0x2f, 6); /* CMD and DATA only; keep power/clock */
  while (c->read8(io->ctx, 0x2f) & 6) {
    if (io->now_us(io->ctx) - start >= budget) return -ETIMEDOUT;
    io->pause(io->ctx);
  }
  if (io->now_us(io->ctx) - start >= budget) return -ETIMEDOUT;
  c->host->needs_recovery = 0;
  return 0; /* card must be initialized again before ready can be set */
}

int vv_sdhci_init_command(void *p, unsigned cmd, uint32_t arg,
                          enum vv_response type, uint32_t response[4],
                          uint8_t *data, uint32_t budget)
{
  struct vv_sdhci_control *c = p;
  struct vv_sdhci_host *h = c->host;
  struct vv_sdhci_io *io = &h->io;
  uint64_t start = io->now_us(io->ctx), elapsed;
  unsigned flags, i; int rc;
  /* Closed allow-list: no SWITCH, block writes, erase or vendor commands. */
  if (!budget || !response ||
      !((cmd==0 && type==VV_NONE && arg==0 && !data) ||
        (cmd==1 && type==VV_R3 && arg==0x40ff8000 && !data) ||
        (cmd==2 && type==VV_R2 && arg==0 && !data) ||
        ((cmd==3 || cmd==13) && type==VV_R1 && arg==0x10000 && !data) ||
        (cmd==9 && type==VV_R2 && arg==0x10000 && !data) ||
        (cmd==7 && type==VV_R1B && arg==0x10000 && !data) ||
        (cmd==8 && type==VV_R1 && arg==0 && data))) return -EINVAL;
  if (h->needs_recovery || h->ready) return -EAGAIN;
  c->last_command = cmd;
  for (;;) {
    if (io->now_us(io->ctx)-start >= budget) return -ETIMEDOUT;
    c->last_state = io->read32(io->ctx, 0x24);
    if (!(c->last_state & 3)) break;
    io->pause(io->ctx);
  }
  io->write32(io->ctx, 0x30, 0xffff8033u);
  flags = type==VV_NONE ? 0 : type==VV_R2 ? 9 : type==VV_R3 ? 2 :
          type==VV_R1B ? 0x1b : 0x1a;
  if (data) {
    flags |= 0x20;
    h->write16(io->ctx, 4, 512); h->write16(io->ctx, 6, 1);
    h->write16(io->ctx, 0x0c, 0x12);
  } else h->write16(io->ctx, 0x0c, 0);
  io->write32(io->ctx, 8, arg);
  if (io->now_us(io->ctx)-start >= budget) return -ETIMEDOUT;
  h->needs_recovery = 1;
  h->write16(io->ctx, 0x0e, (uint16_t)((cmd<<8)|flags));
  for (;;) {
    if (io->now_us(io->ctx)-start >= budget) return -ETIMEDOUT;
    c->last_status = io->read32(io->ctx, 0x30);
    c->last_state = io->read32(io->ctx, 0x24);
    if (c->last_status & 0xffff8000u) return -EIO;
    if (c->last_status & 1) break;
    io->pause(io->ctx);
  }
  /* Preserve the four raw SDHCI response words for CID/CSD diagnostics. */
  for (i=0; i<4; ++i) response[i]=io->read32(io->ctx, 0x10+4*i);
  io->write32(io->ctx, 0x30, 1);
  if ((type==VV_R1 || type==VV_R1B) &&
      (response[0] & ~UINT32_C(0x0206bf7f))) return -EIO;
  if (type==VV_R1B) {
    for (;;) {
      if (io->now_us(io->ctx)-start >= budget) return -ETIMEDOUT;
      c->last_status=io->read32(io->ctx,0x30);
      c->last_state=io->read32(io->ctx,0x24);
      if (c->last_status & 0xffff8000u) return -EIO;
      if ((c->last_status & 2) && !(c->last_state & 2)) break;
      io->pause(io->ctx);
    }
    io->write32(io->ctx,0x30,2);
  }
  if (data) {
    struct vv_sdhci_result r;
    elapsed=io->now_us(io->ctx)-start;
    if (elapsed >= budget) return -ETIMEDOUT;
    rc=vv_sdhci_read512(io,data,512,budget-(uint32_t)elapsed,&r);
    c->last_status=r.interrupt_status; c->last_state=r.present_state;
    if (rc) return rc;
  }
  if (io->now_us(io->ctx)-start >= budget) return -ETIMEDOUT;
  h->needs_recovery=0;
  return 0;
}
