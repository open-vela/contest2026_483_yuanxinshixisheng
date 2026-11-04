#ifndef VV_RTC_PROVISION_H
#define VV_RTC_PROVISION_H
#include "vv_rtc_reader.h"
#include <stdint.h>

#define VV_RTC_PROVISION_MIN_EPOCH UINT64_C(946684800)
#define VV_RTC_PROVISION_MAX_EPOCH UINT64_C(4102444799)

/* Pure UTC encoder: 2000-01-01 through 2099-12-31, inclusive. Output is
 * exactly SEC..YEAR (02..08); no control, alarm or CLKOUT register content.
 * On error a non-null output is zeroed. Weekday is Sunday=0. */
int vv_rtc_hym8563_encode(uint64_t epoch, uint8_t calendar[7]);

struct vv_rtc_provision_ops {
 struct vv_rtc_reader_ops reader;
 /* One fixed HYM8563@0x51 transaction: pointer=02 and seven data bytes.
  * No caller-selectable address/count or retries. Must finish bus STOP
  * within deadline, poison/retain owner if uncertain, and return 0 only on
  * complete ACK+STOP. Called only while RTC CTL1 STOP is asserted. */
 int (*write_calendar)(void *,const uint8_t calendar[7],uint64_t deadline_ms);
 /* Fixed CTL1 register only: set STOP then restore/normalize per policy.
  * Called for cleanup even after cancellation; must still obey deadline. */
 int (*write_control1)(void *,uint8_t value,uint64_t deadline_ms);
 /* No MMIO: latch poison and retain the same hardware owner. Required when
  * required cleanup CTL1 could not be confirmed; release retains ownership. */
 void (*poison)(void *);
};
struct vv_rtc_provision {
 struct vv_rtc_provision_ops ops;
 /* Caller serializes this one-shot instance; never clear attempted to retry
  * an uncertain write. It is consumed before acquire for a valid request. */
 bool attempted;
};
struct vv_rtc_provision_result {
 uint8_t readback[2][9];unsigned readback_count[2];
 bool initial_control1_valid,initial_seconds_valid,initialize_requested;
 uint8_t initial_control1,initial_seconds,control_blocked;
 bool initial_vl;
 bool stop_attempted,stop_acked,restore_attempted,restored;
 bool write_attempted,write_acked,readback_valid;
 bool rollback_attempted,running_verified;
 int operation_rc,restore_rc,rollback_rc,cleanup_rc;
 uint64_t observed_epoch;
};
/* Explicit epoch and authorization, no wall-clock lookup or network time.
 * All operations and cleanup share min(deadline, entry+100ms). Reads only
 * 00..08; only CTL1 STOP/restore and one calendar write 02..08. Reject
 * stopped/test RTC before write; records prior VL, which this explicit
 * provisioning may repair. Readback must have VL clear. Two
 * post-write snapshots must match effective fields, decode, and equal epoch
 * or its next second (running clock). No retries on any error/rollover.
 * Success establishes only immediate readback, NOT trust, battery retention,
 * epoch-source authenticity, or permission to arm TLS. */
int vv_rtc_provision_once(struct vv_rtc_provision *,bool authorized,
                          uint64_t epoch,uint64_t deadline_ms,
                          struct vv_rtc_provision_result *);
/* Strict unsigned decimal Unix UTC seconds; no signs, whitespace, suffixes,
 * implicit local timezone, or system-clock fallback. Performs no MMIO. */
int vv_rtc_provision_parse_epoch(const char *,uint64_t *);
/* CLI-facing entry; parse failure has zero hardware effects. Caller supplies
 * the already-bound one-shot instance and explicit absolute deadline. */
int vv_rtc_provision_cli(struct vv_rtc_provision *,bool authorized,
                         const char *epoch,uint64_t deadline_ms,
                         struct vv_rtc_provision_result *);
/* Explicit normal-mode initialization: permits POR TESTC and STOP, rejects
 * TEST1 external-clock mode. Writes CTL1=0x20, calendar, CTL1=0. Reserved bits
 * are zero as required by the datasheet. Cleanup preserves the original STOP
 * state with TESTC/reserved bits cleared; it never re-enables POR override.
 * A failed start can perform one bounded rollback, never a calendar retry. */
int vv_rtc_provision_initialize_cli(struct vv_rtc_provision *,bool authorized,
                         const char *epoch,uint64_t deadline_ms,
                         struct vv_rtc_provision_result *);
#endif
