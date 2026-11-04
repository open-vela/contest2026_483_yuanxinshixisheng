#ifndef VV_RTC_READER_H
#define VV_RTC_READER_H
#include <stdbool.h>
#include <stdint.h>
/* Read-only candidate, no direct MMIO or clock/pinmux setup. All callbacks
 * return 0 or negative errno. Caller serializes a zero-initialized instance.
 * acquire is nonblocking and grants exclusive I2C2 ownership. read_register
 * must return within the absolute monotonic deadline and clean up STOP.
 * release must attempt bounded cleanup: on uncertain STOP it MUST poison and
 * retain hardware ownership, and return an error; never unlock a live bus.
 * The wrapper cannot make an unbounded hardware callback safe. */
struct vv_rtc_reader_ops {
 int (*acquire)(void *);
 int (*read_register)(void *,uint8_t register_index,uint8_t *,uint64_t deadline_ms);
 int (*release)(void *,uint64_t deadline_ms);
 uint64_t (*now_ms)(void *); /* UINT64_MAX means clock failure */
 bool (*cancelled)(void *);
 void *arg;
};
struct vv_rtc_reader {struct vv_rtc_reader_ops ops;bool entered;};
/* Two complete reads 0x00..0x08, equal effective fields, valid decode. Any
 * rollover/mismatch returns -EAGAIN; no automatic retry or clock write.
 * Callback accesses only HYM8563@0x51 on prevalidated I2C2; never CLKOUT0x0d.
 * On any error utc=0. Success proves stable valid registers, NOT trustworthy
 * provisioning or battery health beyond the decoded flags. Trust arm remains
 * a separate platform decision. The effective deadline is min(caller deadline, entry + 100 ms), including
 * cleanup; this prevents a long callback sequence from matching a later
 * seconds cycle. Cleanup shares that SAME effective deadline. */
int vv_rtc_reader_read(struct vv_rtc_reader *,uint64_t deadline_ms,uint64_t *utc);
/* Diagnostic only: one raw read of 00..08 without decode/trust or RTC writes.
 * Uses the same owner/100ms limit/release. count records valid prefix bytes
 * on partial failure. Does not touch any provisioning one-shot state. */
int vv_rtc_reader_raw(struct vv_rtc_reader *,uint64_t deadline_ms,uint8_t raw[9],unsigned *count);
#endif
