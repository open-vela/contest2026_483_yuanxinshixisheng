#ifndef VV_RTC_TIME_H
#define VV_RTC_TIME_H
#include <stdint.h>
#include <stdbool.h>
/* Decode one sequential HYM8563 register snapshot 0x00..0x08.
 * Returns a UTC candidate only: successful decoding does NOT establish prior
 * trusted provisioning, correct time, battery retention, or resistance to
 * tampering. Never arm TLS trust from this result alone.
 * Reject STOP/test mode, low-voltage flag and invalid BCD/calendar. Ignore
 * unused bits and the unreliable HYM century bit, as Linux rtc-hym8563 does;
 * the supported provisioning epoch is explicitly 2000..2099. */
int vv_rtc_hym8563_decode(const uint8_t registers[9], uint64_t *utc_seconds);
/* Compare only effective clock/calendar bits; CTL2 and unused/century bits
 * do not define this 2000..2099 calendar. Includes VL and CTL1 mode bits. */
bool vv_rtc_hym8563_same_fields(const uint8_t a[9],const uint8_t b[9]);
#endif
