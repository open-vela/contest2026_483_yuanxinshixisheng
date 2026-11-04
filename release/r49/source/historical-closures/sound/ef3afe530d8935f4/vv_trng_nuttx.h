#ifndef VV_TRNG_NUTTX_H
#define VV_TRNG_NUTTX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
/* Explicit authorization attests mapped CRU/TRNG_NS access, exclusive hardware
 * ownership, upstream parent clock/reset/firewall verification. It does NOT
 * substitute for the external health approval callback. Default is disabled.
 * No CRU/reset writes, and no automatic recovery after an error. */
int vv_trng_nuttx_configure(bool platform_approved,
                          bool (*health_approved)(void *),void *health_arg);
ssize_t vv_trng_nuttx_read_exact(void *unused,uint8_t *,size_t,uint64_t deadline_ms);
int vv_trng_nuttx_disable(void);
/* Separate explicit development NS-source policy. No assertion about secure
 * world exclusivity/health certification. Reserves wrapper for this boot;
 * production configure/read/disable cannot bypass this reservation. policy is
 * a non-NULL stable owner cookie; callback is dynamic and may admit probing.
 * No clock/reset/firewall writes and no automatic retry. */
int vv_trng_nuttx_development_configure(bool (*admitted)(void *),void *policy);
ssize_t vv_trng_nuttx_development_read_exact(void *policy,uint8_t *,size_t,uint64_t);
#endif
