#include "emmc_init.h"
#include <errno.h>
#include <string.h>

struct init { const struct vv_card_io *io; struct vv_card *card;
              uint64_t start; uint32_t budget; };
static int send(struct init *s, unsigned cmd, uint32_t arg,
                enum vv_response type, uint32_t r[4], uint8_t *data)
{
  uint64_t elapsed = s->io->now_us(s->io->ctx) - s->start;
  int rc;
  s->card->last_command = cmd;
  if (elapsed >= s->budget) return -ETIMEDOUT;
  memset(r, 0, 4 * sizeof(*r));
  rc = s->io->command(s->io->ctx, cmd, arg, type, r, data,
                       s->budget - (uint32_t)elapsed);
  if (s->io->now_us(s->io->ctx) - s->start >= s->budget)
    return -ETIMEDOUT;
  if (!rc && (type == VV_R1 || type == VV_R1B) &&
      (r[0] & ~UINT32_C(0x0206bf7f))) return -EIO;
  return rc;
}
int vv_emmc_initialize(const struct vv_card_io *io, struct vv_card *card,
                       uint32_t timeout_us)
{
  struct init s;
  uint32_t r[4]; uint8_t ext[512]; int rc;
  if (card) memset(card, 0, sizeof(*card));
  if (!io || !card || !io->command || !io->now_us || !io->pause ||
      !timeout_us) return -EINVAL;
  s = (struct init){io, card, io->now_us(io->ctx), timeout_us};
  rc = send(&s, 0, 0, VV_NONE, r, NULL);
  if (rc) return rc;
  io->pause(io->ctx);
  do {
    /* Request sector addressing and 2.7..3.6V device supply (not I/O rail). */
    rc = send(&s, 1, UINT32_C(0x40ff8000), VV_R3, r, NULL);
    if (rc) return rc;
    card->ocr = r[0];
    if (r[0] & UINT32_C(0x80000000)) break;
    io->pause(io->ctx);
  } while (1);
  if ((card->ocr & UINT32_C(0x60000000)) != UINT32_C(0x40000000) ||
      !(card->ocr & UINT32_C(0x00ff8000))) return -ENOTSUP;
  rc = send(&s, 2, 0, VV_R2, card->cid, NULL);
  if (rc) return rc;
  rc = send(&s, 3, UINT32_C(0x00010000), VV_R1, r, NULL);
  if (rc) return rc;
  rc = send(&s, 9, UINT32_C(0x00010000), VV_R2, card->csd, NULL);
  if (rc) return rc;
  rc = send(&s, 7, UINT32_C(0x00010000), VV_R1B, r, NULL);
  if (rc) return rc;
  rc = send(&s, 8, 0, VV_R1, r, ext);
  if (rc) return rc;
  card->revision = ext[192]; card->partition = ext[179] & 7;
  card->bus_width = ext[183]; card->timing = ext[185];
  card->sectors = (uint32_t)ext[212] | (uint32_t)ext[213] << 8 |
                  (uint32_t)ext[214] << 16 | (uint32_t)ext[215] << 24;
  /* DATA_SECTOR_SIZE[61]: 0=512B, 1=4KiB. Legacy bus/timing required. */
  if (card->revision < 2 || card->partition || card->bus_width ||
      card->timing || ext[61] || !card->sectors) return -ENOTSUP;
  rc = send(&s, 13, UINT32_C(0x00010000), VV_R1, r, NULL);
  if (rc) return rc;
  if ((r[0] & 0x1f00u) != 0x900u) return -EIO; /* READY, TRAN */
  card->ready = 1;
  return 0;
}
