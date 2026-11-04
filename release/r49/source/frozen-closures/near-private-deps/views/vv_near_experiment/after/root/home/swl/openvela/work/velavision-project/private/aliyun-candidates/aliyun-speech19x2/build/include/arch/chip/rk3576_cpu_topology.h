/* K7 captured-DTB topology candidate. Not enabled in the BSP. */
#ifndef VV_K7_TOPOLOGY_H
#define VV_K7_TOPOLOGY_H
#include <stdint.h>

/* Ignore MPIDR non-affinity flags, retain all four affinity levels. */
#define VV_MPIDR_AFFINITY_MASK UINT64_C(0xff00ffffff)

static inline int vv_k7_logical_cpu(uint64_t mpidr, unsigned enabled)
{
  uint64_t affinity = mpidr & VV_MPIDR_AFFINITY_MASK;
  unsigned core = (unsigned)(affinity & 255);
  unsigned cluster = (unsigned)((affinity >> 8) & 255);
  unsigned cpu;
  if (enabled == 0 || enabled > 8 || (affinity >> 16) != 0 ||
      core > 3 || cluster > 1)
    return -1;
  cpu = cluster * 4 + core;
  return cpu < enabled ? (int)cpu : -1;
}

static inline int vv_k7_cpu_affinity(unsigned cpu, unsigned enabled,
                                    uint64_t *affinity)
{
  if (!affinity || enabled == 0 || enabled > 8 || cpu >= enabled)
    return -1;
  *affinity = ((uint64_t)(cpu / 4) << 8) | (cpu % 4);
  return 0;
}
#endif
