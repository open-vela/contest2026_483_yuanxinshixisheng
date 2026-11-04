#ifndef K7LOAD_WORKLOAD_H
#define K7LOAD_WORKLOAD_H
#include <stdint.h>
/* Volatile prevents the compiler replacing this deterministic load with its
 * known result. Integer operations have defined uint32_t wrapping semantics. */
static inline uint32_t k7load_batch(void)
{
  volatile uint32_t value = UINT32_C(0x12345678);
  for (unsigned i = 0; i < 4096; i++)
    {
      value ^= value << 13;
      value ^= value >> 17;
      value ^= value << 5;
    }
  return value;
}
#define K7LOAD_EXPECTED UINT32_C(0xf6e37410)
#endif
