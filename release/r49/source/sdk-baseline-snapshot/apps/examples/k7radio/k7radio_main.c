/* SPDX-License-Identifier: Apache-2.0 */
/* K7 native wireless discovery, explicit isolated RAM experiment.
 * Register facts: pinned project Rockchip clk-rk3576.c, pinctrl-rockchip.c,
 * pm_domains.c and K7 DTB. SD commands: SDIO CCCR/CIS, DW-MSHC interface.
 * This file never addresses eMMC (2a330000), UART6, camera or NPU.
 */
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "skw_rk3576.h"
#include "skw_wifi_diag.h"
#include "skw_bt.h"
#include "skw_wifi_bss.h"
#include "skw_wifi_join.h"
#include "skw_wpa_transport.h"
#include "radio_firmware.h"
#include "k7_radio_service.h"
#include <sys/socket.h>
#include <netpacket/bluetooth.h>
#include <sys/ioctl.h>
#include <nuttx/wireless/bluetooth/bt_ioctl.h>
#include <nuttx/net/bluetooth.h>
#include <nuttx/kthread.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <unistd.h>
#include <sched.h>

#define BIT(n) (UINT32_C(1) << (n))
#define CRU UINT32_C(0x27200000)
#define PMU UINT32_C(0x27380000)
#define IOC UINT32_C(0x26040000)
#define GPIO1 UINT32_C(0x2ae10000)
#define SDIO UINT32_C(0x2a320000)
#define I2C2 UINT32_C(0x2ac50000)
#define I2C1 UINT32_C(0x2ac40000)
#define GATE(n) (CRU + 0x800 + (n) * 4)
#define RESET(n) (CRU + 0xa00 + (n) * 4)
#define CLKSEL(n) (CRU + 0x300 + (n) * 4)
#define CMD_START BIT(31)
#define CMD_WAIT BIT(13)
#define CMD_RESP BIT(6)
#define CMD_CRC BIT(8)
#define CMD_ERRORS (BIT(1) | BIT(6) | BIT(8) | BIT(12))

static volatile int g_session;
static uint16_t g_cis_vendor, g_cis_device;

static uint32_t rd(uintptr_t a)
{
  return *(volatile const uint32_t *)a;
}

static void wr(uintptr_t a, uint32_t v)
{
  *(volatile uint32_t *)a = v;
  __asm__ __volatile__("dsb sy" ::: "memory");
}

static void masked(uintptr_t a, uint32_t mask, uint32_t v)
{
  wr(a, (mask << 16) | (v & mask));
}

