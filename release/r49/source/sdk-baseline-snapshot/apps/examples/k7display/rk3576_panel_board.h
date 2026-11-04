/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RK3576_PANEL_BOARD_H
#define RK3576_PANEL_BOARD_H
#include <stdint.h>

/* Original implementation from RK3576 TRM Part1 v1.2 (20240624), GPIO ch21,
 * PWM ch34; KICKPI F050008M01 DTS. No Linux driver code is copied.
 * The caller must own/serialize these pins, GPIO0 and PWM1 clock changes and
 * channel 1, including all consumers affected by PWM1's shared source mux.
 * No bootloader display handoff: only an explicitly quiesced panel is allowed.
 * Caller maps IOC 26040000, CRU 27200000, PMU_CRU 27220000,
 * GPIO0 27320000 and PWM1_CH1 2add1000 as device memory; verifies parent
 * clocks, 24MHz crystal and upstream LCD 3.3V supply before granting proofs.
 * MMIO callbacks provide device barriers and return 0 or negative errno.
 * delay_ms must not return early; neither MMIO nor delay is called implicitly.
 */
#define K7PB_MAPPED        1u
#define K7PB_EXCLUSIVE     2u
#define K7PB_PARENTS_READY 4u
#define K7PB_QUIESCED      8u
#define K7PB_ALL_PROOFS   15u
struct k7pb_io {
  void *arg;
  int (*read)(void *, uintptr_t, uint32_t *);
  int (*write)(void *, uintptr_t, uint32_t);
  int (*delay_ms)(void *, unsigned int);
};
enum k7pb_phase { K7PB_OFF, K7PB_POWERED, K7PB_RESET_DONE, K7PB_LIT,
                  K7PB_FAULT };
/* Zero initialize once. No automatic retry after FAULT; inspect evidence,
 * explicitly call off and re-grant ownership before another prepare. */
struct k7pb_state {
  enum k7pb_phase phase;
  unsigned int proofs, touched, pwm_accessible;
  int primary_error, cleanup_error, last_cleanup_error;
};
/* prepare leaves reset asserted/backlight low; reset releases panel for DSI.
 * DSI commands and display scanout are NOT implemented by this module. */
int k7pb_prepare(const struct k7pb_io *, struct k7pb_state *, unsigned proofs);
int k7pb_reset(const struct k7pb_io *, struct k7pb_state *);
/* percent 0..100. Nonzero requires caller proof of completed panel init and
 * valid scanout. Zero only disables backlight; does not reset/power off. */
int k7pb_backlight(const struct k7pb_io *, struct k7pb_state *,
                  unsigned percent, int panel_and_video_ready);
/* Caller stops scanout/transfers and sends panel exit DCS first when possible.
 * On failure, best effort shutdown continues and cleanup_error is preserved.
 * off never restores former high pin values or shared clock settings. */
int k7pb_off(const struct k7pb_io *, struct k7pb_state *);
/* Visibility only, for this owner's already configured LIT session. blank=1
 * drives the existing backlight GPIO low; blank=0 restores its PWM mux after
 * checking the retained PWM configuration. Caller serializes with start/stop
 * and scanout. No clock, power, reset, DMA or session-state changes, including
 * on error. LIT denotes configured PWM, not current visibility. An error means
 * visibility is unknown; it must not be reported as a successful blank. */
int k7pb_blank(const struct k7pb_io *, const struct k7pb_state *, int blank);
#endif
