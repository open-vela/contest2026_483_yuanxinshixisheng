/* SPDX-License-Identifier: GPL-2.0-only */
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include "live.h"

#define CRU ((uintptr_t)0x27200000)
#define I2C ((uintptr_t)0x2ac60000)
#define GPIO4 ((uintptr_t)0x2ae40000)
#define IOC ((uintptr_t)0x2604408c)
static pthread_mutex_t owner = PTHREAD_MUTEX_INITIALIZER;
static bool bus_locked;
static struct i3_live bus;

static uint32_t rd(void *ctx, uintptr_t a)
{
  (void)ctx;
  return *(volatile uint32_t *)a;
}

static void wr(void *ctx, uintptr_t a, uint32_t v)
{
  (void)ctx;
  *(volatile uint32_t *)a = v;
  __asm__ volatile("dsb sy" ::: "memory");
}

static void masked(uintptr_t a, uint32_t mask, uint32_t value)
{
  wr(NULL, a, (mask << 16) | (value & mask));
}

static uint64_t now_us(void *ctx)
{
  struct timespec t = {0};
  (void)ctx;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return UINT64_MAX;
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* The process-wide owner excludes simultaneous commands; the bus callback
 * catches accidental reentrant use while a transaction owns the controller. */
static int lock_bus(void *ctx)
{
  (void)ctx;
  if (bus_locked) return -EBUSY;
  bus_locked = true;
  return 0;
}

static void unlock_bus(void *ctx)
{
  (void)ctx;
  bus_locked = false;
}

static int lines(void *ctx)
{
  (void)ctx;
  int rc = i3_gpio_lines(rd(NULL, GPIO4 + 0x78), rd(NULL, GPIO4 + 0x70));
  if (rc != 1) return rc;
  up_udelay(5);
  return i3_gpio_lines(rd(NULL, GPIO4 + 0x78), rd(NULL, GPIO4 + 0x70));
}

struct saved_i2c
{
  uint32_t gate12, gate18, mux, source, con, divider, con1;
  bool controller_saved, changed, switched;
};

static int prepare_i2c(struct saved_i2c *old)
{
  uint32_t reset = rd(NULL, CRU + 0xa30);
  uint32_t gate11 = rd(NULL, CRU + 0x82c);
  uint32_t parent = (rd(NULL, CRU + 0x3dc) >> 2) & 3;
  uint32_t gate0 = rd(NULL, CRU + 0x800);
  old->gate12 = rd(NULL, CRU + 0x830);
  old->gate18 = rd(NULL, CRU + 0x848);
  old->mux = rd(NULL, IOC);
  old->source = rd(NULL, CRU + 0x3e4);
  printf("AUDIO_IO reset12=%08" PRIx32 " gate11=%08" PRIx32
         " gate12=%08" PRIx32 " gate18=%08" PRIx32
         " mux=%08" PRIx32 " source=%08" PRIx32 "\n",
         reset, gate11, old->gate12, old->gate18, old->mux, old->source);
  /* Existing BSP owns the shared PCLK root. Do not reset an unknown live bus. */
  if (parent == 3 || (parent == 0 && (gate0 & 2)) ||
      (parent == 1 && (gate0 & 1)) ||
      (reset & 0x4004) || (rd(NULL, CRU + 0xa48) & 0x60) || (gate11 & 2) ||
      ((old->mux & 0xff) != 0 && (old->mux & 0xff) != 0xbb))
    return -EBUSY;
  /* No I2C3 IRQ handler is installed in this profile. Keep INTID123 masked
   * while vendor IEN bits are used for polling latched status. */
  up_disable_irq(123);
  masked(CRU + 0x830, 4, 0);
  masked(CRU + 0x848, 0x20, 0);
  old->changed = true;
  up_udelay(10);
  if ((rd(NULL, CRU + 0x830) & 4) || (rd(NULL, CRU + 0x848) & 0x20)) return -EIO;
  old->con = rd(NULL, I2C);
  old->divider = rd(NULL, I2C + 4);
  printf("AUDIO_IO con=%08" PRIx32 " ien=%08" PRIx32
         " gpio_version=%08" PRIx32 " gpio_input=%08" PRIx32 "\n",
         old->con, rd(NULL, I2C + 0x18), rd(NULL, GPIO4 + 0x78), rd(NULL, GPIO4 + 0x70));
  if ((old->con & 0x19) || rd(NULL, I2C + 0x18)) return -EBUSY;
  if (((old->con >> 16) & 0x1ff) >= 5) old->con1 = rd(NULL, I2C + 0x228);
  old->controller_saved = true;
  masked(CRU + 0x830, 0x4000, 0x4000);
  masked(CRU + 0x3e4, 0x30, 0x30);
  masked(CRU + 0x830, 0x4000, 0);
  masked(IOC, 0xff, 0xbb);
  old->switched = true;
  up_udelay(20);
  if ((rd(NULL, CRU + 0x3e4) & 0x30) != 0x30 ||
      (rd(NULL, CRU + 0x830) & 0x4004) ||
      (rd(NULL, IOC) & 0xff) != 0xbb || lines(NULL) != 1) return -EIO;
  struct i3_port p = {NULL, rd, wr, now_us, lock_bus, unlock_bus, lines};
  int rc = i3_init(&bus, &p, true);
  return rc ? rc : i3_timing(&bus);
}

static int restore_i2c(const struct saved_i2c *old)
{
  if (!old->changed) return 0;
  /* Never change pins/clocks beneath a transaction whose STOP is unproven. */
  if (bus.active || bus.held) return -EBUSY;
  if ((rd(NULL, CRU + 0x830) & 4) || (rd(NULL, CRU + 0x848) & 0x20)) return -EIO;
  if (old->controller_saved)
    {
      uint32_t expected_mux = old->switched ? 0xbb : (old->mux & 0xff);
      uint32_t expected_src = old->switched ? 0x30 : (old->source & 0x30);
      if ((rd(NULL, IOC) & 0xff) != expected_mux ||
          (rd(NULL, CRU + 0x3e4) & 0x30) != expected_src ||
          rd(NULL, I2C + 0x18) || (rd(NULL, I2C) & 0x19)) return -EBUSY;
      wr(NULL, I2C, old->con & 0xff00);
      wr(NULL, I2C + 4, old->divider);
      if (((old->con >> 16) & 0x1ff) >= 5) wr(NULL, I2C + 0x228, old->con1);
      if ((rd(NULL, I2C) & 0xff00) != (old->con & 0xff00) ||
          rd(NULL, I2C + 4) != old->divider ||
          (((old->con >> 16) & 0x1ff) >= 5 && rd(NULL, I2C + 0x228) != old->con1)) return -EIO;
      masked(IOC, 0xff, old->mux);
      masked(CRU + 0x830, 0x4000, 0x4000);
      masked(CRU + 0x3e4, 0x30, old->source);
      masked(CRU + 0x830, 0x4000, old->gate12);
      if ((rd(NULL, IOC) & 0xff) != (old->mux & 0xff) ||
          (rd(NULL, CRU + 0x3e4) & 0x30) != (old->source & 0x30) ||
          (rd(NULL, CRU + 0x830) & 0x4000) != (old->gate12 & 0x4000)) return -EIO;
    }
  masked(CRU + 0x830, 4, old->gate12);
  masked(CRU + 0x848, 0x20, old->gate18);
  return ((rd(NULL, CRU + 0x830) & 4) != (old->gate12 & 4) ||
          (rd(NULL, CRU + 0x848) & 0x20) != (old->gate18 & 0x20)) ? -EIO : 0;
}

int main(int argc, char **argv)
{
  struct saved_i2c old = {0};
  int rc, cleanup;
  if (argc != 2 || strcmp(argv[1], "codec-probe"))
    {
      puts("usage: k7audiohw codec-probe");
      return 1;
    }
  if (pthread_mutex_trylock(&owner)) return 1;
  /* Faulted contexts remain faulted; recovery is explicit, not a blind retry. */
  if (bus.poisoned || bus.held)
    {
      puts("AUDIO_IO fault_latched=1");
      pthread_mutex_unlock(&owner);
      return 1;
    }
  memset(&bus, 0, sizeof(bus));
  rc = prepare_i2c(&old);
  for (unsigned pass = 0; !rc && pass < 2; pass++)
    for (uint8_t reg = 0; !rc && reg < 3; reg++)
      {
        uint8_t value = 0;
        rc = i3_xfer(&bus, true, reg, &value, 100000);
        printf("AUDIO_IO codec_read pass=%u reg=%02x value=%02x result=%d\n",
               pass, reg, value, rc);
      }
  cleanup = restore_i2c(&old);
  if (cleanup) bus.poisoned = true;
  printf("AUDIO_IO result=%d restore=%d poisoned=%u held=%u active=%u "
         "codec_writes=0 playback=0 capture=0\n",
         rc, cleanup, bus.poisoned, bus.held, bus.active);
  pthread_mutex_unlock(&owner);
  return rc || cleanup ? 1 : 0;
}
