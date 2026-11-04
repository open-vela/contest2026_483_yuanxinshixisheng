#ifndef VV_RTC_PROVISION_I2C2_H
#define VV_RTC_PROVISION_I2C2_H
/* Bind only after the existing platform diagnostic prepared the singleton. */
struct vv_rtc_provision_ops;
int vv_rtc_i2c2_platform_provision_ops(struct vv_rtc_provision_ops *);
#include "vv_rtc_provision.h"
#include "vv_rtc_i2c2.h"
/* Binds to the EXISTING, prepared RTC bus object, not another singleton.
 * Uses its exact owner/active/poison state and its existing read/acquire/release
 * callbacks. No clocks, pins, reset, address scans, automatic retry or MMIO
 * happen during bind. Caller must serialize and exclude the legacy owner. */
int vv_rtc_provision_i2c2_bind(struct vv_rtc_i2c2 *,struct vv_rtc_provision_ops *);
#endif
