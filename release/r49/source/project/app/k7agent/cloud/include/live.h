/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef I2C3_LIVE_H
#define I2C3_LIVE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
struct i3_port {
 void *ctx;
 uint32_t (*read)(void *, uintptr_t);
 void (*write)(void *, uintptr_t, uint32_t);
 uint64_t (*us)(void *);
 int (*lock)(void *); /* nonblocking, common physical-bus owner */
 void (*unlock)(void *);
 int (*lines)(void *); /* 1=both lines high; 0=low; negative=unknown */
};
struct i3_live { struct i3_port p; bool ready,held,active,poisoned,timed; uint32_t tuning; };
/* Zero-initialized instance. Root attests clocks 24MHz, mux, reset, rails,
 * IRQ excluded, common ownership, and acceptance of standard-mode defaults.
 * No clock/pin/reset/GIC writes here. Ready init has no MMIO. */
int i3_init(struct i3_live *, const struct i3_port *, bool prepared);
/* Programs 24MHz/100kHz using official default 1000/300ns engineering bounds.
 * Root calls once after prepare, before transfers; not a measured timing. */
int i3_timing(struct i3_live *);
/* 7-bit codec address 0x10 only; one 8-bit register / one data byte.
 * timeout 1..100000us, plus separate bounded 10000us STOP cleanup.
 * Failed write can have device-side effects; never retry automatically. */
int i3_xfer(struct i3_live *, bool read, uint8_t reg, uint8_t *value, uint32_t timeout_us);
/* Held on uncertain cleanup. May be retried by same supervising owner;
 * success releases bus but poison remains; no implicit reinit/retry. */
int i3_cleanup(struct i3_live *);
/* Pure decode, caller must establish GPIO4 PCLK/input path first. */
int i3_gpio_lines(uint32_t version, uint32_t ext);
#endif
