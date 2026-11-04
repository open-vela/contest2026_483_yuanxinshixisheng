/* SPDX-License-Identifier: Apache-2.0 */
/* Field locations from the user's frozen RK3576 SDK clk-rk3576.c and pinctrl.
 * Decode only: parent labels are not measured or guaranteed clock rates. */
#include "audio_preflight.h"
const uintptr_t k7_audio_preflight_addresses[K7_AUDIO_PREFLIGHT_REGS] =
{
  0x27200300u, 0x27200304u, 0x272003dcu, 0x272003e4u,
  0x27200800u, 0x2720082cu, 0x27200830u, 0x2604408cu
};
void k7_audio_decode(const uint32_t raw[K7_AUDIO_PREFLIGHT_REGS],
                     struct k7_audio_clock_view *v)
{
  v->i2c_parent = (raw[3] >> 4) & 3u;
  v->pclk_parent = (raw[2] >> 2) & 3u;
  v->i2c_gate_closed = (raw[6] >> 14) & 1u;
  v->pclk_gate_closed = (raw[6] >> 2) & 1u;
  v->root_gate_closed = (raw[5] >> 1) & 1u;
  v->sda_mux = raw[7] & 15u;
  v->scl_mux = (raw[7] >> 4) & 15u;
  v->div6_parent = (raw[1] >> 11) & 1u;
  v->div6_divisor = ((raw[1] >> 6) & 31u) + 1u;
  v->div6_gate_closed = (raw[4] >> 3) & 1u;
  v->div10_parent = (raw[0] >> 11) & 1u;
  v->div10_divisor = ((raw[0] >> 6) & 31u) + 1u;
  v->div10_gate_closed = (raw[4] >> 1) & 1u;
  v->div20_parent = (raw[0] >> 5) & 1u;
  v->div20_divisor = (raw[0] & 31u) + 1u;
  v->div20_gate_closed = raw[4] & 1u;
}
