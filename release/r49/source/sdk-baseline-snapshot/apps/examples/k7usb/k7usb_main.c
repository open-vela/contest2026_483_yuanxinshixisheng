/* SPDX-License-Identifier: Apache-2.0 */
/* K7 USB-A controller diagnostics. Reads only; no USB enumeration or DMA.
 * Register sources: rockchip-linux/kernel develop-6.1 rk3576.dtsi,
 * pm_domains.c, clk-rk3576.c, drivers/usb/dwc3/core.h. See README.
 */

#ifndef K7USB_TEST
#include <nuttx/config.h>
#endif
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CRU_BASE       UINT32_C(0x27200000)
#define PMU_BASE       UINT32_C(0x27380000)
#define HOST_BASE      UINT32_C(0x23400000)
#define GATE34         (CRU_BASE + 0x888)
#define GATE35         (CRU_BASE + 0x88c)
#define RESET35        (CRU_BASE + 0xa8c)
#define PMU_REPAIR     (PMU_BASE + 0x570)
#define PMU_IDLE       (PMU_BASE + 0x128)
#define PMU_ACK        (PMU_BASE + 0x120)
#define PHP_POWER_ON   (UINT32_C(1) << 9)
#define PHP_BUS_IDLE   (UINT32_C(1) << 15)
#define PHP_ROOT_GATE  (UINT32_C(1) << 7)
#define HOST_GATE_MASK (UINT32_C(7) << 3)
#define HOST_RESET    (UINT32_C(1) << 3)

#ifdef K7USB_TEST
extern uint32_t k7usb_read32(uintptr_t address);
#else
static uint32_t k7usb_read32(uintptr_t address)
{
  return *(volatile const uint32_t *)address;
}
#endif

static uint32_t showreg(const char *name, uintptr_t address)
{
  uint32_t value = k7usb_read32(address);
  char line[96];
  int length=snprintf(line,sizeof(line),"%-15s 0x%08" PRIxPTR
                      " = 0x%08" PRIx32 "\n",name,address,value);
  if (length > 0 && (size_t)length < sizeof(line))
    for (int i=0;i<length;i++)
      {
        putchar(line[i]);
        fflush(stdout);
#ifndef K7USB_TEST
        /* Diagnostic output only: CH340/VM path loses burst output. */
        struct timespec gap={0,1000000};
        nanosleep(&gap,NULL);
#endif
      }
  return value;
}

static int clocks(void)
{
  uint32_t repair = showreg("PMU_REPAIR", PMU_REPAIR);
  uint32_t idle = showreg("PMU_BUS_IDLE", PMU_IDLE);
  uint32_t ack = showreg("PMU_BUS_ACK", PMU_ACK);
  uint32_t gate34 = showreg("CRU_GATE34", GATE34);
  uint32_t gate35 = showreg("CRU_GATE35", GATE35);
  uint32_t reset = showreg("CRU_RESET35", RESET35);

  if (!(repair & PHP_POWER_ON) || ((idle | ack) & PHP_BUS_IDLE))
    {
      puts("BLOCKED: PHP power domain is off or its bus is idle.");
      return 1;
    }

  if ((gate34 & PHP_ROOT_GATE) || (gate35 & HOST_GATE_MASK) ||
      (reset & HOST_RESET))
    {
      puts("BLOCKED: USB-A clock is gated or controller reset is asserted.");
      return 1;
    }

  puts("USB-A power/clock/reset prerequisites appear ready.");
  return 0;
}

