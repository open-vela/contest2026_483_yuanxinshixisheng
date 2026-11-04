/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_RK3576_DSI2_H
#define K7_RK3576_DSI2_H
#include "rk3576_dcphy.h"
#include <stddef.h>
struct k7_dsi2 {
  struct k7_display_io io;
  struct k7_dcphy phy;
  bool prepared, video, faulted, touched;
  int cleanup_error;
  /* First failed poll is retained even when shutdown succeeds. */
  uint32_t failed_poll_reg, failed_poll_value;
  int failed_poll_error;
};
/* Initialize a fresh object only. Do not init a live/touched object. */
int k7_dsi2_init(struct k7_dsi2 *, const struct k7_display_io *);
/* Fixed panel: 720x1280, 84MHz, H:32/15/32 V:21/15/12, 4lane,
 * RGB888, burst, VC0, LP commands, no EOT. Caller must verify VO0 domain,
 * PCLK_DSIHOST0/CLK_DSIHOST0 and PHY resources, MMIO, reset, exclusive use.
 * sys_hz is actual CLK_DSIHOST0 rate, not requested pixel clock.
 * Pixel input must be 84MHz/4=21MHz. VOP remains disabled during prepare. */
int k7_dsi2_prepare(struct k7_dsi2 *, uint32_t sys_hz, uint32_t ref_hz,
                    bool verified_resources);
/* Signature compatible with k7panel_io.tx. DCS writes only, <=255 bytes.
 * Success is bytes transmitted, not proof the glass displayed the command. */
int k7_dsi2_panel_tx(void *, unsigned char, const unsigned char *, size_t);
/* Call only after panel initialization and valid VOP VP1 input are ready. */
int k7_dsi2_video_enable(struct k7_dsi2 *, bool panel_and_vop_ready);
/* Switch video back to command mode BEFORE removing VOP input, preserving
 * PHY for panel exit DCS. On failure, caller still attempts other cleanup. */
int k7_dsi2_video_disable(struct k7_dsi2 *);
int k7_dsi2_shutdown(struct k7_dsi2 *);
#endif
