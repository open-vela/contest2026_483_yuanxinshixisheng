/* SPDX-License-Identifier: GPL-2.0-only */
#include "display_session.h"
#include <errno.h>

static int panel_delay(void *arg, unsigned ms)
{
  struct k7display_session *s = arg;
  return s->panel_io.delay_ms(s->panel_io.arg, ms);
}
static int panel_tx(void *arg, unsigned char type,
                    const unsigned char *bytes, size_t size)
{
  struct k7display_session *s = arg;
  return k7_dsi2_panel_tx(&s->dsi, type, bytes, size);
}
static void first_error(int *first, int ret)
{
  if (ret < 0 && !*first) *first = ret;
}
static void cleanup_result(struct k7display_session *s, int *current,
                           unsigned stage, int ret)
{
  if (ret < 0) s->cleanup_failures |= stage;
  first_error(current, ret);
  first_error(&s->cleanup_error, ret);
}
int k7display_stop(struct k7display_session *s)
{
  struct k7panel_io io;
  int error = 0;
  if (!s) return -EINVAL;
  if (!s->touched) return 0;
  s->started = 0;
  first_error(&s->cleanup_error, s->panel.cleanup_error);
  first_error(&s->cleanup_error, s->dsi.cleanup_error);
  first_error(&s->cleanup_error, s->dsi.phy.cleanup_error);
  /* Darken before stopping scanout; still attempt all shutdown stages when
   * an earlier stage fails. Never free the framebuffer on uncertain stop. */
  if (s->panel.phase == K7PB_RESET_DONE || s->panel.phase == K7PB_LIT)
    cleanup_result(s, &error, 1u, k7pb_backlight(&s->panel_io, &s->panel, 0, 0));
  if (s->dsi.video)
    cleanup_result(s, &error, 2u, k7_dsi2_video_disable(&s->dsi));
  if (s->vop.touched)
    cleanup_result(s, &error, 4u, k7vop_disable(&s->vop));
  io.tx = panel_tx; io.delay_ms = panel_delay; io.arg = s;
  if (s->dsi.prepared && !s->dsi.faulted)
    cleanup_result(s, &error, 8u, k7panel_run(k7panel_exit, k7panel_exit_size, &io));
  cleanup_result(s, &error, 16u, k7_dsi2_shutdown(&s->dsi));
  if (s->panel.touched)
    cleanup_result(s, &error, 32u, k7pb_off(&s->panel_io, &s->panel));
  s->last_stop_error = error;
  if (!error) s->touched = 0;
  return error;
}
int k7display_start(struct k7display_session *s,
                    const struct k7display_platform *p, unsigned brightness)
{
  unsigned proofs = 0;
  uint32_t sys_hz = 0;
  int ret;
  struct k7panel_io commands;
  if (!s || !p || !p->preflight || brightness > 100) return -EINVAL;
  if (s->touched || s->started || s->primary_error) return -EBUSY;
  ret = k7panel_validate(k7panel_init, k7panel_init_size);
  if (ret != 171) return ret < 0 ? ret : -EINVAL;
  ret = k7panel_validate(k7panel_exit, k7panel_exit_size);
  if (ret != 2) return ret < 0 ? ret : -EINVAL;
  ret = k7vop_validate_frame(&p->frame);
  if (ret < 0) return ret;
  ret = k7_dsi2_init(&s->dsi, &p->link);
  if (ret < 0) return ret;
  ret = k7vop_init(&s->vop, &p->vop);
  if (ret < 0) return ret;
  if (!p->panel.read || !p->panel.write || !p->panel.delay_ms) return -EINVAL;
  ret = p->preflight(p->arg, &proofs, &sys_hz);
  if (ret < 0) return ret;
  if (proofs != K7PB_ALL_PROOFS || !sys_hz) return -EACCES;
  s->panel_io = p->panel;
  s->touched = 1;
#define STAGE(call) do { ++s->start_stage; ret = (call); if (ret < 0) goto fail; } while (0)
  STAGE(k7pb_prepare(&s->panel_io, &s->panel, proofs));
  STAGE(k7pb_reset(&s->panel_io, &s->panel));
  STAGE(k7_dsi2_prepare(&s->dsi, sys_hz, 24000000u, 1));
  commands.tx = panel_tx; commands.delay_ms = panel_delay; commands.arg = s;
  STAGE(k7panel_run(k7panel_init, k7panel_init_size, &commands));
  STAGE(k7vop_configure(&s->vop, &p->frame));
  /* Vendor enable order: CRTC scanout, connector video mode, backlight.
   * Backlight waits for a newly observed frame-start, never a stale flag. */
  STAGE(k7vop_enable(&s->vop, 200000));
  STAGE(k7_dsi2_video_enable(&s->dsi, 1));
  STAGE(k7pb_backlight(&s->panel_io, &s->panel, brightness, 1));
  s->started = 1;
  return 0;
fail:
  s->primary_error = ret;
  k7display_stop(s);
  return ret;
#undef STAGE
}
