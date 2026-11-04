/* SPDX-License-Identifier: GPL-2.0-only */
/* Explicitly bound prototype; no board init or application auto-start hook. */
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "skw_rk3576.h"

/* Wireless SDMMC1 only. The eMMC controller is NOT an API parameter. */
#define SDMMC1_BASE UINT64_C(0x2a320000)
static uint32_t mmio_read(void *ctx, unsigned int offset)
{
  uint32_t value;
  (void)ctx;
  __asm__ volatile("dmb sy" ::: "memory");
  value = *(volatile uint32_t *)(uintptr_t)(SDMMC1_BASE + offset);
  __asm__ volatile("dmb sy" ::: "memory");
  return value;
}
static void mmio_write(void *ctx, unsigned int offset, uint32_t value)
{
  (void)ctx;
  __asm__ volatile("dmb sy" ::: "memory");
  *(volatile uint32_t *)(uintptr_t)(SDMMC1_BASE + offset) = value;
  __asm__ volatile("dsb sy" ::: "memory");
}
static uint64_t monotonic_us(void *ctx)
{
  struct timespec now;
  (void)ctx;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
  return (uint64_t)now.tv_sec * UINT64_C(1000000) +
         (uint64_t)now.tv_nsec / UINT64_C(1000);
}
static void delay_us(void *ctx, unsigned int us)
{
  (void)ctx;
  up_udelay(us);
}
static const struct skw_dw_io io =
  { mmio_read, mmio_write, monotonic_us, delay_us };
static int bus_lock(void *ctx)
{
  return nxmutex_lock(&((struct skw_rk3576 *)ctx)->mutex);
}
static void bus_unlock(void *ctx)
{
  nxmutex_unlock(&((struct skw_rk3576 *)ctx)->mutex);
}
static int cmd52(void *ctx, uint32_t arg, uint32_t *r5)
{
  return skw_dw_cmd52(&((struct skw_rk3576 *)ctx)->dw, arg, r5);
}
static int cmd53(void *ctx, uint32_t arg, void *buf, size_t len, uint32_t *r5)
{
  return skw_dw_cmd53(&((struct skw_rk3576 *)ctx)->dw, arg, buf, len, r5);
}
static const struct skw_bus_ops bus_ops =
  { bus_lock, bus_unlock, cmd52, cmd53 };

int skw_rk3576_bind_selected(struct skw_rk3576 *host, struct skw_bus *bus,
                             uint16_t vendor, uint16_t device)
{
  uint32_t enabled = 0, ready = 0, low = 0, high = 0;
  int ret;
  if (!host || !bus) return -EINVAL;
  memset(bus, 0, sizeof(*bus));
  if (!((vendor == 0x1ffe && device == 0x6621) ||
        (vendor == 0x3607 && device == 0x6160))) return -ENODEV;
  memset(host, 0, sizeof(*host));
  ret = nxmutex_init(&host->mutex);
  if (ret) return ret;
  ret = skw_dw_prepare(&host->dw, &io, NULL);
  if (!ret) ret = skw_dw_cmd52(&host->dw, 0x002u << 9, &enabled);
  if (!ret) ret = skw_dw_cmd52(&host->dw, 0x003u << 9, &ready);
  if (!ret && (!(enabled & 2u) || !(ready & 2u))) ret = -EHOSTDOWN;
  if (!ret) ret = skw_dw_cmd52(&host->dw, 0x110u << 9, &low);
  if (!ret) ret = skw_dw_cmd52(&host->dw, 0x111u << 9, &high);
  if (!ret && (((low & 0xffu) | ((high & 0xffu) << 8)) != 512u))
    ret = -ENOTSUP;
  if (!ret) ret = skw_bus_attach(bus, &bus_ops, host, vendor, device);
  if (ret)
    {
      host->dw.prepared = false;
      nxmutex_destroy(&host->mutex);
    }
  return ret;
}
void skw_rk3576_unbind(struct skw_rk3576 *host, struct skw_bus *bus)
{
  if (!host || !bus || !bus->attached || bus->ctx != host) return;
  bus->attached = false;
  bus->lite_verified = false;
  host->dw.prepared = false;
  nxmutex_destroy(&host->mutex);
}