static uint64_t now_us(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int wait_mask(uintptr_t a, uint32_t mask, uint32_t value)
{
  uint64_t end = now_us() + 100000;
  do
    {
      if ((rd(a) & mask) == value) return 0;
      up_udelay(10);
    }
  while (now_us() < end);
  return -ETIMEDOUT;
}

/* K7 schematic: HYM8563 at I2C2 address 0x51 supplies module LPO.
 * This diagnostic reads only CTL1 and CLKOUT, never changes RTC registers.
 * Controller procedure: RK3576 TRM 32.3.2 / 32.4.3. No bus scanning.
 */
static int i2c_wait(uintptr_t base, uint32_t bit)
{
  uint64_t end = now_us() + 100000;
  do
    {
      uint32_t pending = rd(base + 0x1c);
      if (pending & BIT(6)) return -ENXIO;
      if (pending & bit) return 0;
      up_udelay(10);
    }
  while (now_us() < end);
  return -ETIMEDOUT;
}

static int i2c_read_byte(uintptr_t base, uint8_t address,
                         uint8_t reg, uint8_t *value)
{
  const uint32_t con = 0x300 | BIT(6) | BIT(5) | BIT(1) | BIT(0);
  int ret, stop;
  wr(base + 0x1c, 0x1ff);
  wr(base + 8, BIT(24) | ((uint32_t)address << 1));
  wr(base + 0xc, BIT(24) | reg);
  wr(base, con | BIT(3));
  ret = i2c_wait(base, BIT(4));
  if (!ret)
    {
      wr(base + 0x1c, BIT(4));
      wr(base, con);
      wr(base + 0x14, 1);
      ret = i2c_wait(base, BIT(3));
      if (!ret) *value = rd(base + 0x200);
    }
  if (ret) printf("I2C read reg=%02x ret=%d ipd=%08" PRIx32 "\n",
                  reg, ret, rd(base + 0x1c));
  wr(base + 0x1c, 0x1ff);
  wr(base, con | BIT(4));
  stop = i2c_wait(base, BIT(5));
  wr(base, 0);
  wr(base + 0x1c, 0x1ff);
  return ret ? ret : stop;
}

static int rtc_status(void)
{
#ifdef CONFIG_VELAVISION_LEGACY_RTC_STATUS_DISABLED
  puts("BLOCKED: I2C2 reserved for native cloud RTC owner");
  return -EBUSY;
#else
  uint32_t gates = rd(GATE(12));
  uint32_t source = rd(CLKSEL(57));
  uint32_t muxb = rd(IOC + 0x2000), muxc = rd(IOC + 0x2004);
  uint32_t con, divider;
  uint8_t ctl = 0, output = 0;
  int ret;
  masked(GATE(12), BIT(1) | BIT(13), 0);
  con = rd(I2C2);
  divider = rd(I2C2 + 4);
  if ((con & 1) || rd(I2C2 + 0x18))
    {
      masked(GATE(12), BIT(1) | BIT(13), gates);
      puts("BLOCKED: I2C2 controller already owned");
      return -EBUSY;
    }
  masked(CLKSEL(57), 0xc, 0xc); /* xin24m, divided below to 100kHz */
  masked(IOC + 0x2000, 0xf000, 0x9000); /* GPIO0 B7 SCL M0 */
  masked(IOC + 0x2004, 0xf, 9);        /* GPIO0 C0 SDA M0 */
  wr(I2C2 + 4, (14u << 16) | 14u);
  ret = i2c_read_byte(I2C2, 0x51, 0, &ctl);
  if (!ret) ret = i2c_read_byte(I2C2, 0x51, 0x0d, &output);
  wr(I2C2, con & 0xffff);
  wr(I2C2 + 4, divider);
  masked(IOC + 0x2000, 0xf000, muxb);
  masked(IOC + 0x2004, 0xf, muxc);
  masked(CLKSEL(57), 0xc, source);
  masked(GATE(12), BIT(1) | BIT(13), gates);
  printf("RTC status_ret=%d ctl1=%02x clkout=%02x enabled=%u rate_code=%u\n",
         ret, ctl, output, (output >> 7) & 1, output & 3);
  return ret;
#endif
}

/* Read-only RK806 identity and wireless supply configuration. There is no
 * PMIC write implementation: the combined I2C transaction writes only the
 * register index, then reads its value. Keep the U-Boot-established pins.
 */
static int pmic_status(void)
{
  static const uint8_t regs[] = {0x5a, 0x5b, 0x00, 0x01, 0x1d, 0x21};
  uint8_t value[sizeof(regs)] = {0};
  uint32_t gates = rd(GATE(12)), source = rd(CLKSEL(57));
  uint32_t con, divider;
  unsigned int i;
  int ret = 0;
  masked(GATE(12), BIT(0) | BIT(12), 0);
  con = rd(I2C1);
  divider = rd(I2C1 + 4);
  if ((con & 1) || rd(I2C1 + 0x18))
    {
      masked(GATE(12), BIT(0) | BIT(12), gates);
      puts("BLOCKED: I2C1 controller already owned");
      return -EBUSY;
    }
  masked(CLKSEL(57), 3, 3); /* known 24MHz source, 100kHz SCL */
  wr(I2C1 + 4, (14u << 16) | 14u);
  for (i = 0; i < sizeof(regs); i++)
    {
      ret = i2c_read_byte(I2C1, 0x23, regs[i], &value[i]);
      if (ret) break;
      printf("PMIC read-only reg=%02x value=%02x\n", regs[i], value[i]);
      up_mdelay(20);
    }
  wr(I2C1, con & 0xffff);
  wr(I2C1 + 4, divider);
  masked(CLKSEL(57), 3, source);
  masked(GATE(12), BIT(0) | BIT(12), gates);
  printf("PMIC status_ret=%d device_writes=0\n", ret);
  return ret;
}

static void status(void)
{
  printf("RADIO PMU repair=%08" PRIx32 " idle=%08" PRIx32
         " ack=%08" PRIx32 "\n", rd(PMU + 0x570),
         rd(PMU + 0x128), rd(PMU + 0x120));
  printf("RADIO CRU gate42=%08" PRIx32 " reset42=%08" PRIx32
         " clksel104=%08" PRIx32 " gate17=%08" PRIx32 "\n",
         rd(GATE(42)), rd(RESET(42)), rd(CLKSEL(104)), rd(GATE(17)));
  printf("RADIO IOC Bhigh=%08" PRIx32 " Clow=%08" PRIx32
         " Chigh=%08" PRIx32 "\n", rd(IOC + 0x402c),
         rd(IOC + 0x4030), rd(IOC + 0x4034));
  printf("RADIO scope=SDMMC1-only firmware=0 wifi=0 bluetooth=0\n");
}

static int update_clock(void)
{
  wr(SDIO + 0x2c, CMD_START | CMD_WAIT | BIT(21));
  return wait_mask(SDIO + 0x2c, CMD_START, 0);
}

static void bus_status(void)
{
  uint32_t previous, changed = 0;
  unsigned int i, transitions = 0;
  printf("RADIO GPIO1 ver=%08" PRIx32 " drh=%08" PRIx32
         " ddrh=%08" PRIx32 " ext=%08" PRIx32 "\n",
         rd(GPIO1 + 0x78), rd(GPIO1 + 4), rd(GPIO1 + 0xc), rd(GPIO1 + 0x70));
  up_mdelay(20);
  printf("RADIO SDIO ctrl=%08" PRIx32 " clkdiv=%08" PRIx32
         " clkena=%08" PRIx32 " uhs=%08" PRIx32 "\n",
         rd(SDIO), rd(SDIO + 8), rd(SDIO + 0x10), rd(SDIO + 0x74));
  up_mdelay(20);
  printf("RADIO SDIO usrid=%08" PRIx32 " phase_out=%08" PRIx32
         " phase_in=%08" PRIx32 "\n", rd(SDIO + 0x68),
         rd(SDIO + 0x130), rd(SDIO + 0x134));
  up_mdelay(20);
  printf("RADIO PAD ieB=%08" PRIx32 " ieC=%08" PRIx32
         " pullB=%08" PRIx32 " pullC=%08" PRIx32 "\n",
         rd(IOC + 0x6184), rd(IOC + 0x6188),
         rd(IOC + 0x6114), rd(IOC + 0x6118));
  up_mdelay(20);
  printf("RADIO PAD driveBhigh=%08" PRIx32 " driveClow=%08" PRIx32
         " expected_masked=2222/22\n",
         rd(IOC + 0x602c), rd(IOC + 0x6030));
  up_mdelay(20);
  previous = rd(GPIO1 + 0x70);
  for (i = 0; i < 1000; i++)
    {
      uint32_t pins = rd(GPIO1 + 0x70);
      uint32_t diff = (pins ^ previous) & 0x0003f000;
      changed |= diff;
      if (diff & BIT(17)) transitions++;
      previous = pins;
      up_udelay(3);
    }
  printf("RADIO PAD observed_changes=%08" PRIx32
         " clock_changes=%u (sampled, not frequency measurement)\n",
         changed, transitions);
  up_mdelay(20);
}

static int command(unsigned int index, uint32_t arg, uint32_t flags,
                   uint32_t *response)
{
  uint64_t end;
  uint32_t raw;
  uint32_t previous = rd(GPIO1 + 0x70);
  unsigned int samples = 0, cmd_changes = 0, clk_changes = 0;
  int trace = index == 0 || index == 5 ||
              (index == 52 && ((arg >> 9) & 0x1ffff) == 6);
  int ret = wait_mask(SDIO + 0x2c, CMD_START, 0);
  if (ret) return ret;
  wr(SDIO + 0x44, 0xffffffff);
  wr(SDIO + 0x28, arg);
  /* Vendor DW command preparation uses the output hold register. CMD0
   * is STOP/ABORT; non-data CMD5/3/7/52 do not wait on the data FSM.
   */
  wr(SDIO + 0x2c, CMD_START | BIT(29) | flags | index |
                    (index == 0 ? BIT(14) : 0));
  end = now_us() + 200000;
  do
    {
      if (trace)
        {
          uint32_t pins = rd(GPIO1 + 0x70);
          uint32_t diff = pins ^ previous;
          cmd_changes += !!(diff & BIT(16));
          clk_changes += !!(diff & BIT(17));
          previous = pins;
          samples++;
        }
      raw = rd(SDIO + 0x44);
      if (trace && (raw & (CMD_ERRORS | BIT(2))))
        {
          printf("SDIO CMD%u pad_samples=%u cmd_changes=%u clk_changes=%u\n",
                 index, samples, cmd_changes, clk_changes);
          up_mdelay(20); /* preserve paced UART evidence */
        }
      if (raw & CMD_ERRORS)
        {
          printf("SDIO CMD%u arg=%08" PRIx32 " irq=%08" PRIx32
                 " status=%08" PRIx32 "\n", index, arg, raw,
                 rd(SDIO + 0x48));
          return -EIO;
        }
      if (raw & BIT(2))
        {
          if (response) *response = rd(SDIO + 0x30);
          wr(SDIO + 0x44, raw);
          return 0;
        }
      up_udelay(trace ? 1 : 10);
    }
  while (now_us() < end);
  printf("SDIO CMD%u timeout irq=%08" PRIx32 " cmd=%08" PRIx32 "\n",
         index, rd(SDIO + 0x44), rd(SDIO + 0x2c));
  return -ETIMEDOUT;
}

static int read_byte(uint32_t address, uint8_t *value)
{
  uint32_t response;
  int ret;
  if (address > 0x1ffff) return -ERANGE;
  ret = command(52, address << 9, CMD_RESP | CMD_CRC, &response);
  if (ret) return ret;
  if (response & 0xcb00) return -EIO; /* R5 errors, not current-state bits */
  *value = response;
  return 0;
}

static int identify_cis(uint32_t base)
{
  uint32_t ptr = 0;
  uint8_t value, code, size, id[4];
  unsigned int i, tuple;
  int ret;
  for (i = 0; i < 3; i++)
    {
      ret = read_byte(base + 9 + i, &value);
      if (ret) return ret;
      ptr |= (uint32_t)value << (i * 8);
    }
  printf("SDIO CIS base=%03" PRIx32 " ptr=%06" PRIx32 "\n", base, ptr);
  if (!ptr || ptr > 0x1ffff) return -EINVAL;
  for (tuple = 0; tuple < 64 && ptr < 0x1fffe; tuple++)
    {
      ret = read_byte(ptr++, &code);
      if (ret) return ret;
      if (code == 0xff) return -ENOENT;
      if (code == 0) continue;
      ret = read_byte(ptr++, &size);
      if (ret) return ret;
      if (size == 0xff || ptr + size > 0x20000) return -EINVAL;
      if (code == 0x20 && size >= 4)
        {
          for (i = 0; i < 4; i++)
            {
              ret = read_byte(ptr + i, &id[i]);
              if (ret) return ret;
            }
          g_cis_vendor = id[0] | id[1] << 8;
          g_cis_device = id[2] | id[3] << 8;
          printf("SDIO ID vendor=%04x device=%04x\n",
                 id[0] | id[1] << 8, id[2] | id[3] << 8);
          return 0;
        }
      ptr += size;
    }
  return -EOVERFLOW;
}

static int gpio_clock_bit(void)
{
  int value;
  masked(GPIO1 + 4, BIT(1), 0);
  up_udelay(10);
  masked(GPIO1 + 4, BIT(1), BIT(1));
  up_udelay(10);
  value = (rd(GPIO1 + 0x70) >> 16) & 1;
  masked(GPIO1 + 4, BIT(1), 0);
  return value;
}

static void gpio_send_cmd(unsigned int index)
{
  uint8_t frame[6] = {0x40 | index, 0, 0, 0, 0, 0};
  uint8_t crc = 0;
  unsigned int i, bit;
  for (i = 0; i < 5; i++)
    for (bit = 0; bit < 8; bit++)
      {
        unsigned int feedback = ((frame[i] >> (7 - bit)) ^ (crc >> 6)) & 1;
        crc = (crc << 1) & 0x7f;
        if (feedback) crc ^= 0x09;
      }
  frame[5] = (crc << 1) | 1;
  masked(GPIO1 + 0xc, BIT(0), BIT(0));
  for (i = 0; i < sizeof(frame); i++)
    for (bit = 0; bit < 8; bit++)
      {
        masked(GPIO1 + 4, BIT(0), (frame[i] >> (7 - bit)) & 1);
        gpio_clock_bit();
      }
  /* Release CMD while CLK is low before the response turnaround. */
  masked(GPIO1 + 0xc, BIT(0), 0);
}

static int gpio_cmd5(void)
{
  uint8_t response[6] = {0};
  unsigned int i;
  int ret = -ETIMEDOUT;
  for (i = 0; i < 80; i++) gpio_clock_bit();
  gpio_send_cmd(0);
  for (i = 0; i < 16; i++) gpio_clock_bit();
  gpio_send_cmd(5);
  for (i = 0; i < 256; i++)
    if (!gpio_clock_bit()) break;
  if (i < 256)
    {
      /* First sampled start bit is zero; collect remaining 47 bits. */
      for (i = 1; i < 48; i++)
        response[i / 8] |= gpio_clock_bit() << (7 - i % 8);
      ret = response[0] == 0x3f && response[5] == 0xff ? 0 : -EPROTO;
    }
  for (i = 0; i < 8; i++) gpio_clock_bit();
  printf("SDIO GPIO CMD5 ret=%d response=%02x %02x %02x %02x %02x %02x\n",
         ret, response[0], response[1], response[2], response[3],
         response[4], response[5]);
  /* End with module disabled, CLK low and CMD released. No active bus. */
  masked(GPIO1 + 4, BIT(6), 0);
  return ret;
}

static int sdio_identify(int gpio_mode, int slow_mode)
{
  uint32_t response, ocr;
  uint8_t cccr, caps;
  unsigned int tries;
  int ret;
  status();
  /* Do not touch a powered-off/isolated peripheral or change shared domains. */
  if (!(rd(PMU + 0x570) & BIT(7)) ||
      ((rd(PMU + 0x128) | rd(PMU + 0x120)) & BIT(17)))
    {
      puts("BLOCKED: SDGMAC power/bus prerequisites; no SDIO access");
      return -EHOSTDOWN;
    }
  if (__sync_lock_test_and_set(&g_session, 1))
    {
      puts("BLOCKED: isolated radio session consumed; RAM reboot required");
      return -EBUSY;
    }
  /* Only target resets and leaf clocks; keep DDR/PLLs/other buses unchanged. */
  masked(GATE(17), BIT(15), 0);
  masked(IOC + 0x4034, 0x0f00, 0); /* GPIO1_C6 wireless REG_ON */
  masked(GPIO1 + 4, BIT(6), 0);     /* v2 SWPORT_DR_H */
  masked(GPIO1 + 0xc, BIT(6), BIT(6)); /* v2 SWPORT_DDR_H */
  /* Live K7 comparison: disabling UART4 prevents SDIO enumeration;
   * keeping UART4 disabled but biasing its C4/C5 pins high restores it.
   * Keep these auxiliary module pins as GPIO inputs during reset. No UART
   * transmission or UART4 clock is needed. The exact chip strap meanings
   * are not documented here; this is the experimentally verified pin state.
   */
  masked(GPIO1 + 0xc, BIT(4) | BIT(5), 0);
  masked(IOC + 0x4034, 0x00ff, 0);
  masked(IOC + 0x6118, 0x3f00, 0x3f00); /* C4/C5 and REG_ON C6 pull-up */
  if ((rd(GPIO1 + 0xc) & (BIT(4) | BIT(5))) ||
      (rd(IOC + 0x4034) & 0x00ff) ||
      (rd(IOC + 0x6118) & 0x3f00) != 0x3f00)
    {
      puts("BLOCKED: auxiliary radio pin setup failed");
      return -EIO;
    }
  printf("RADIO id13 C4/C5 GPIO inputs pull-up; C6 pull-up, pullC=%08" PRIx32
         " ext=%08" PRIx32 "\n", rd(IOC + 0x6118), rd(GPIO1 + 0x70));
  up_mdelay(200); /* board vendor power-cycle interval */
  masked(RESET(42), BIT(12), BIT(12));
  masked(GATE(42), BIT(0) | BIT(11) | BIT(12), 0);
  /* Rockchip dw_mmc uses the CRU divider, not arbitrary DW CLKDIV values.
   * xin24m / 30 / fixed CLKGEN 2 = 400kHz; DW CLKDIV must be 0 here.
   */
  masked(CLKSEL(104), 0xff, (2u << 6) | (slow_mode ? 59u : 29u));
  masked(IOC + 0x402c, 0xffff, 0x2222); /* GPIO1_B4..7 D0..3 */
  masked(IOC + 0x4030, 0xff, 0x22);     /* GPIO1_C0 CMD, C1 CLK */
  masked(IOC + 0x6114, 0xff00, 0xff00); /* GPIO1_B4..7 pull-up */
  masked(IOC + 0x6118, 0xf, 0xf);      /* GPIO1_C0..1 pull-up per DT */
  /* Saved K7 DT: SDMMC1 pins use drive-strength level 2. RK3576 TRM
   * VCCIO_IOC encodes level 2 as 010 (50 ohm), not the reset 110.
   * Write only B4..7 and C0..1; leave C2/C3 and reserved bits alone.
   */
  masked(IOC + 0x602c, 0x7777, 0x2222);
  masked(IOC + 0x6030, 0x0077, 0x0022);
  up_udelay(10);
  masked(RESET(42), BIT(12), 0);
  up_udelay(10);
  if ((rd(GATE(42)) & (BIT(0) | BIT(11) | BIT(12))) ||
      (rd(CLKSEL(104)) & 0xff) != (slow_mode ? 0xbb : 0x9d) ||
      (rd(IOC + 0x402c) & 0xffff) != 0x2222 ||
      (rd(IOC + 0x4030) & 0xff) != 0x22 ||
      (rd(IOC + 0x602c) & 0x7777) != 0x2222 ||
      (rd(IOC + 0x6030) & 0x0077) != 0x0022) return -EIO;
  printf("SDIO controller ver=%08" PRIx32 " hcon=%08" PRIx32 "\n",
         rd(SDIO + 0x6c), rd(SDIO + 0x70));
  wr(SDIO, 7); /* reset controller/FIFO/DMA, interrupts and DMA disabled */
  ret = wait_mask(SDIO, 7, 0);
  if (ret) return ret;
  wr(SDIO + 0x24, 0);
  wr(SDIO + 0x80, 0); /* bus mode: no IDMAC */
  wr(SDIO + 0x44, 0xffffffff);
  wr(SDIO + 4, 1);
  wr(SDIO + 0x10, 0);
  if ((ret = update_clock())) return ret;
  if (gpio_mode)
    {
      /* Independent low-speed command path on the same wireless pins.
       * CRU clock is running internally, but SDIO CLKENA remains zero.
       */
      masked(GPIO1 + 4, BIT(0) | BIT(1), BIT(0));
      masked(GPIO1 + 0xc, BIT(0) | BIT(1), BIT(0) | BIT(1));
      masked(IOC + 0x4030, 0xff, 0);
      masked(GPIO1 + 4, BIT(6), BIT(6));
      up_mdelay(200);
      return gpio_cmd5();
    }
  wr(SDIO + 8, 0); /* frequency set in CRU; never UHS/voltage switching */
  wr(SDIO + 0xc, 0);
  wr(SDIO + 0x18, 0); /* one-bit */
  wr(SDIO + 0x74, 0); /* SDR, no 1.8V request or DDR bit inherited */
  wr(SDIO + 0x14, 0xffffffff);
  if (rd(SDIO + 0x68) == 0x20230001)
    {
      /* TRM 39.6.6: initialize CLKGEN before changing either phase. */
      masked(SDIO + 0x130, BIT(0), BIT(0));
      up_udelay(10);
      masked(SDIO + 0x130, 0x0ffe, 2); /* vendor identification drive 90deg */
      masked(SDIO + 0x134, 0x0ffe, 0); /* default sample 0deg */
      masked(SDIO + 0x130, BIT(0), 0);
      up_udelay(10);
    }
  if ((ret = update_clock())) return ret;
  /* Keep SDCLK stopped while releasing module reset, matching the board
   * pwrseq post-power-on delay before the enumeration clock is applied.
   */
  masked(GPIO1 + 4, BIT(6), BIT(6));
  up_mdelay(200);
  wr(SDIO + 0x10, 1);
  if ((ret = update_clock())) return ret;
  up_mdelay(10);
  bus_status();
  printf("SDIO init_hz=%u (configured, not measured)\n",
         slow_mode ? 200000u : 400000u);
  up_mdelay(20);
  if (!(rd(GPIO1 + 4) & BIT(6)) || !(rd(GPIO1 + 0xc) & BIT(6)) ||
      !(rd(GPIO1 + 0x70) & BIT(22)))
    {
      puts("BLOCKED: wireless REG_ON output/readback not high");
      return -EIO;
    }
  /* Match Rockchip MMC core rescan: try the standard CCCR I/O reset
   * before GO_IDLE, even when the initial CCCR read has no response.
   * This writes only SDIO function 0 I/O_ABORT (0x06), never firmware.
   * Reset failure is diagnostic: still attempt fresh CMD0/CMD5 as Linux
   * does, since an uninitialized device may ignore the pre-reset access.
   */
  {
    uint32_t abort_value = 8, reset_response = 0;
    int read_ret, reset_ret;
    read_ret = command(52, 6u << 9, CMD_RESP | CMD_CRC | BIT(15),
                       &reset_response);
    if (!read_ret && !(reset_response & 0xcb00))
      abort_value |= reset_response & 0xff;
    reset_ret = command(52, BIT(31) | (6u << 9) | abort_value,
                        CMD_RESP | CMD_CRC, &reset_response);
    printf("SDIO pre-reset read_ret=%d write_ret=%d r5=%08" PRIx32 "\n",
           read_ret, reset_ret, reset_response);
  }
  up_mdelay(1);
  if ((ret = command(0, 0, BIT(15), NULL))) return ret;
  up_mdelay(2); /* mmc_go_idle: 1ms reset settling + 1ms CS release */
  if ((ret = command(5, 0, CMD_RESP, &response))) return ret;
  printf("SDIO OCR initial=%08" PRIx32 " functions=%u\n", response,
         (unsigned int)((response >> 28) & 7));
  if (!((response >> 28) & 7)) return -ENODEV;
  ocr = response & 0x00300000; /* only advertised 3.2..3.4V supply window */
  if (!ocr) return -ENOTSUP;
  for (tries = 0; tries < 100; tries++)
    {
      ret = command(5, ocr, CMD_RESP, &response);
      if (ret) return ret;
      if (response & BIT(31)) break;
      up_mdelay(10);
    }
  if (tries == 100) return -ETIMEDOUT;
  if ((ret = command(3, 0, CMD_RESP | CMD_CRC, &response))) return ret;
  if (!((response >> 16) & 0xffff) || (response & 0xe000)) return -EIO;
  printf("SDIO RCA=%04" PRIx32 "\n", response >> 16);
  if ((ret = command(7, response & 0xffff0000, CMD_RESP | CMD_CRC,
                     &response))) return ret;
  if (response & 0xfdffe008) return -EIO;
  if ((ret = read_byte(0, &cccr))) return ret;
  if ((ret = read_byte(8, &caps))) return ret;
  printf("SDIO CCCR=%02x caps=%02x\n", cccr, caps);
  ret = identify_cis(0);
  if (ret == -ENOENT || ret == -EINVAL) ret = identify_cis(0x100);
  return ret;
}


/* A single explicit lifecycle: live CIS, F1 ready, block size, then CMD53.
 * No vendor firmware memory writes or boot command are part of this probe.
 */
static int f0_write(uint32_t address, uint8_t value)
{
  uint32_t response;
  int ret = command(52, BIT(31) | (address << 9) | value,
                    CMD_RESP | CMD_CRC, &response);
  return ret ? ret : skw_r5_status(response);
}

static int sdio_chip_open(struct skw_rk3576 *host, struct skw_bus *bus)
{
  uint8_t value, chip[16] = {0};
  unsigned int tries, i;
  int ret = sdio_identify(0, 0);
  if (ret) return ret;
  if (!((g_cis_vendor == 0x1ffe && g_cis_device == 0x6621) ||
        (g_cis_vendor == 0x3607 && g_cis_device == 0x6160))) return -ENODEV;
  if ((ret = read_byte(2, &value))) return ret;
  if ((ret = f0_write(2, value | 2))) return ret;
  for (tries = 0; tries < 100; tries++)
    {
      if ((ret = read_byte(3, &value))) return ret;
      if (value & 2) break;
      up_mdelay(1);
    }
  if (tries == 100) return -ETIMEDOUT;
  if ((ret = f0_write(0x110, 0))) return ret;
  if ((ret = f0_write(0x111, 2))) return ret;
  /* Leave identification speed for data mode: default-speed 4-bit at
   * 12 MHz, within 25 MHz SD default mode. No 1.8 V or high-speed switch.
   */
  if ((ret = read_byte(7, &value))) return ret;
  if ((ret = f0_write(7, (value & ~3u) | 2u))) return ret;
  wr(SDIO + 0x18, 1);
  wr(SDIO + 0x10, 0);
  if ((ret = update_clock())) return ret;
  masked(CLKSEL(104), 0xff, 0x80); /* xin24m / 1 / fixed 2 */
  wr(SDIO + 0x10, 1);
  if ((ret = update_clock())) return ret;
  if ((ret = read_byte(7, &value))) return ret;
  if ((value & 3) != 2 || rd(SDIO + 0x18) != 1 ||
      (rd(CLKSEL(104)) & 0xff) != 0x80) return -EIO;
  printf("RADIO data mode 4-bit 12000000Hz configured CCCR07=%02x\n", value);
  ret = skw_rk3576_bind_selected(host, bus, g_cis_vendor, g_cis_device);
  printf("RADIO id36 F1 ready; bind_ret=%d\n", ret);
  if (ret) return ret;
  ret = skw_probe_lite(bus, chip);
  printf("RADIO CMD53 chip bytes:");
  for (i = 0; i < sizeof(chip); i++) printf(" %02x", chip[i]);
  printf("\nRADIO chip_probe_ret=%d lite_verified=%u host_bytes=%zu card_bytes=%" PRIu32
         " raw=%08" PRIx32 " status=%08" PRIx32 " cleanup=%d\n",
         ret, bus->lite_verified, host->dw.host_bytes, host->dw.card_bytes,
         host->dw.last_raw, host->dw.last_status, host->dw.cleanup_error);
  if (ret) skw_rk3576_unbind(host, bus);
  return ret;
}
static int sdio_chip_probe(void)
{
  struct skw_rk3576 host;
  struct skw_bus bus;
  int ret = sdio_chip_open(&host, &bus);
  if (!ret) skw_rk3576_unbind(&host, &bus);
  return ret;
}

/* Explicit RAM-only firmware diagnostic. SDK skw_sdio_main/rx.c protocol.
 * Firmware inputs have fixed SHA256 in the Windows loader; CRC and complete
 * byte readback below prevent startup after any transfer failure.
 */
static struct skw_rk3576 g_fw_host;
static struct skw_bus g_fw_bus;
static bool g_cp_ready, g_wifi_ready, g_bt_ready;
/* Ten SDIO2 slots plus trailer fit the existing 16 KiB PIO limit.
 * Vendor RX drains trailer-reported pending packets before polling IRQ again. */
#define FW_RX_BATCH_SLOTS 10u
static uint8_t g_rx[FW_RX_BATCH_SLOTS * SKW_RX_SLOT_SIZE + SKW_BLOCK_SIZE];
_Static_assert(sizeof(g_rx) <= 16384u, "RX batch exceeds PIO transfer limit");
static unsigned int g_rx_batches, g_rx_pending_high, g_rx_read_us_high;
static unsigned int g_rx_idle_sleeps, g_rx_budget_pending;
/* RX owner is the only writer; status readers use atomic snapshots. */
static void fw_rx_count(unsigned int *counter)
{
  unsigned int n = __atomic_load_n(counter, __ATOMIC_RELAXED);
  if (n != UINT32_MAX) __atomic_store_n(counter, n + 1, __ATOMIC_RELAXED);
}
static void fw_rx_high(unsigned int *counter, uint64_t value)
{
  unsigned int n = value > UINT32_MAX ? UINT32_MAX : (unsigned int)value;
  if (n > __atomic_load_n(counter, __ATOMIC_RELAXED))
    __atomic_store_n(counter, n, __ATOMIC_RELAXED);
}
static unsigned int g_pending;
static uint8_t g_fifo_ind;
static uint16_t g_hci_opcode, g_hci_revision;
static bool g_hci_done, g_bt_initialized, g_wifi_ack;
static int g_hci_status;
static unsigned int g_adv_reports;
static struct skw_bt g_native_bt;
static bool g_bt_host_mode, g_bt_worker_fault;
static unsigned int g_bt_host_rx, g_rx_empty;
static uint8_t g_hci_addr[6];
static uint16_t g_wifi_seq, g_wifi_status;
static uint8_t g_wifi_cmd, g_wifi_response[1532];
static size_t g_wifi_response_len;
static bool g_wifi_info_valid, g_wifi_calibrated, g_wifi_scan_done;
static uint8_t g_wifi_mac[6], g_wifi_seen[64][6];
static unsigned int g_wifi_reports, g_wifi_unique;
static mutex_t g_wifi_operation = NXMUTEX_INITIALIZER;
static mutex_t g_wifi_state = NXMUTEX_INITIALIZER;
static sem_t g_wifi_acksem = SEM_INITIALIZER(0), g_wifi_scansem = SEM_INITIALIZER(0);
static bool g_wifi_waiting;
static struct skw_bss g_wifi_target;
static bool g_wifi_target_valid;
/* Protected by g_wifi_state; selection changes only under g_wifi_operation. */
static uint8_t g_wifi_target_ssid[32]={'L','a','n','s','e','e'};
static size_t g_wifi_target_ssid_len=6;
static unsigned int g_wifi_data_packets;
#include "skw_eapol_queue.h"
static struct skw_eapol_queue g_wifi_eapol;
static bool g_wifi_pn_reuse;
static struct skw_wpa_transport g_wifi_wpa;
static struct skw_bss g_wifi_join_bss;
static bool g_wifi_auth_waiting,g_wifi_auth_received;
static uint16_t g_wifi_auth_status;
static uint8_t g_wifi_join_instance;
static sem_t g_wifi_authsem=SEM_INITIALIZER(0);
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
static bool g_wifi_network_active,g_wifi_network_stop,g_wifi_link_lost;
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
#include "radio_backend_state.inc"
#endif
static void fw_wifi_net_slot(const uint8_t *,size_t);
#endif
static void fw_wifi_mgmt_event(uint8_t instance,const uint8_t *p,size_t n);
static int prov_start(void);
#ifndef CONFIG_EXAMPLES_K7RADIO_SHARED
static void prov_scan_report(const uint8_t *p,size_t n);
#endif

static void fw_wifi_scan_report(const uint8_t *payload, size_t length)
{
  struct skw_scan_record rec;
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
  rb_rx_report(payload,length);
#else
  prov_scan_report(payload,length);
#endif
  int ret = skw_diag_scan(payload, length, &rec);
  if (ret) { printf("RADIO malformed Wi-Fi scan report ret=%d\n", ret); return; }
  if (nxmutex_lock(&g_wifi_state)) return;
  if (rec.ssid_len == g_wifi_target_ssid_len &&
      !memcmp(rec.ssid,g_wifi_target_ssid,rec.ssid_len) &&
      (!g_wifi_target_valid || rec.rssi > g_wifi_target.scan.rssi))
    {
      static struct skw_bss candidate;
      if (!skw_bss_decode(payload,length,&candidate))
        { memcpy(&g_wifi_target,&candidate,sizeof(candidate)); g_wifi_target_valid=true; }
    }
  g_wifi_reports++;
  for (unsigned int i = 0; i < g_wifi_unique; i++)
    if (!memcmp(g_wifi_seen[i], rec.bssid, 6)) { nxmutex_unlock(&g_wifi_state); return; }
  if (g_wifi_unique >= 64) { nxmutex_unlock(&g_wifi_state); return; }
  memcpy(g_wifi_seen[g_wifi_unique++], rec.bssid, 6);
  printf("RADIO Wi-Fi AP %u band=%u channel=%u RSSI=%d SSID=", g_wifi_unique, rec.band, rec.channel, rec.rssi);
  for (unsigned int i = 0; i < rec.ssid_len; i++)
    {
      unsigned int c = rec.ssid[i];
      if (c >= 32 && c <= 126 && c != 92) putchar(c);
      else printf("\\x%02x", c);
    }
  putchar('\n');
  nxmutex_unlock(&g_wifi_state);
}

static int fw_f0(bool write, uint32_t address, uint8_t *value)
{
  struct skw_bus *bus = &g_fw_bus;
  uint32_t r5 = 0;
  int ret;
  if (!bus->attached || !bus->lite_verified) return -ENODEV;
  ret = bus->ops->lock(bus->ctx);
  if (ret) return ret;
  if (bus->faulted) { bus->ops->unlock(bus->ctx); return -ENODEV; }
  ret = bus->ops->cmd52(bus->ctx, (write ? BIT(31) : 0) |
                       (address << 9) | (write ? *value : 0), &r5);
  if (!ret) ret = skw_r5_status(r5);
  if (ret) bus->faulted = true;
  else if (!write) *value = r5 & 0xff;
  bus->ops->unlock(bus->ctx);
  return ret;
}

static int fw_set(uint32_t address, uint8_t value)
{
  return fw_f0(true, address, &value);
}

static int fw_packet(bool write, void *data, size_t length)
{
  struct skw_bus *bus = &g_fw_bus;
  uint32_t r5 = 0;
  int ret;
  if (!length || length % 512 || length > sizeof(g_rx)) return -EINVAL;
  if (!bus->attached || !bus->lite_verified) return -ENODEV;
  ret = bus->ops->lock(bus->ctx);
  if (ret) return ret;
  if (bus->faulted) { bus->ops->unlock(bus->ctx); return -ENODEV; }
  /* F1 block mode, fixed FIFO address 0x20; host uses PIO. */
  ret = bus->ops->cmd53(bus->ctx, (write ? BIT(31) : 0) |
                       0x18000000u | (0x20u << 9) | (length / 512),
                       data, length, &r5);
  if (!ret) ret = skw_r5_status(r5);
  if (ret) bus->faulted = true;
  bus->ops->unlock(bus->ctx);
  return ret;
}

static uint32_t fw_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int fw_rx_fault(const char *site, int ret, unsigned int slot, size_t bytes)
{
  size_t offset = slot * SKW_RX_SLOT_SIZE;
  printf("RADIO RX fault site=%s ret=%d slot=%u bytes=%zu valid=%" PRIu32 " next=%u raw=",
         site, ret, slot, bytes, fw_u32(g_rx + bytes - 8), g_pending);
  for (size_t n = 0; n < 128 && offset + n < bytes - 8; n++)
    printf("%02x", g_rx[offset + n]);
  putchar('\n');
  return ret;
}

static int fw_receive(unsigned int duration_ms)
{
  unsigned int slots, i;
  uint64_t deadline = now_us() + (uint64_t)duration_ms * 1000;
  uint8_t pending = 0, fifo_ind = 0;
  int ret;
  while (now_us() < deadline)
    {
      if (!g_pending)
        {
          if ((ret = fw_f0(false, 5, &pending))) return ret;
          if ((ret = fw_f0(false, 0x181, &fifo_ind))) return ret;
          if (!(pending & 2) && !g_pending && fifo_ind == g_fifo_ind)
            {
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
              rb_rx_idle();
#endif
              fw_rx_count(&g_rx_idle_sleeps);
              if (g_bt_host_mode) usleep(1000); else up_mdelay(10);
              continue;
            }
          if (!g_bt_host_mode) printf("RADIO notify cccr05=%02x fifo=%02x->%02x GPIO=%08" PRIx32 "\n",
                 pending, g_fifo_ind, fifo_ind, rd(GPIO1 + 0x70));
          g_fifo_ind = fifo_ind;
        }
      slots = g_pending ? g_pending : 1;
      if (slots > FW_RX_BATCH_SLOTS) slots = FW_RX_BATCH_SLOTS;
      size_t bytes = slots * SKW_RX_SLOT_SIZE + 512;
      memset(g_rx, 0, bytes);
      uint64_t read_start = now_us();
      if ((ret = fw_packet(false, g_rx, bytes))) return ret;
      fw_rx_count(&g_rx_batches);
      fw_rx_high(&g_rx_read_us_high, now_us() - read_start);
      g_pending = fw_u32(g_rx + bytes - 4);
      if (!g_bt_host_mode) printf("RADIO RX bytes=%zu valid=%" PRIu32 " next=%u header=%08" PRIx32 "\n",
             bytes, fw_u32(g_rx + bytes - 8), g_pending, fw_u32(g_rx));
      /* The trailer is the firmware's outstanding queue depth, not the
       * maximum number of slots in one host read.  The vendor driver clamps
       * each read to MAX_PAC_COUNT (72); it does not reject a larger queue.
       * In particular a valid depth of 84 must not discard this batch's ACKs
       * and terminate the RX owner.  Our PIO read is independently capped at
       * FW_RX_BATCH_SLOTS above; every returned slot is still bounds checked.
       */
      fw_rx_high(&g_rx_pending_high, g_pending);
      for (i = 0; i < slots; i++)
        {
          struct skw_packet packet;
          ret = skw_sdio2_decode_slot(g_rx + i * SKW_RX_SLOT_SIZE,
                                     SKW_RX_SLOT_SIZE, &packet);
          if (ret) return fw_rx_fault("sdio2", ret, i, bytes);
          if (packet.eof) break;
          if (packet.discard) { __atomic_add_fetch(&g_rx_empty, 1, __ATOMIC_RELAXED); continue; }
          if (!g_bt_host_mode) printf("RADIO RX channel=%u len=%zu\n", packet.channel, packet.length);
          if (g_bt_host_mode && (packet.channel == SKW_BT_CMD_PORT || packet.channel == SKW_BT_DATA_PORT))
            {
              ret = skw_bt_receive_slot(&g_native_bt, g_rx + i * SKW_RX_SLOT_SIZE, SKW_RX_SLOT_SIZE);
              if (ret) return fw_rx_fault("bt-host", ret, i, bytes);
              __atomic_add_fetch(&g_bt_host_rx, 1, __ATOMIC_RELAXED);
              continue;
            }
          /* Lite SDIO2 LOOPCHECK is channel 1 (legacy SDIO1 uses 7).
           * The 12-byte logical link header precedes its ASCII message.
           */
          if (packet.channel == 1 && packet.length > 12)
            {
              const uint8_t *msg = packet.payload + 12;
              size_t n = packet.length - 12;
              printf("RADIO CP message: %.*s\n", (int)n, (const char *)msg);
              if (n >= 7 && !memcmp(msg, "trunk_W", 7)) g_cp_ready = true;
              if (n == 9 && !memcmp(msg, "WIFIREADY", 9)) g_wifi_ready = true;
              if (n == 7 && !memcmp(msg, "BTREADY", 7)) g_bt_ready = true;
              if (n >= 9 && !memcmp(msg, "BSPASSERT", 9)) return -EIO;
            }
          else if (packet.channel == SKW_WIFI_DATA_PORT)
            {
              __atomic_add_fetch(&g_wifi_data_packets,1,__ATOMIC_RELAXED);
              if (nxmutex_lock(&g_wifi_state)==0)
                {
                  /* PN reuse includes the SDIO header in the descriptor.
                   * Copy before the next FIFO read overwrites g_rx. */
                  skw_eapol_push(&g_wifi_eapol,g_rx+i*SKW_RX_SLOT_SIZE,
                    SKW_RX_SLOT_SIZE);
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
                  fw_wifi_net_slot(g_rx+i*SKW_RX_SLOT_SIZE,SKW_RX_SLOT_SIZE);
#endif
                  nxmutex_unlock(&g_wifi_state);
                }
            }
          else if (packet.channel == SKW_WIFI_CMD_PORT)
            {
              if (packet.length == 12) continue; /* logical link ACK */
              if (packet.length < 20) return -EPROTO;
              const uint8_t *msg = packet.payload + 12;
              uint16_t seq = msg[2] | msg[3] << 8;
              uint16_t len = msg[4] | msg[5] << 8;
              if (len < 8 || len > packet.length - 12) return -EPROTO;
              printf("RADIO Wi-Fi RX type=%u id=%u seq=%u len=%u\n", msg[0] >> 4, msg[1], seq, len);
              if ((msg[0] >> 4) == 2)
                {
                  if (msg[1] == 0)
                    {
                      if ((ret = nxmutex_lock(&g_wifi_state))) return ret;
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
                      rb_rx_done();
#endif
                      g_wifi_scan_done = true;
                      nxsem_post(&g_wifi_scansem);
                      nxmutex_unlock(&g_wifi_state);
                    }
                  if (msg[1] == 11) fw_wifi_scan_report(msg + 8, len - 8);
                  if (msg[1] == 4) fw_wifi_mgmt_event(msg[0]&15,msg+8,len-8);
                }
              if ((msg[0] >> 4) == 1)
                {
                  if (len < 10) return -EPROTO;
                  if ((ret = nxmutex_lock(&g_wifi_state))) return ret;
                  if (g_wifi_waiting && msg[1] == g_wifi_cmd && seq == g_wifi_seq)
                    {
                      g_wifi_status = msg[8] | msg[9] << 8;
                      g_wifi_response_len = len - 10;
                      memcpy(g_wifi_response, msg + 10, g_wifi_response_len);
                      g_wifi_ack = true;
                      g_wifi_waiting = false;
                      nxsem_post(&g_wifi_acksem);
                    }
                  nxmutex_unlock(&g_wifi_state);
                }
            }
          else if (packet.channel == SKW_BT_CMD_PORT)
            {
              struct skw_hci_packet hci;
              ret = skw_hci_decode(&packet, &hci);
              if (ret) return ret;
              if (!hci.ack && !hci.vendor_log)
                {
                  printf("RADIO HCI event:");
                  for (size_t j = 0; j < hci.length; j++) printf(" %02x", hci.data[j]);
                  putchar('\n');
                  if (hci.length >= 6 && hci.data[0] == 0x0e)
                    {
                      uint16_t opcode = hci.data[3] | hci.data[4] << 8;
                      if (opcode == 0x1001 && hci.length >= 14 && !hci.data[5])
                        g_hci_revision = hci.data[7] | hci.data[8] << 8;
                      if (opcode == 0x1009 && hci.length >= 12 && !hci.data[5])
                        memcpy(g_hci_addr, hci.data + 6, 6);
                      if (opcode == g_hci_opcode)
                        { g_hci_done = true; g_hci_status = hci.data[5]; }
                    }
                  if (hci.length >= 4 && hci.data[0] == 0x3e && hci.data[2] == 2)
                    {
                      size_t p = 4;
                      for (unsigned int n = 0; n < hci.data[3]; n++)
                        {
                          if (hci.length - p < 10) return -EPROTO;
                          size_t len = hci.data[p+8];
                          if (len + 10 > hci.length - p) return -EPROTO;
                          g_adv_reports++;
                          printf("RADIO BLE advertising report=%u type=%u RSSI=%d bytes=%zu\n",
                                 g_adv_reports, hci.data[p], (int)(int8_t)hci.data[p+9+len], len);
                          p += len + 10;
                        }
                    }
                }
            }
        }
      if (!g_pending)
        {
          fw_rx_count(&g_rx_idle_sleeps);
          if (g_bt_host_mode) usleep(1000); else up_mdelay(10);
        }
    }
  if (g_pending) fw_rx_count(&g_rx_budget_pending);
  return 0;
}

static int fw_region(uint32_t address, uintptr_t staged, size_t length,
                     uint16_t expected)
{
  uint8_t check[512];
  const uint8_t *src = (const uint8_t *)staged;
  size_t done = 0, n, got;
  uint16_t crc = 0;
  int ret = skw_stage_region(&g_fw_bus, address, src, length, expected, &done);
  printf("RADIO FW write %08" PRIx32 " ret=%d completed=%zu/%zu\n",
         address, ret, done, length);
  if (ret || done != length) return ret ? ret : -EIO;
  /* Compare every byte, not just a checksum or a short sample. */
  for (done = 0; done < length; done += n)
    {
      n = length - done;
      if (n > sizeof(check)) n = sizeof(check);
      ret = skw_mem_read(&g_fw_bus, address + done, check, n, &got);
      if (ret || got != n) return ret ? ret : -EIO;
      if (memcmp(check, src + done, n))
        { printf("RADIO FW mismatch at +%zx\n", done); return -EBADMSG; }
      crc = skw_crc16(crc, check, n);
    }
  printf("RADIO FW verified %08" PRIx32 " bytes=%zu crc16=%04x\n", address, length, crc);
  return crc == expected ? 0 : -EBADMSG;
}

static int sdio_firmware_boot(void)
{
  uint8_t value;
  int ret;
  /* Independent read-only MMU region, outside CONFIG_RAM heap and image. */
  if (skw_crc16(0, (const void *)K7_RADIO_DRAM, 193224) != 0x2a29 ||
      skw_crc16(0, (const void *)K7_RADIO_IRAM, 356616) != 0xd34b)
    { puts("RADIO firmware staging CRC mismatch; no radio changes"); return -EBADMSG; }
  ret = sdio_chip_open(&g_fw_host, &g_fw_bus);
  if (ret) return ret;
  /* Match the original Android download: ADMA packet format and sleep off.
   * No host DMA engine is enabled. No external NV overlay was used there.
   */
  if ((ret = fw_set(0x165, 1))) return ret;
  if ((ret = fw_set(0x167, 1))) return ret;
  if ((ret = fw_region(0x20200000, K7_RADIO_DRAM, 193224, 0x2a29))) return ret;
  if ((ret = fw_region(0x00100000, K7_RADIO_IRAM, 356616, 0xd34b))) return ret;
  if ((ret = fw_f0(false, 0x16, &value))) return ret;
  if ((ret = fw_set(0x16, value | 2))) return ret;
  if ((ret = fw_set(4, 3))) return ret;  /* poll in-band interrupt pending */
  /* Full readback changes the indirect address window. Restore the exact
   * final base used by the vendor's IRAM download before its start flag.
   */
  for (unsigned int i = 0; i < 4; i++)
    {
      if ((ret = fw_f0(false, 0x15c + i, &value))) return ret;
      printf("RADIO pre-start window[%u]=%02x\n", i, value);
      if ((ret = fw_set(0x15c + i, (0x00100000u >> (8*i)) & 0xff))) return ret;
    }
  if ((ret = fw_set(0x160, 1))) return ret;
  if ((ret = fw_receive(3000))) return ret;
  if (!g_cp_ready)
    {
      const uint16_t regs[] = {4, 5, 0x16, 0x160, 0x165, 0x167, 0x180, 0x181, 0x184};
      for (unsigned int i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        {
          if ((ret = fw_f0(false, regs[i], &value))) return ret;
          printf("RADIO F0[%03x]=%02x\n", regs[i], value);
        }
      return -ETIMEDOUT;
    }
  if ((ret = fw_set(0x1b0, 1))) return ret; /* Wi-Fi SERVICE_START */
  if ((ret = fw_receive(2000))) return ret;
  if (!g_wifi_ready) return -ETIMEDOUT;
  if ((ret = fw_set(0x1b0, 4))) return ret; /* BT SERVICE_START */
  if ((ret = fw_receive(2000))) return ret;
  if (!g_bt_ready) return -ETIMEDOUT;
  /* Read Local Version Information; no pairing or RF scan is implied. */
  uint8_t frame[512];
  const uint8_t cmd[] = {0x01, 0x10, 0x00};
  size_t written = 0;
  ret = skw_hci_encode(1, cmd, sizeof(cmd), frame, sizeof(frame), &written);
  if (!ret) ret = fw_packet(true, frame, written);
  if (!ret) ret = fw_receive(2000);
  return ret;
}

/* Control-plane bring-up only; no network or Bluetooth host registration. */
static int fw_hci_command(uint16_t opcode, const void *params, size_t length)
{
  uint8_t cmd[258], frame[512];
  size_t written;
  int ret;
  if (!g_bt_ready || length > 255 || (length && !params)) return -EINVAL;
  cmd[0] = opcode & 0xff; cmd[1] = opcode >> 8; cmd[2] = length;
  if (length) memcpy(cmd + 3, params, length);
  g_hci_opcode = opcode; g_hci_done = false; g_hci_status = -ETIMEDOUT;
  ret = skw_hci_encode(1, cmd, length + 3, frame, sizeof(frame), &written);
  if (!ret) ret = fw_packet(true, frame, written);
  if (!ret) ret = fw_receive(1000);
  if (!ret) ret = g_hci_done ? (g_hci_status ? -EIO : 0) : -ETIMEDOUT;
  printf("RADIO HCI opcode=%04x complete=%u status=%d ret=%d\n",
         opcode, g_hci_done, g_hci_status, ret);
  return ret;
}

static int fw_bt_initialize(void)
{
  const uint8_t *nv = (const void *)K7_RADIO_NV;
  uint8_t params[35] = {0, 33};
  size_t pos = 4;
  int ret;
  if (g_bt_initialized) return 0;
  if (skw_crc16(0, nv, 37) != 0xe645 || memcmp(nv, "NVDS", 4)) return -EBADMSG;
  if ((ret = fw_hci_command(0x1001, NULL, 0))) return ret;
  if (g_hci_revision != 0x5302) return -ENODEV;
  if ((ret = fw_hci_command(0x1009, NULL, 0))) return ret;
  /* Follow vendor NVDS setup. The pre-NV ROM address is a placeholder;
   * read the actual efuse-derived controller address after reset.
   */
  memcpy(params + 2, nv + 4, 33);
  while (pos < 37)
    {
      size_t n;
      if (pos + 3 > 37) return -EBADMSG;
      n = nv[pos + 2] + 3;
      if (pos + n > 37) return -EBADMSG;
      if (nv[pos] == 5 && n > 3) params[2 + pos - 4 + 3] = 1;
      pos += n;
    }
  if ((ret = fw_hci_command(0xfc80, params, sizeof(params)))) return ret;
  if ((ret = fw_hci_command(0x0c03, NULL, 0))) return ret;
  if ((ret = fw_hci_command(0x1009, NULL, 0))) return ret;
  printf("RADIO BT controller address=%02x:%02x:%02x:%02x:%02x:%02x post_reset=1\n",
         g_hci_addr[5],g_hci_addr[4],g_hci_addr[3],g_hci_addr[2],g_hci_addr[1],g_hci_addr[0]);
  g_bt_initialized = true;
  return 0;
}
static int fw_bt_scan(void)
{
  const uint8_t scan_params[] = {0, 0x60, 0, 0x30, 0, 0, 0};
  const uint8_t scan_on[] = {1, 1}, scan_off[] = {0, 1};
  int ret = fw_bt_initialize();
  if (ret) return ret;
  if ((ret = fw_hci_command(0x2003, NULL, 0))) return ret;
  if ((ret = fw_hci_command(0x200b, scan_params, sizeof(scan_params)))) return ret;
  g_adv_reports = 0;
  ret = fw_hci_command(0x200c, scan_on, sizeof(scan_on));
  if (!ret) ret = fw_receive(5000);
  int stop = g_fw_bus.faulted ? -EIO : fw_hci_command(0x200c, scan_off, sizeof(scan_off));
  printf("RADIO BLE passive scan reports=%u ret=%d stop_ret=%d; connected=0 paired=0\n",g_adv_reports,ret,stop);
  return ret ? ret : stop;
}

static int fw_wifi_command_result(bool data_path,uint8_t id,int transport,
                                 bool matched_ack,unsigned status)
{
  if(transport)return transport;
  if(!matched_ack)return -ETIMEDOUT;
  if(data_path && id==15 && status==3)return -EAGAIN;
  return status?-EIO:0;
}
static int fw_wifi_command_policy(uint8_t id, const void *params, size_t length,
                                  bool data_path)
{
  static uint8_t msg[1100], frame[1536];
  memset(msg, 0, sizeof(msg));
  size_t written;
  int ret;
  if (!g_wifi_ready || length > sizeof(msg) - 8 || (length && !params)) return -EINVAL;
  if ((ret = nxmutex_lock(&g_wifi_state))) return ret;
  while (nxsem_trywait(&g_wifi_acksem) == 0) {}
  g_wifi_seq++;
  g_wifi_cmd = id; g_wifi_ack = false; g_wifi_status = 0xffff;
  g_wifi_response_len = 0; g_wifi_waiting = true;
  nxmutex_unlock(&g_wifi_state);
  msg[1] = id; msg[2] = g_wifi_seq & 0xff; msg[3] = g_wifi_seq >> 8;
  msg[4] = (length + 8) & 255; msg[5] = (length + 8) >> 8;
  if (length) memcpy(msg + 8, params, length);
  ret = skw_sdio2_encode(6, msg, length + 8, frame, sizeof(frame), &written);
  if (!ret) ret = fw_packet(true, frame, written);
  /* fw_packet completes synchronous PIO transfer. Do not retain key/EAPOL
   * material in static staging buffers while awaiting ACK or after errors. */
  skw_wpa_wipe(msg,sizeof(msg));
  skw_wpa_wipe(frame,sizeof(frame));
  if (!ret)
    ret = g_bt_host_mode ? nxsem_tickwait_uninterruptible(&g_wifi_acksem, MSEC2TICK(id == 50 ? 5000 : 1500))
                         : fw_receive(id == 50 ? 5000 : 1500);
  int locked = nxmutex_lock(&g_wifi_state);
  if (locked) return locked;
  g_wifi_waiting = false;
  int result = fw_wifi_command_result(data_path,id,ret,g_wifi_ack,g_wifi_status);
  if(data_path && id==15 && !ret && g_wifi_ack && g_wifi_status==3)
    printf("WIFI frame_ack cmd=15 seq=%u status=3 retryable=1 rc=%d\n",g_wifi_seq,result);
  printf("RADIO Wi-Fi cmd=%u seq=%u ack=%u status=%u response=%zu ret=%d\n",
         id, g_wifi_seq, g_wifi_ack, g_wifi_status, g_wifi_response_len, ret);
  nxmutex_unlock(&g_wifi_state);
  return result;
}

#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
#define fw_wifi_command fw_wifi_command_raw
#endif
static int fw_wifi_command(uint8_t id,const void *params,size_t length)
{return fw_wifi_command_policy(id,params,length,false);}

static int fw_wifi_wpa_command(void *arg,uint8_t id,const void *p,size_t n)
{
  (void)arg;
  return fw_wifi_command(id,p,n);
}

#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
#undef fw_wifi_command
static int fw_wifi_command(uint8_t id,const void *params,size_t length)
{int rc=fw_wifi_command_raw(id,params,length);rb_note(id,rc);return rc;}
#endif
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
static int fw_wifi_data_command(const void *params,size_t length)
{
  int rc=fw_wifi_command_policy(15,params,length,true);
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
  rb_note(15,rc);
#endif
  return rc;
}
#endif
static int fw_wifi_info(void)
{
  uint8_t timestamp[8] = {0};
  int ret = fw_wifi_command(2, NULL, 0); /* SYN_VERSION */
  if (ret) return ret;
  if (g_wifi_response_len != 512 || g_wifi_response[1] != 1 ||
      g_wifi_response[2] != 1) return -EPROTO;
  printf("RADIO Wi-Fi version protocol GET_INFO=%u SYN_VERSION=%u OPEN=%u SCAN=%u\n",
         g_wifi_response[1], g_wifi_response[2], g_wifi_response[3], g_wifi_response[5]);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
  for (unsigned int i = 0; i < 8; i++) timestamp[i] = (ms >> (8*i)) & 0xff;
  ret = fw_wifi_command(1, timestamp, sizeof(timestamp)); /* GET_INFO */
  if (!ret)
    {
      printf("RADIO Wi-Fi chip-info bytes:");
      for (size_t i = 0; i < g_wifi_response_len; i++) printf(" %02x", g_wifi_response[i]);
      putchar('\n');
      if (g_wifi_response_len != 234 || fw_u32(g_wifi_response + 178) != 0x300 ||
          skw_diag_u16(g_wifi_response + 182) != 0 || (g_wifi_response[64] & 1)) return -EPROTO;
      memcpy(g_wifi_mac, g_wifi_response + 64, 6);
      if (!memcmp(g_wifi_mac, "\0\0\0\0\0\0", 6)) return -EPROTO;
      /* Packed skw_chip_info: MAC at 64, rates at 70, BW at 86,
       * private capabilities at 90; bit 2 selects RX PN reuse layout. */
      g_wifi_pn_reuse = (fw_u32(g_wifi_response + 90) & 4) != 0;
      g_wifi_info_valid = true;
    }
  return ret;
}

static int fw_wifi_scan(void)
{
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
  if(__atomic_load_n(&g_wifi_network_active,__ATOMIC_ACQUIRE))return -EBUSY;
#endif
  const uint8_t *calib = (const void *)K7_RADIO_CALIB;
  const uint8_t five[] = {36, 40, 44, 48, 149, 153, 157, 161, 165};
  uint8_t chunk[516] = {0}, open[10] = {1, 0}, scan[98] = {0};
  int ret, stop = 0, close;
  if (!g_wifi_info_valid) return -EAGAIN;
  if (!g_wifi_calibrated)
    {
      if (skw_crc16(0, calib, 2372) != 0xd20d) return -EBADMSG;
      for (size_t offset = 0, seq = 0; offset < 2372; seq++)
        {
          size_t bytes = 2372 - offset;
          if (bytes > 512) bytes = 512;
          memset(chunk, 0, sizeof(chunk));
          chunk[0] = seq; chunk[1] = offset + bytes == 2372;
          chunk[2] = bytes & 255; chunk[3] = bytes >> 8;
          memcpy(chunk + 4, calib + offset, bytes);
          ret = fw_wifi_command(50, chunk, sizeof(chunk));
          printf("RADIO Wi-Fi calibration seq=%zu bytes=%zu end=%u ret=%d\n",
                 seq, bytes, chunk[1], ret);
          if (ret) return ret;
          offset += bytes;
        }
      g_wifi_calibrated = true;
    }
  memcpy(open + 4, g_wifi_mac, 6);
  ret = fw_wifi_command(3, open, sizeof(open));
  if (ret) goto scan_close;
  /* Passive scan: no SSID probe requests and no association. */
  scan[8] = 22; scan[12] = 32;
  for (unsigned int i = 0; i < 22; i++)
    {
      scan[32 + i * 3] = i < 13 ? i + 1 : five[i - 13];
      scan[33 + i * 3] = i < 13 ? 0 : 1;
      scan[34 + i * 3] = 0x80;
    }
  if ((ret = nxmutex_lock(&g_wifi_state))) goto scan_close;
  while (nxsem_trywait(&g_wifi_scansem) == 0) {}
  g_wifi_scan_done = false; g_wifi_reports = 0; g_wifi_unique = 0;
  nxmutex_unlock(&g_wifi_state);
  ret = fw_wifi_command(5, scan, sizeof(scan));
  if (!ret)
    ret = g_bt_host_mode ? nxsem_tickwait_uninterruptible(&g_wifi_scansem, MSEC2TICK(8000)) : fw_receive(8000);
  int check=nxmutex_lock(&g_wifi_state);
  if(!check){if(!ret&&!g_wifi_scan_done)ret=-ETIMEDOUT;nxmutex_unlock(&g_wifi_state);}
  else if(!ret)ret=check;
  if (ret) stop = fw_wifi_command(6, NULL, 0);
scan_close:
  close = fw_wifi_command(4, NULL, 0);
  int locked = nxmutex_lock(&g_wifi_state);
  if (locked) return locked;
  printf("RADIO Wi-Fi passive scan reports=%u unique=%u done=%u ret=%d stop=%d close=%d; network_connected=0\n",
         g_wifi_reports, g_wifi_unique, g_wifi_scan_done, ret, stop, close);
  if (!ret && !g_wifi_scan_done) ret = -ETIMEDOUT;
  nxmutex_unlock(&g_wifi_state);
  return ret ? ret : (stop ? stop : close);
}

/* Native NuttX host transport: bounded queue, one SDIO link packet in flight. */
struct radio_bt_tx { size_t length; uint8_t data[2048]; };
static struct radio_bt_tx g_bt_queue[8], g_bt_current;
static mutex_t g_bt_qlock = NXMUTEX_INITIALIZER;
static sem_t g_bt_items = SEM_INITIALIZER(0), g_bt_acksem = SEM_INITIALIZER(0);
static unsigned int g_bt_head, g_bt_tail, g_bt_count;
static bool g_bt_pending;
static uint8_t g_bt_pending_channel;
static unsigned int g_bt_host_tx, g_bt_host_ack;

static int bt_lower_ready(void *ctx)
{
  (void)ctx;
  return g_bt_initialized && !__atomic_load_n(&g_bt_worker_fault, __ATOMIC_ACQUIRE) ? 0 : -EHOSTDOWN;
}
static int bt_lower_send(void *ctx, const void *packet, size_t length)
{
  int ret;
  (void)ctx;
  if (!packet || !length || length > sizeof(g_bt_queue[0].data)) return -EMSGSIZE;
  ret = nxmutex_lock(&g_bt_qlock);
  if (ret) return ret;
  if (g_bt_count == 8 || __atomic_load_n(&g_bt_worker_fault, __ATOMIC_ACQUIRE))
    { nxmutex_unlock(&g_bt_qlock); return -EAGAIN; }
  memcpy(g_bt_queue[g_bt_tail].data, packet, length);
  g_bt_queue[g_bt_tail].length = length;
  g_bt_tail = (g_bt_tail + 1) % 8; g_bt_count++;
  nxmutex_unlock(&g_bt_qlock);
  nxsem_post(&g_bt_items);
  return 0;
}
static int bt_lower_ack(void *ctx, uint8_t channel, uint16_t seq)
{
  bool pending = true;
  (void)ctx; (void)seq;
  if (channel == __atomic_load_n(&g_bt_pending_channel, __ATOMIC_ACQUIRE) &&
      __atomic_compare_exchange_n(&g_bt_pending, &pending, false, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
      __atomic_add_fetch(&g_bt_host_ack, 1, __ATOMIC_RELAXED);
      nxsem_post(&g_bt_acksem);
    }
  return 0;
}
static const struct skw_bt_lower g_bt_lower = { bt_lower_ready, bt_lower_send, bt_lower_ack };

static int bt_tx_worker(int argc, char **argv)
{
  int ret = 0;
  (void)argc; (void)argv;
  while (!__atomic_load_n(&g_bt_worker_fault, __ATOMIC_ACQUIRE))
    {
      ret = nxsem_wait_uninterruptible(&g_bt_items);
      if (ret) break;
      if (__atomic_load_n(&g_bt_worker_fault, __ATOMIC_ACQUIRE)) break;
      if ((ret = nxmutex_lock(&g_bt_qlock))) break;
      if (!g_bt_count) { nxmutex_unlock(&g_bt_qlock); continue; }
      memcpy(&g_bt_current, &g_bt_queue[g_bt_head], sizeof(g_bt_current));
      g_bt_head = (g_bt_head + 1) % 8; g_bt_count--;
      nxmutex_unlock(&g_bt_qlock);
      while (nxsem_trywait(&g_bt_acksem) == 0) {}
      __atomic_store_n(&g_bt_pending_channel, g_bt_current.data[3], __ATOMIC_RELEASE);
      __atomic_store_n(&g_bt_pending, true, __ATOMIC_RELEASE);
      ret = fw_packet(true, g_bt_current.data, g_bt_current.length);
      if (ret) break;
      __atomic_add_fetch(&g_bt_host_tx, 1, __ATOMIC_RELAXED);
      ret = nxsem_tickwait_uninterruptible(&g_bt_acksem, MSEC2TICK(1500));
      if (ret) break;
    }
  __atomic_store_n(&g_bt_worker_fault, true, __ATOMIC_RELEASE);
  printf("RADIO BT TX worker stopped ret=%d\n", ret);
  return ret;
}
static int bt_rx_worker(int argc, char **argv)
{
  int ret = 0;
  (void)argc; (void)argv;
  while (!__atomic_load_n(&g_bt_worker_fault, __ATOMIC_ACQUIRE))
    {
      ret = fw_receive(50);
      if (ret) break;
      /* Each batch unlocked the bus; yield even while backlog remains. */
      if (g_pending) sched_yield(); else usleep(1000);
    }
  __atomic_store_n(&g_bt_worker_fault, true, __ATOMIC_RELEASE);
  nxsem_post(&g_bt_items);
  printf("RADIO BT RX worker stopped ret=%d\n", ret);
  return ret;
}
static int fw_bt_host(void)
{
  int ret, rxpid, txpid;
  if (g_bt_host_mode) return -EALREADY;
  ret = fw_bt_initialize();
  if (ret) return ret;
  ret = skw_bt_init(&g_native_bt, &g_bt_lower, NULL);
  if (ret) return ret;
  g_bt_host_mode = true; /* One background RX dispatcher for both services. */
  txpid = kthread_create("skw_bt_tx", 95, 4096, bt_tx_worker, NULL);
  if (txpid < 0) { __atomic_store_n(&g_bt_worker_fault,true,__ATOMIC_RELEASE); return txpid; }
  rxpid = kthread_create("skw_radio_rx", 90, 4096, bt_rx_worker, NULL);
  if (rxpid < 0)
    { __atomic_store_n(&g_bt_worker_fault,true,__ATOMIC_RELEASE); nxsem_post(&g_bt_items); return rxpid; }
  ret = bt_driver_register(&g_native_bt.driver);
  if(!ret) ret=prov_start();
  if (ret)
    { __atomic_store_n(&g_bt_worker_fault,true,__ATOMIC_RELEASE); nxsem_post(&g_bt_items); }
  printf("RADIO native Bluetooth host register=%d txpid=%d rxpid=%d tx=%u link_ack=%u RX=%u fault=%u; WiFi_connected=0\n",
         ret, txpid, rxpid, __atomic_load_n(&g_bt_host_tx,__ATOMIC_RELAXED),
         __atomic_load_n(&g_bt_host_ack,__ATOMIC_RELAXED),
         __atomic_load_n(&g_bt_host_rx,__ATOMIC_RELAXED),
         __atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE));
  return ret;
}

static int fw_wifi_target_scan(void)
{
  int ret;
  if (!g_wifi_info_valid && (ret=fw_wifi_info())) return ret;
  if ((ret=nxmutex_lock(&g_wifi_state))) return ret;
  g_wifi_target_valid=false;
  nxmutex_unlock(&g_wifi_state);
  ret=fw_wifi_scan();
  if (ret) return ret;
  if ((ret=nxmutex_lock(&g_wifi_state))) return ret;
  if (!g_wifi_target_valid) ret=-ENOENT;
  else
    {
      struct skw_bss *b=&g_wifi_target;
      printf("RADIO target=selected BSSID=%02x:%02x:%02x:%02x:%02x:%02x channel=%u band=%u RSSI=%d beacon=%u capability=%04x IE=%u RSN=%u WPA=%u group=%u pairwise=%08" PRIx32 " AKM=%08" PRIx32 " unknown=%u\n",
        b->scan.bssid[0],b->scan.bssid[1],b->scan.bssid[2],b->scan.bssid[3],b->scan.bssid[4],b->scan.bssid[5],
        b->scan.channel,b->scan.band,b->scan.rssi,b->beacon_interval,b->capability,b->ie_len,b->rsn,b->wpa,b->group_cipher,b->pairwise,b->akm,b->unknown_suite);
      printf("RADIO target beacon IE:");
      for(unsigned int i=0;i<b->ie_len;i++) printf("%02x",b->ies[i]);
      putchar('\n');
    }
  nxmutex_unlock(&g_wifi_state);
  return ret;
}

#include "wifi_assoc_probe.inc"
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
#include "wifi_ip_service.inc"
#endif
#include "wifi_auth_console.inc"

#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
#include "radio_backend.inc"
#endif
#include "prov_service.inc"

static int fw_ble_advertise(void)
{
  struct bt_eir_s ad[3] = {0}, sd[2] = {0};
  struct btreq_s req = {0};
  static const char name[] = "VelaVision K7";
  int fd,ret;
  if (!g_bt_host_mode || __atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE)) return -EHOSTDOWN;
  ad[0].len=2; ad[0].type=BT_EIR_FLAGS;
  ad[0].data[0]=BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
  ad[1].len=sizeof(name); ad[1].type=BT_EIR_NAME_COMPLETE;
  memcpy(ad[1].data,name,sizeof(name)-1);
  sd[0]=ad[1]; /* Correct terminating zero entry; no embedded NUL in AD. */
  strlcpy(req.btr_name,"bnep0",IFNAMSIZ);
  req.btr_advtype=BT_LE_ADV_IND; req.btr_advad=ad; req.btr_advsd=sd;
  fd=socket(PF_BLUETOOTH,SOCK_RAW,BTPROTO_L2CAP);
  if(fd<0) return -errno;
  /* Parameters may only be changed while advertising is disabled. */
  ret=ioctl(fd,SIOCBTADVSTOP,(unsigned long)(uintptr_t)&req);
  if(ret<0) { ret=-errno; close(fd); return ret; }
  ret=ioctl(fd,SIOCBTADVSTART,(unsigned long)(uintptr_t)&req);
  if(ret<0) ret=-errno;
  close(fd);
  if(nxmutex_lock(&g_native_bt.lock)==0)
    {
      printf("RADIO BLE advertise name=VelaVision K7 connectable=1 ret=%d HCI=%u/%u/%u/%u connected=%u\n",
        ret,g_native_bt.adv_status[0],g_native_bt.adv_status[1],g_native_bt.adv_status[2],g_native_bt.adv_status[3],g_native_bt.connected);
      for(unsigned int i=0;i<4;i++) if(g_native_bt.adv_status[i] && !ret) ret=-EIO;
      nxmutex_unlock(&g_native_bt.lock);
    }
  return ret;
}
static int fw_ble_status(void)
{
  if(!g_bt_host_mode) return -ENODEV;
  int ret=nxmutex_lock(&g_native_bt.lock);
  if(ret) return ret;
  printf("RADIO BLE name=VelaVision K7 connected=%u handle=%u ACL_RX=%u ACL_TX_queued=%u\n",
    g_native_bt.connected,g_native_bt.handle,g_native_bt.acl_rx,g_native_bt.acl_tx);
  printf("RADIO BLE history connections=%u disconnections=%u last_reason=%u encrypt_status=%u encrypted=%u\n",
    g_native_bt.connections,g_native_bt.disconnections,g_native_bt.disconnect_reason,
    g_native_bt.encryption_status,g_native_bt.encryption_enabled);
  nxmutex_unlock(&g_native_bt.lock);
  return 0;
}

int main(int argc, char **argv)
{
  int ret;
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
  if(argc==2&&!strcmp(argv[1],"wifi-service-start"))
    {ret=rb_start();printf("WIFI shared start ret=%d\n",ret);return ret?1:0;}
  if(argc>1&&!strncmp(argv[1],"wifi-",5)&&strcmp(argv[1],"wifi-status"))
    {puts("WIFI shared service: legacy RF CLI disabled; use owner transaction API");return 1;}
#endif
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
  if(argc==2 && !strcmp(argv[1],"wifi-status")){fw_wifi_ip_status();return 0;}
  if(argc==2 && !strcmp(argv[1],"wifi-disconnect"))
    {__atomic_store_n(&g_wifi_network_stop,true,__ATOMIC_RELEASE);return 0;}
  if(argc==2 && !strncmp(argv[1],"wifi-",5) && __atomic_load_n(&g_wifi_network_active,__ATOMIC_ACQUIRE))
    {puts("WIFI link active; use wifi-status or wifi-disconnect");return 1;}
#endif
  if(argc==2 && !strcmp(argv[1],"wifi-auth"))return fw_wifi_auth_console()?1:0;
  if (argc == 2 && (!strcmp(argv[1],"ble-advertise") || !strcmp(argv[1],"ble-status")))
    {
      ret=!strcmp(argv[1],"ble-advertise") ? fw_ble_advertise() : fw_ble_status();
      printf("RADIO %s ret=%d\n",argv[1],ret);
      return ret ? 1 : 0;
    }
  if(argc==2 && !strcmp(argv[1],"provision-status")) { prov_status();return 0; }
  if(argc==2 && !strcmp(argv[1],"provision-start")) return prov_start()?1:0;
  if (argc == 2 && !strcmp(argv[1], "host-status"))
    {
      printf("RADIO host_mode=%u fault=%u tx=%u ACK=%u RX=%u\n",g_bt_host_mode,
        __atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE),
        __atomic_load_n(&g_bt_host_tx,__ATOMIC_RELAXED),
        __atomic_load_n(&g_bt_host_ack,__ATOMIC_RELAXED),
        __atomic_load_n(&g_bt_host_rx,__ATOMIC_RELAXED));
      printf("RADIO empty_slots=%u\n", __atomic_load_n(&g_rx_empty,__ATOMIC_RELAXED));
      printf("RADIO RX batches=%u pending_high=%u read_us_high=%u\n",
        __atomic_load_n(&g_rx_batches,__ATOMIC_RELAXED),
        __atomic_load_n(&g_rx_pending_high,__ATOMIC_RELAXED),
        __atomic_load_n(&g_rx_read_us_high,__ATOMIC_RELAXED));
      printf("RADIO RX idle_sleeps=%u budget_pending=%u\n",
        __atomic_load_n(&g_rx_idle_sleeps,__ATOMIC_RELAXED),
        __atomic_load_n(&g_rx_budget_pending,__ATOMIC_RELAXED));
      return 0;
    }
  if (g_bt_host_mode && argc==2 && (!strcmp(argv[1],"wifi-join-probe") || !strcmp(argv[1],"wifi-assoc-probe")))
    {
      if (__atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE)) return 1;
      ret=nxmutex_lock(&g_wifi_operation);
      if (ret) return 1;
      ret=fw_wifi_join_probe(!strcmp(argv[1],"wifi-assoc-probe"));
      nxmutex_unlock(&g_wifi_operation);
      return ret ? 1:0;
    }
  if (g_bt_host_mode && argc == 2 &&
      (!strcmp(argv[1], "wifi-info") || !strcmp(argv[1], "wifi-scan") || !strcmp(argv[1],"wifi-target-scan")))
    {
      if (__atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE)) return 1;
      ret = nxmutex_lock(&g_wifi_operation);
      if (ret) return 1;
      ret = !strcmp(argv[1], "wifi-info") ? fw_wifi_info() : (!strcmp(argv[1],"wifi-target-scan") ? fw_wifi_target_scan() : fw_wifi_scan());
      nxmutex_unlock(&g_wifi_operation);
      printf("RADIO shared-RX %s ret=%d BT_RX=%u fault=%u; WiFi_connected=0\n",
             argv[1],ret,__atomic_load_n(&g_bt_host_rx,__ATOMIC_RELAXED),
             __atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE));
      return ret ? 1 : 0;
    }
  if (g_bt_host_mode)
    { puts("RADIO background host owns transport; use bt/host-status or RAM reboot"); return 1; }
  if (argc == 2 && !strcmp(argv[1], "bt-host"))
    {
      ret = fw_bt_host();
      printf("RADIO bt-host ret=%d\n",ret);
      return ret ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "wifi-scan"))
    {
      ret = fw_wifi_scan();
      printf("RADIO wifi-scan ret=%d; network_connected=0\n", ret);
      return ret ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "wifi-info"))
    {
      ret = fw_wifi_info();
      printf("RADIO wifi-info ret=%d; network_connected=0\n", ret);
      return ret ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "bt-scan"))
    {
      ret = fw_bt_scan();
      printf("RADIO bt-scan ret=%d; paired=0\n", ret);
      return ret ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "fw-poll"))
    {
      ret = fw_receive(3000);
      printf("RADIO poll_ret=%d cp=%u wifi=%u bt=%u\n", ret, g_cp_ready, g_wifi_ready, g_bt_ready);
      return ret ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "sdio-boot"))
    {
      ret = sdio_firmware_boot();
      printf("RADIO id36 boot_ret=%d cp_ready=%u wifi_ready=%u bt_ready=%u; network_connected=0 paired=0\n",
             ret, g_cp_ready, g_wifi_ready, g_bt_ready);
      printf("RADIO final fault=%u raw=%08" PRIx32 " status=%08" PRIx32
             " host=%zu card=%" PRIu32 " cleanup=%d\n",
             g_fw_host.dw.faulted, g_fw_host.dw.last_raw,
             g_fw_host.dw.last_status, g_fw_host.dw.host_bytes,
             g_fw_host.dw.card_bytes, g_fw_host.dw.cleanup_error);
      return ret ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "sdio-probe"))
    {
      ret = sdio_chip_probe();
      printf("RADIO probe_ret=%d; firmware=0 wifi=0 bluetooth=0\n", ret);
      return ret ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "pmic-status"))
    {
      return pmic_status() ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "rtc-status"))
    {
      return rtc_status() ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "status"))
    {
      status();
      return 0;
    }
  if (argc == 2 && (!strcmp(argv[1], "sdio-id") ||
                   !strcmp(argv[1], "sdio-id-200k") ||
                   !strcmp(argv[1], "sdio-gpio-id")))
    {
      ret = sdio_identify(!strcmp(argv[1], "sdio-gpio-id"),
                          !strcmp(argv[1], "sdio-id-200k"));
      printf("RADIO identify_ret=%d; firmware=0 wifi=0 bluetooth=0\n", ret);
      return ret ? 1 : 0;
    }
  puts("k7radio sdio-boot | sdio-probe | status | rtc-status | pmic-status | sdio-id | sdio-id-200k | sdio-gpio-id");
  return 1;
}
