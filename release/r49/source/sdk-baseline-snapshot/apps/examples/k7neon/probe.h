#ifndef NEON_PROBE_H
#define NEON_PROBE_H
#include <stdint.h>
#define PROBE_LANES 16
#define PROBE_STEPS 32
/* Returns completed additions, 0..32; callback nonzero stops early. */
unsigned neon_probe_hold(const uint32_t in[16], uint32_t out[16],
                         int (*checkpoint)(void *), void *context);
/* Integer-only IEEE754 encoding permits -mgeneral-regs-only in coordinator.
 * Input domain here is exactly 0..815; no float C operations in target file. */
static inline uint32_t probe_bits(unsigned value)
{
  if (!value) return 0;
  unsigned exponent = 0;
  for (unsigned n = value; n > 1; n >>= 1) ++exponent;
  return ((exponent + 127u) << 23) |
         ((value - (1u << exponent)) << (23 - exponent));
}
static inline void probe_input(unsigned id, uint32_t in[16])
{
  for (unsigned i = 0; i < 16; ++i) in[i] = probe_bits(id * 256 + i);
}
static inline int probe_match(unsigned id, unsigned steps, const uint32_t out[16])
{
  if (steps > PROBE_STEPS) return 0;
  for (unsigned i = 0; i < 16; ++i)
    if (out[i] != probe_bits(id * 256 + i + steps)) return 0;
  return 1;
}
#endif
