/* SPDX-License-Identifier: GPL-2.0-only */
/* Register/PIO behavior from Rockchip K7 SDK dw_mmc.c/.h and rk3576.dtsi.
 * Polling prototype: no DMA addresses, IRQ registration, clock/pin changes,
 * card initialization, or automatic continuation of a failed transaction.
 */
#include "skw_dw.h"
#include <errno.h>
#include <string.h>

#define BIT(n) (UINT32_C(1) << (n))
#define CTRL 0x00u
#define PWREN 0x04u
#define CLKENA 0x10u
#define BLKSIZ 0x1cu
#define BYTCNT 0x20u
#define INTMASK 0x24u
#define CMDARG 0x28u
#define CMD 0x2cu
#define RESP0 0x30u
#define RINTSTS 0x44u
#define STATUS 0x48u
#define FIFOTH 0x4cu
#define TCBCNT 0x5cu
#define VERID 0x6cu
#define HCON 0x70u
#define BMOD 0x80u
#define DATA 0x200u
#define START BIT(31)
#define CMD_DONE BIT(2)
#define DTO BIT(3)
#define RXDR BIT(5)
#define TXDR BIT(4)
#define CMD_ERRORS (BIT(1) | BIT(6) | BIT(8) | BIT(12))
#define ERRORS (CMD_ERRORS | BIT(7) | BIT(9) | BIT(10) | BIT(11) | BIT(13) | BIT(15))
#define EVENTS (ERRORS | CMD_DONE | DTO | RXDR | TXDR)
#define FORBIDDEN_CTRL (BIT(25) | BIT(5) | BIT(4) | 7u)
#define DATA_BUSY (BIT(9) | BIT(10))
#define FIFO_DEPTH 256u

static uint32_t rd(struct skw_dw *dw, unsigned int offset)
{
  return dw->io->read(dw->ctx, offset);
}
static void wr(struct skw_dw *dw, unsigned int offset, uint32_t value)
{
  dw->io->write(dw->ctx, offset, value);
}
static int expired(struct skw_dw *dw, uint64_t start, unsigned int us,
                     unsigned int polls)
{
  /* The iteration cap also bounds a malfunctioning time source. */
  return dw->io->time_us(dw->ctx) - start >= us || polls >= us / 5u + 1u;
}
static int wait_clear(struct skw_dw *dw, unsigned int offset, uint32_t mask)
{
  uint64_t start = dw->io->time_us(dw->ctx);
  unsigned int polls = 0;
  do
    {
      if (!(rd(dw, offset) & mask)) return 0;
      dw->io->delay_us(dw->ctx, 5);
    }
  while (!expired(dw, start, 100000, ++polls));
  return -ETIMEDOUT;
}
static int error_code(uint32_t raw)
{
  if (raw & (BIT(8) | BIT(9) | BIT(10))) return -ETIMEDOUT;
  if (raw & (BIT(6) | BIT(7))) return -EILSEQ;
  return -EIO;
}
static int available(struct skw_dw *dw)
{
  if (!dw || !dw->prepared) return -ENODEV;
  if (dw->faulted) return -EIO;
  if (rd(dw, CTRL) & FORBIDDEN_CTRL || rd(dw, INTMASK) || rd(dw, BMOD) & BIT(7))
    return -EBUSY;
  if (!(rd(dw, PWREN) & 1) || !(rd(dw, CLKENA) & 1)) return -EHOSTDOWN;
  if (rd(dw, CMD) & START || rd(dw, STATUS) & DATA_BUSY) return -EBUSY;
  return 0;
}
static int fail(struct skw_dw *dw, int error)
{
  uint32_t ctrl = rd(dw, CTRL);
  int ret;
  dw->last_raw = rd(dw, RINTSTS);
  dw->last_status = rd(dw, STATUS);
  dw->card_bytes = rd(dw, TCBCNT);
  dw->faulted = true;
  /* This instance exclusively owns SDMMC1. Reset only its internal engine;
   * no CMD12/CMD52 retry, no power/PLL/shared reset manipulation.
   * CLKENA=0 is pushed through UPDATE_CLOCK after reset completion.
   */
  wr(dw, CTRL, (ctrl & ~FORBIDDEN_CTRL) | 7u);
  ret = wait_clear(dw, CTRL, 7u);
  wr(dw, CLKENA, 0);
  if (!ret)
    {
      wr(dw, CMD, START | BIT(21) | BIT(13));
      ret = wait_clear(dw, CMD, START);
    }
  dw->cleanup_error = ret;
  wr(dw, RINTSTS, EVENTS);
  return error;
}

int skw_dw_prepare(struct skw_dw *dw, const struct skw_dw_io *io, void *ctx)
{
  int ret;
  if (!dw || !io || !io->read || !io->write || !io->time_us || !io->delay_us)
    return -EINVAL;
  memset(dw, 0, sizeof(*dw));
  dw->io = io;
  dw->ctx = ctx;
  /* Board-tested IP revision and 32-bit FIFO width. Do not guess another IP. */
  if (rd(dw, VERID) != 0x5342270au || ((rd(dw, HCON) >> 7) & 7u) != 1)
    return -ENODEV;
  dw->prepared = true;
  ret = available(dw);
  if (ret) dw->prepared = false;
  return ret;
}

