#ifndef VV_ADDRESSED_WINDOW_H
#define VV_ADDRESSED_WINDOW_H
#include <stdbool.h>
#include <stdint.h>
/* Admission policy, not speaker identification. Service-owner access only.
 * PHOTO and an existing reply/handoff retain their existing lifecycle. */
#define VV_ADDRESSED_IDLE_MS 40000u
static inline bool vv_addressed_window_expired(bool awake, bool occupied,
                                               uint64_t last, uint64_t now) {
 return awake && !occupied &&
  (now == UINT64_MAX || now < last || now-last >= VV_ADDRESSED_IDLE_MS);
}
#endif
