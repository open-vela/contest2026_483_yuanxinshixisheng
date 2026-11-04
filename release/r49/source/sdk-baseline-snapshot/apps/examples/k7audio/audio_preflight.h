/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_AUDIO_PREFLIGHT_H
#define K7_AUDIO_PREFLIGHT_H
#include <stdint.h>
#define K7_AUDIO_PREFLIGHT_REGS 8u
/* CLKSEL0,1,55,57; CLKGATE0,11,12; GPIO4B4/B5 IOC. */
extern const uintptr_t k7_audio_preflight_addresses[K7_AUDIO_PREFLIGHT_REGS];
struct k7_audio_clock_view
{
  unsigned i2c_parent, pclk_parent;
  unsigned i2c_gate_closed, pclk_gate_closed, root_gate_closed;
  unsigned sda_mux, scl_mux;
  unsigned div6_parent, div6_divisor, div6_gate_closed;
  unsigned div10_parent, div10_divisor, div10_gate_closed;
  unsigned div20_parent, div20_divisor, div20_gate_closed;
};
void k7_audio_decode(const uint32_t raw[K7_AUDIO_PREFLIGHT_REGS],
                     struct k7_audio_clock_view *view);
#endif
