#ifndef VV_RTC_I2C2_H
#define VV_RTC_I2C2_H
#include "vv_rtc_reader.h"
#include <stddef.h>
struct vv_rtc_i2c2_io {
 uint32_t (*read32)(void *,uintptr_t);
 void (*write32)(void *,uintptr_t,uint32_t);
 uint64_t (*now_ms)(void *);
 void (*wait_us)(void *,unsigned);
 bool (*cancelled)(void *);
 void *arg;
};
/* Zero initialize exactly once. Do not reinitialize poisoned/live instances.
 * approved means all other I2C2 users are excluded AND security/mapping and
 * clock/pin configuration were approved. Caller serializes configuration. */
/* Linux rk3x_i2c_disable preserves CON[15:8] timing configuration. Only
 * those retained bits are accepted; low-byte state/command bits still block. */
#define VV_RTC_I2C2_CON_BUSY_MASK 0xffu
enum vv_rtc_i2c2_phase {
 VV_RTC_PHASE_NONE, VV_RTC_PHASE_PREFLIGHT, VV_RTC_PHASE_PCLK,
 VV_RTC_PHASE_CONTROLLER, VV_RTC_PHASE_SCLK_GATE, VV_RTC_PHASE_CONFIGURE,
 VV_RTC_PHASE_VERIFY, VV_RTC_PHASE_READY
};
enum vv_rtc_i2c2_block {
 VV_RTC_BLOCK_ROOT=1u, VV_RTC_BLOCK_BUS=2u, VV_RTC_BLOCK_RESET=4u,
 VV_RTC_BLOCK_SCL=8u, VV_RTC_BLOCK_SDA=16u, VV_RTC_BLOCK_CON=32u,
 VV_RTC_BLOCK_IEN=64u
};
/* Cached during the bounded prepare operation, never a new MMIO probe.
 * controller_valid is false until PCLK is confirmed enabled and reset clear. */
struct vv_rtc_i2c2_diag {
 unsigned phase,blocked;bool controller_valid;
 uint32_t root_select,root_gate,bus_gate,reset,scl,sda,pclk,con,ien;
};
struct vv_rtc_i2c2 {struct vv_rtc_i2c2_io io;volatile int owner;bool approved,poisoned,active;uint64_t last;struct vv_rtc_i2c2_diag diag;};
int vv_rtc_i2c2_prepare(struct vv_rtc_i2c2 *,bool platform_approved,uint64_t deadline_ms);
int vv_rtc_i2c2_platform_prepare(bool platform_approved,uint64_t deadline_ms);
int vv_rtc_i2c2_bind(struct vv_rtc_i2c2 *,struct vv_rtc_reader_ops *);
/* NuttX singleton. Default disabled; enabling requires the old rtc-status
 * path to be compiled out or routed here. Never enables clocks/pins itself. */
int vv_rtc_i2c2_platform_ops(bool approved,struct vv_rtc_reader_ops *);
#endif
