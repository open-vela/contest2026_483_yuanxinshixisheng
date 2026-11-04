/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "audio_preflight.h"

#if !defined(CONFIG_RK3576_USB_DIAG) || !defined(CONFIG_ARCH_BOARD_KICKPI_K7)
#error preflight requires the audited K7 CRU and IOC device mappings
#endif

int main(int argc, char **argv)
{
  uint32_t raw[K7_AUDIO_PREFLIGHT_REGS];
  struct k7_audio_clock_view view;
  if (argc != 2 || strcmp(argv[1], "preflight") != 0)
    {
      puts("usage: k7audio preflight (CRU/IOC read only)");
      return 1;
    }
  /* No I2C/SAI window access: their clocks/power have not been established.
   * Existing BSP maps these two ranges as Device-nGnRnE. */
  for (unsigned pass = 0; pass < 2; ++pass)
    {
      for (unsigned i = 0; i < K7_AUDIO_PREFLIGHT_REGS; ++i)
        {
          raw[i] = *(volatile const uint32_t *)k7_audio_preflight_addresses[i];
          printf("AUDIO_PREFLIGHT snapshot=%u address=%08" PRIxPTR " value=%08" PRIx32 "\n",
                 pass, k7_audio_preflight_addresses[i], raw[i]);
        }
      k7_audio_decode(raw, &view);
      printf("AUDIO_PREFLIGHT snapshot=%u i2c_parent=%u pclk_parent=%u "
             "i2c_gate_closed=%u pclk_gate_closed=%u root_gate_closed=%u sda_mux=%u scl_mux=%u\n",
             pass, view.i2c_parent, view.pclk_parent, view.i2c_gate_closed,
             view.pclk_gate_closed, view.root_gate_closed, view.sda_mux, view.scl_mux);
      printf("AUDIO_PREFLIGHT snapshot=%u div6_parent=%u div6_divisor=%u div6_gate_closed=%u "
             "div10_parent=%u div10_divisor=%u div10_gate_closed=%u "
             "div20_parent=%u div20_divisor=%u div20_gate_closed=%u\n",
             pass, view.div6_parent, view.div6_divisor, view.div6_gate_closed,
             view.div10_parent, view.div10_divisor, view.div10_gate_closed,
             view.div20_parent, view.div20_divisor, view.div20_gate_closed);
    }
  puts("AUDIO_PREFLIGHT result=0 writes=0 i2c_transactions=0 actual_rate_known=0 audio_ready=0");
  return 0;
}
