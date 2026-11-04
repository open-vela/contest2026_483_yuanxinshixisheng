/* SPDX-License-Identifier: GPL-2.0-only */
/* Derived protocol: Seekwave Tech Inc. skw_sdio_main.c / skw_boot.c.
 * Original driver copyright (C) 2022 Seekwave Tech Inc.; GPL version 2.
 * This port has no Linux, MMIO, GPIO, filesystem or firmware-start calls.
 */
#include "skw_native.h"
#include <errno.h>
#include <string.h>

int skw_r5_status(uint32_t response)
{
  if (response & 0x8000u) return -EILSEQ;
  if (response & 0x4000u) return -ENOTSUP;
  if (response & 0x0800u) return -EIO;
  if (response & 0x0200u) return -EINVAL;
  if (response & 0x0100u) return -ERANGE;
  return 0;                     /* Bits 13:12 are current state, not errors. */
}

int skw_bus_attach(struct skw_bus *bus, const struct skw_bus_ops *ops,
                   void *ctx, uint16_t vendor, uint16_t device)
{
  if (!bus) return -EINVAL;
  memset(bus, 0, sizeof(*bus));
  if (!ops || !ops->lock || !ops->unlock || !ops->cmd52 || !ops->cmd53)
    return -EINVAL;
  if (!((vendor == 0x1ffe && device == 0x6621) ||
        (vendor == 0x3607 && device == 0x6160))) return -ENODEV;
  bus->ops = ops;
  bus->ctx = ctx;
  bus->attached = true;
  return 0;
}

static int result(int ret, uint32_t response)
{
  if (ret) return ret < 0 ? ret : -EIO;
  return skw_r5_status(response);
}

static int transfer(struct skw_bus *bus, bool write, uint32_t address,
                     void *buffer, size_t length, size_t *completed)
{
  uint8_t *data = buffer;
  size_t done = 0;
  unsigned int i;
  int ret;

  if (!completed) return -EINVAL;
  *completed = 0;
  if (!bus || !buffer || length == 0) return -EINVAL;
  if (length > UINT32_MAX || length - 1 > UINT32_MAX - address)
    return -ERANGE;
  if (!bus->attached) return -ENODEV;
  ret = bus->ops->lock(bus->ctx);
  if (ret) return ret < 0 ? ret : -EIO;
  if (bus->faulted) { ret = -EIO; goto out; }
  if (write && !bus->lite_verified) { ret = -ENODEV; goto out; }

  /* F0 0x15c..0x15f is the little-endian target address window.
   * Keep it atomic with every following F1 data transfer.
   */
  for (i = 0; i < 4; i++)
    {
      uint32_t r5 = 0;
      uint32_t arg = 0x80000000u | ((0x15cu + i) << 9) |
                     ((address >> (8 * i)) & 0xffu);
      ret = bus->ops->cmd52(bus->ctx, arg, &r5);
      ret = result(ret, r5);
      if (ret) goto failed;
    }

  while (done < length)
    {
      uint32_t r5 = 0;
      size_t count = length - done;
      uint32_t arg;
      if (count > SKW_BLOCK_SIZE) count = SKW_BLOCK_SIZE;
      /* Vendor dt_read/write: incrementing CMD53 F1 port 0x0f for EACH
       * chunk. Its indirect target auto-advances. Do not add done to 0x0f.
       * Byte mode count 0 encodes exactly 512 bytes, never an empty write.
       */
      arg = (write ? 0x80000000u : 0) | 0x14000000u |
            (0x0fu << 9) | ((uint32_t)count & 0x1ffu);
      ret = bus->ops->cmd53(bus->ctx, arg, data + done, count, &r5);
      ret = result(ret, r5);
      if (ret) goto failed;
      done += count;
      *completed = done;
    }
  ret = 0;
  goto out;

failed:
  /* A failed data command may have partially reached the module. No blind
   * retry and no boot/start command is issued. completed excludes that chunk.
   */
  bus->faulted = true;
  bus->lite_verified = false;
out:
  bus->ops->unlock(bus->ctx);
  return ret;
}

int skw_mem_read(struct skw_bus *bus, uint32_t address, void *buffer,
                 size_t length, size_t *completed)
{
  return transfer(bus, false, address, buffer, length, completed);
}

int skw_mem_write(struct skw_bus *bus, uint32_t address, const void *buffer,
                  size_t length, size_t *completed)
{
  return transfer(bus, true, address, (void *)buffer, length, completed);
}

int skw_probe_lite(struct skw_bus *bus, uint8_t chip_id[16])
{
  size_t completed;
  uint8_t value[16];
  int ret;
  if (!bus || !chip_id) return -EINVAL;
  /* Probe is a lifecycle operation; owner must exclude concurrent users. */
  bus->lite_verified = false;
  memset(chip_id, 0, 16);
  ret = skw_mem_read(bus, 0x40000000u, value, sizeof(value), &completed);
  if (ret) return ret;
  memcpy(chip_id, value, sizeof(value));
  if (memcmp(value, "SV6160LITE", 10) != 0) return -ENODEV;
  bus->lite_verified = true;
  return 0;
}

uint16_t skw_crc16(uint16_t crc, const void *buffer, size_t length)
{
  const uint8_t *bytes = buffer;
  size_t i;
  unsigned int bit;
  /* CRC-16/XMODEM: polynomial 0x1021, initial 0, non-reflected. */
  for (i = 0; i < length; i++)
    {
      crc = (uint16_t)(crc ^ ((uint32_t)bytes[i] << 8));
      for (bit = 0; bit < 8; bit++)
        crc = (uint16_t)(((uint32_t)crc << 1) ^
                        ((crc & 0x8000u) ? 0x1021u : 0u));
    }
  return crc;
}

int skw_stage_region(struct skw_bus *bus, uint32_t address,
                       const void *data, size_t length, uint16_t expected_crc,
                       size_t *completed)
{
  if (!completed) return -EINVAL;
  *completed = 0;
  if (!data || length == 0) return -EINVAL;
  if (length > UINT32_MAX || length - 1 > UINT32_MAX - address)
    return -ERANGE;
  if (skw_crc16(0, data, length) != expected_crc) return -EBADMSG;
  return skw_mem_write(bus, address, data, length, completed);
}
