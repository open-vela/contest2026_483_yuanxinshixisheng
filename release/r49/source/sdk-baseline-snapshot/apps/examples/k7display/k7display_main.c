/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include "f050008m01.h"
#include "rk3576_display_resources.h"
#include "display_session.h"
#include "rk3576_display_native.h"
#include "rk3576_display_transition.h"
/* Pace independent lines so the diagnostic UART can drain under SMP load. */
static void diag_line(const char *format, ...)
  __attribute__((format(printf, 1, 2)));
static void diag_line(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  fflush(stdout);
  usleep(20000);
}
/* IOC/CRU/PMU_CRU are mapped by the Kconfig-required USB diagnostics.
 * Never read the unverified GPIO/PWM/DSI address ranges here. */
static unsigned read32(uintptr_t address)
{
  return *(volatile const uint32_t *)address;
}
static int resource_read(void *arg, uintptr_t address, uint32_t *value)
{
  (void)arg;
  *value = read32(address);
  return 0;
}
static int probe(void)
{
  unsigned a=read32(0x26040000), b=read32(0x26042000);
  unsigned c=read32(0x26042008), gate=read32(0x27200840);
  unsigned select=read32(0x2720041c), gpio_gate=read32(0x2722081c);
  diag_line("K7DISPLAY reset_mux=%u power_mux=%u backlight_mux=%u expected=0,0,12\n",
         (a>>8)&15,(c>>8)&15,(b>>4)&15);
  diag_line("K7DISPLAY pwm_pclk_gated=%u pwm_func_gated=%u pwm_source=%u gpio0_pclk_gated=%u\n",
         (gate>>10)&1,(gate>>11)&1,(select>>8)&3,(gpio_gate>>6)&1);
  diag_line("K7DISPLAY probe_only=1 hardware_writes=0 supply_voltage_verified=0 panel_ready=0\n");
  /* Raw snapshot only: do not attest clock stability, upstream topology or
   * DMA setup. These observations alone cannot authorize initialization. */
  for (unsigned i = 0; i < K7DR_REG_COUNT; ++i) {
    uint32_t value;
    uintptr_t address = k7dr_address((enum k7dr_reg)i);
    resource_read(NULL, address, &value);
    diag_line("K7DISPLAY resource[%u] address=%08lx value=%08lx\n",
           i, (unsigned long)address, (unsigned long)value);
  }
  /* Additional shared-domain/PLL-consumer state: only always-on PMU/CRU,
   * never an unpowered display or QoS register bank. */
  for (unsigned i = 0; i < K7DT_EXTRA_COUNT; ++i) {
    uintptr_t address = k7dt_probe_address((enum k7dt_extra_reg)i);
    diag_line("K7DISPLAY extra[%u] address=%08lx value=%08x\n",
           i, (unsigned long)address, read32(address));
  }
  return k7display_native_probe();
}
/* Decimal index only. The allowlist is the same AON register table as probe;
 * no caller-provided address reaches read32(). */
static int indexed_probe(const char *text, int extra)
{
  unsigned index = 0;
  unsigned count = extra ? K7DT_EXTRA_COUNT : K7DR_REG_COUNT;
  uintptr_t address;
  const char *p = text;
  if (!p || !*p) return 2;
  for (; *p; ++p) {
    if (*p < '0' || *p > '9') return 2;
    index = index * 10u + (unsigned)(*p - '0');
    if (index >= count) return 2;
  }
  address = extra ? k7dt_probe_address((enum k7dt_extra_reg)index) :
                    k7dr_address((enum k7dr_reg)index);
  if (!address) return 2;
  diag_line("K7DISPLAY %s[%u] address=%08lx value=%08x\n",
            extra ? "extra" : "resource", index,
            (unsigned long)address, read32(address));
  return 0;
}
int k7display_main(int argc, char **argv)
{
  if (argc==3 && !strcmp(argv[1],"reg")) return indexed_probe(argv[2],0);
  if (argc==3 && !strcmp(argv[1],"extra")) return indexed_probe(argv[2],1);
  if (argc==2 && !strcmp(argv[1],"probe")) return probe();
  if (argc==2 && !strcmp(argv[1],"colorbars")) return k7display_native_start();
  if (argc==2 && !strcmp(argv[1],"stop")) return k7display_native_stop();
  if (argc==2 && !strcmp(argv[1],"blank")) return k7display_native_blank(1);
  if (argc==2 && !strcmp(argv[1],"unblank")) return k7display_native_blank(0);
  if (argc==2 && !strcmp(argv[1],"status")) return k7display_native_status();
  if (argc!=2 || strcmp(argv[1],"info")) {
    diag_line("k7display info | probe | status | reg <0..35> | extra <0..14> (read-only)\n");
    diag_line("k7display colorbars (one attempt) | stop | blank | unblank\n");
    return 1;
  }
  int count=k7panel_validate(k7panel_init,k7panel_init_size);
  int exit_count=k7panel_validate(k7panel_exit,k7panel_exit_size);
  diag_line("K7DISPLAY panel=F050008M01 ic=FT8756M width=%u height=%u lanes=4 rgb=888 pclk=%u\n",
         K7PANEL_WIDTH,K7PANEL_HEIGHT,K7PANEL_PIXEL_CLOCK);
  diag_line("K7DISPLAY init_commands=%d exit_commands=%d backend_ready=0 info_hardware_writes=0\n",count,exit_count);
  /* Retain the candidate implementation in diagnostic ELF for symbol audit;
   * this exposes an address, never invokes it or grants hardware readiness. */
  diag_line("K7DISPLAY candidate_entry=%p platform_bound=1 hardware_tested=0 auto_start=0\n",
         (void *)(uintptr_t)k7display_start);
  return count==171 && exit_count==2 ? 0 : 1;
}
