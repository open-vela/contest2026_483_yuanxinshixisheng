/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K7_DISPLAY_SESSION_H
#define K7_DISPLAY_SESSION_H
#include "f050008m01.h"
#include "rk3576_panel_board.h"
#include "rk3576_dsi2.h"
#include "rk3576_vop.h"

/* The platform owns the buffer and resource lease until stop succeeds.
 * This object must be zero-initialized once and externally serialized.
 * No implicit retry, clock inference, allocation or framebuffer address. */
struct k7display_session {
  struct k7pb_state panel;
  struct k7pb_io panel_io;
  struct k7_dsi2 dsi;
  struct k7vop vop;
  int started, touched, primary_error, cleanup_error;
  /* Historical first cleanup error is sticky; last_stop_error is the latest
   * explicit stop result. A successful later stop must not erase history. */
  int last_stop_error;
  unsigned cleanup_failures;
  /* 1=panel prepare, 2=reset, 3=link, 4=DCS, 5=VOP configure,
   * 6=VOP enable, 7=video, 8=backlight. Retained across cleanup. */
  unsigned start_stage;
};
struct k7display_platform {
  struct k7pb_io panel;
  struct k7_display_io link;
  struct k7vop_io vop;
  struct k7vop_frame frame;
  void *arg;
  /* Verify freshly acquired exclusive resources, all parent domains/clocks,
   * 24MHz PHY reference, 84MHz VP1 and reserved/cache-safe frame. Missing
   * implementation is -ENOSYS, not permission to assume ready. No MMIO from
   * any other module occurs before this call succeeds. */
  int (*preflight)(void *, unsigned *panel_proofs, uint32_t *dsi_sys_hz);
};
int k7display_start(struct k7display_session *, const struct k7display_platform *,
                    unsigned brightness);
int k7display_stop(struct k7display_session *);
#endif
