/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K7_I2C3_TIMING_H
#define K7_I2C3_TIMING_H
#include <stdint.h>
#include <stdbool.h>
struct k7_i2c_timing_request {
 uint32_t input_hz,bus_hz,scl_rise_ns,scl_fall_ns;
 bool inputs_known; /* Explicit caller evidence; no defaults or probing. */
};
struct k7_i2c_timing {
 uint16_t div_low,div_high;
 uint32_t clkdiv,tuning;
 uint32_t scl_hz_ceiling;
};
/* Standard mode only (1..100 kHz). No MMIO. Output untouched on failure.
 * Pure candidate based on official RK3x v1 calculation, stricter rejection. */
int k7_i2c3_timing(const struct k7_i2c_timing_request *,struct k7_i2c_timing *);
#endif
