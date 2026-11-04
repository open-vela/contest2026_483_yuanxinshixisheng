#ifndef K7_CAPTURE_LIFETIME_H
#define K7_CAPTURE_LIFETIME_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
/* Reserved for boot-owned service entry; ordinary CLI duration stays bounded. */
#define K7_CAPTURE_CONTINUOUS UINT_MAX
static inline bool k7_capture_refill(unsigned seconds, uint64_t elapsed_ms,
                                     bool stop_requested, bool connected)
{
  return connected && (seconds == K7_CAPTURE_CONTINUOUS ? !stop_requested :
                        elapsed_ms < (uint64_t)seconds * 1000);
}
#endif