int skw_dw_cmd52(struct skw_dw *dw, uint32_t arg, uint32_t *r5)
{
  uint64_t start;
  unsigned int polls = 0;
  int ret;
  if (!r5) return -EINVAL;
  *r5 = 0;
  if (((arg >> 28) & 7) > 1 || (arg & (BIT(26) | BIT(8))) ||
      (!(arg & BIT(31)) && (arg & BIT(27)))) return -EINVAL;
  ret = available(dw);
  if (ret) return ret;
  dw->host_bytes = 0;
  dw->cleanup_error = 0;
  wr(dw, RINTSTS, EVENTS);
  wr(dw, CMDARG, arg);
  wr(dw, CMD, START | BIT(29) | BIT(8) | BIT(6) | 52u);
  start = dw->io->time_us(dw->ctx);
  do
    {
      uint32_t raw = rd(dw, RINTSTS);
      if (raw & CMD_ERRORS) return fail(dw, error_code(raw));
      if (raw & CMD_DONE)
        {
          *r5 = rd(dw, RESP0);
          ret = skw_r5_status(*r5);
          if (ret) return fail(dw, ret);
          wr(dw, RINTSTS, CMD_DONE);
          return 0;
        }
      dw->io->delay_us(dw->ctx, 5);
    }
  while (!expired(dw, start, 200000, ++polls));
  return fail(dw, -ETIMEDOUT);
}

int skw_dw_cmd53(struct skw_dw *dw, uint32_t arg, void *buffer,
                  size_t length, uint32_t *r5)
{
  uint8_t *bytes = buffer;
  uint32_t blocks = arg & 0x1ff;
  uint32_t address = (arg >> 9) & 0x1ffff;
  bool blockmode = (arg & BIT(27)) != 0;
  bool write = (arg & BIT(31)) != 0;
  bool got_cmd = false;
  size_t expected = blockmode ? (size_t)blocks * 512u : (blocks ? blocks : 512u);
  uint64_t start;
  unsigned int polls = 0;
  int ret;
  if (!r5) return -EINVAL;
  *r5 = 0;
  if (!buffer || length == 0 || length != expected || length > 16384 ||
      ((arg >> 28) & 7) != 1) return -EINVAL;
  if ((arg & BIT(26)) && length - 1 > 0x1ffffu - address) return -ERANGE;
  ret = available(dw);
  if (ret) return ret;
  dw->host_bytes = 0;
  dw->cleanup_error = 0;
  /* FIFO depth is fixed by the SDK's rk3576.dtsi, not inferred from an
   * inherited (potentially modified) FIFO watermark register.
   */
  wr(dw, CTRL, rd(dw, CTRL) | BIT(1));
  ret = wait_clear(dw, CTRL, BIT(1));
  if (ret) return fail(dw, ret);
  wr(dw, FIFOTH, (2u << 28) | (127u << 16) | 128u);
  wr(dw, BLKSIZ, blockmode ? 512u : (uint32_t)length);
  wr(dw, BYTCNT, (uint32_t)length);
  wr(dw, RINTSTS, EVENTS);
  wr(dw, CMDARG, arg);
  wr(dw, CMD, START | BIT(29) | BIT(13) | BIT(9) | BIT(8) | BIT(6) |
                (write ? BIT(10) : 0) | 53u);
  start = dw->io->time_us(dw->ctx);
  do
    {
      uint32_t raw = rd(dw, RINTSTS);
      uint32_t count;
      if (raw & ERRORS) return fail(dw, error_code(raw));
      if (!got_cmd && (raw & CMD_DONE))
        {
          *r5 = rd(dw, RESP0);
          ret = skw_r5_status(*r5);
          if (ret) return fail(dw, ret);
          got_cmd = true;
          wr(dw, RINTSTS, CMD_DONE);
        }
      count = (rd(dw, STATUS) >> 17) & 0x1fff;
      if (count > FIFO_DEPTH) return fail(dw, -EIO);
      /* Fill TX FIFO without waiting for CMD_DONE: command/data IRQ order
       * may vary, and delaying filling can underflow a fast data engine.
       * RX bytes are valid only if this entire command ultimately succeeds.
       */
      if (write) count = (raw & DTO) ? 0 : FIFO_DEPTH - count;
      while (count && dw->host_bytes < length)
        {
          size_t n = length - dw->host_bytes;
          uint32_t word = 0;
          unsigned int j;
          if (n > 4) n = 4;
          if (write)
            {
              for (j = 0; j < n; j++)
                word |= (uint32_t)bytes[dw->host_bytes + j] << (8 * j);
              wr(dw, DATA, word);
            }
          else
            {
              word = rd(dw, DATA);
              for (j = 0; j < n; j++)
                bytes[dw->host_bytes + j] = (uint8_t)(word >> (8 * j));
            }
          dw->host_bytes += n;
          count--;
        }
      wr(dw, RINTSTS, raw & (RXDR | TXDR));
      if (raw & DTO)
        {
          /* DTO alone and CMD_DONE alone are both insufficient. */
          if (dw->host_bytes != length) return fail(dw, -EIO);
          if (got_cmd && !(rd(dw, STATUS) & DATA_BUSY))
            {
              dw->card_bytes = rd(dw, TCBCNT);
              if (dw->card_bytes != length ||
                  ((rd(dw, STATUS) >> 17) & 0x1fff)) return fail(dw, -EIO);
              wr(dw, RINTSTS, DTO);
              return 0;
            }
        }
      dw->io->delay_us(dw->ctx, 5);
    }
  while (!expired(dw, start, 2000000, ++polls));
  return fail(dw, -ETIMEDOUT);
}