static int probe(void)
{
  uint32_t id;
  uint32_t ctl;
  uint32_t cap;
  uint32_t params;
  unsigned int caplen;
  unsigned int ports;
  unsigned int i;
  uintptr_t op;

  /* Do not access the peripheral when its power/bus/clock prerequisites
   * indicate that register reads might not complete. No force override.
   */

  if (clocks() != 0)
    {
      puts("Controller registers were not accessed.");
      return 1;
    }

  id = showreg("DWC3_GSNPSID", HOST_BASE + 0xc120);
  if ((id >> 16) != 0x5533 && (id >> 16) != 0x3331 &&
      (id >> 16) != 0x3332)
    {
      puts("BLOCKED: unrecognized Synopsys controller signature.");
      return 1;
    }

  ctl = showreg("DWC3_GCTL", HOST_BASE + 0xc110);
  showreg("DWC3_GSTS", HOST_BASE + 0xc118);
  showreg("GHWPARAMS0", HOST_BASE + 0xc140);
  showreg("GHWPARAMS1", HOST_BASE + 0xc144);
  showreg("GHWPARAMS3", HOST_BASE + 0xc14c);
  showreg("GUSB2PHYCFG0", HOST_BASE + 0xc200);
  showreg("GUSB3PIPECTL0", HOST_BASE + 0xc2c0);
  if (((ctl >> 12) & 3) != 1 || (ctl & (UINT32_C(1) << 11)))
    {
      puts("BLOCKED: controller is not in host mode or is held in soft reset.");
      return 1;
    }

  cap = showreg("XHCI_CAP", HOST_BASE);
  params = showreg("XHCI_HCSP1", HOST_BASE + 4);
  caplen = cap & 0xff;
  ports = params >> 24;
  /* Bound all capability-derived register addresses within this block. */
  if (caplen < 0x20 || caplen > 0x80 || (caplen & 3) ||
      (cap >> 16) < 0x0090 || (cap >> 16) > 0x0120 ||
      ports == 0 || ports > 16 || (params & 0xff) == 0)
    {
      puts("BLOCKED: unexpected xHCI capabilities; no port reads attempted.");
      return 1;
    }

  op = HOST_BASE + caplen;
  showreg("XHCI_USBCMD", op);
  showreg("XHCI_USBSTS", op + 4);
  showreg("XHCI_PAGESIZE", op + 8);
  printf("xHCI revision=0x%04" PRIx32 " root_ports=%u slots=%" PRIu32 "\n",
         cap >> 16, ports, params & 0xff);
  for (i = 0; i < ports; i++)
    {
      uint32_t port = k7usb_read32(op + 0x400 + i * 0x10);
      printf("PORT%u 0x%08" PRIx32 " connected=%u enabled=%u "
             "powered=%u speed_code=%u\n", i + 1, port,
             (unsigned int)(port & 1), (unsigned int)((port >> 1) & 1),
             (unsigned int)((port >> 9) & 1),
             (unsigned int)((port >> 10) & 15));
    }

  puts("Register probe completed. Port connection is NOT camera enumeration.");
  puts("This build does not start xHCI, negotiate UVC, or capture frames.");
  return 0;
}

/* USB-C controller facts: pinned rk3576.dtsi / pm_domains.c /
 * clk-rk3576.c. Never access a powered-off or gated peripheral. */
static int otg_probe(void)
{
  uint32_t power=showreg("PMU_REPAIR",PMU_REPAIR);
  uint32_t idle=showreg("PMU_BUS_IDLE",PMU_IDLE);
  uint32_t ack=showreg("PMU_BUS_ACK",PMU_ACK);
  uint32_t gates=showreg("CRU_GATE47",CRU_BASE+0x8bc);
  uint32_t reset=showreg("CRU_RESET47",CRU_BASE+0xabc);
  if (!(power & (1u<<16)) || ((idle|ack)&(1u<<10)) ||
      (gates & ((1u<<1)|(7u<<5))) || (reset & (1u<<5)))
    {
      puts("USB-C BLOCKED: power/idle/clock/reset prerequisite missing; no controller reads");
      return 1;
    }
  uintptr_t base=0x23000000;
  uint32_t id=showreg("OTG_GSNPSID",base+0xc120);
  if ((id>>16)!=0x5533 && (id>>16)!=0x3331 && (id>>16)!=0x3332)
    { puts("USB-C BLOCKED: unexpected signature"); return 1; }
  showreg("OTG_GCTL",base+0xc110);
  showreg("OTG_HWPARAMS0",base+0xc140);
  showreg("OTG_HWPARAMS1",base+0xc144);
  showreg("OTG_HWPARAMS3",base+0xc14c);
  showreg("OTG_PHYCFG",base+0xc200);
  showreg("OTG_DCFG",base+0xc700);
  showreg("OTG_DCTL",base+0xc704);
  showreg("OTG_DSTS",base+0xc70c);
  showreg("PHY0_SUSPEND",0x2602e000);
  showreg("PHY0_ANALOG",0x2602e010);
  showreg("PHY0_STATUS",0x2602e080);
  showreg("OTG_DEPCMD0",base+0xc80c);
  puts("USB-C read-only probe completed; not enumeration or throughput validation");
  return 0;
}

int k7usb_main(int argc, char *argv[])
{
  if (argc == 2 && strcmp(argv[1], "clocks") == 0)
    {
      return clocks();
    }

  if (argc == 2 && strcmp(argv[1], "probe") == 0)
    {
      return probe();
    }

  if (argc == 2 && strcmp(argv[1], "otg") == 0) return otg_probe();
  puts("Usage: k7usb clocks | probe | otg");
  puts("KICKPI-K7 RK3576 USB-A: DWC3 0x23400000, SPI 260 / IRQ 292.");
  puts("Read-only diagnostics; use the usbdiag firmware configuration.");
  return argc == 1 ? 0 : 1;
}
