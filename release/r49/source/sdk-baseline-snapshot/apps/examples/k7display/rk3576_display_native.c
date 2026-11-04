/* SPDX-License-Identifier: GPL-2.0-only */
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
#include "display_session.h"
#include "rk3576_display_transition.h"
#include "rk3576_display_framebuffer.h"
#include "rk3576_display_native_io.h"
#include "rk3576_display_native.h"
#include "rk3576_display_guard.h"

static void diag_line(const char *format, ...)
  __attribute__((format(printf, 1, 2)));
static void diag_line(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  fflush(stdout);
  usleep(20000);
}

static mutex_t g_lock = NXMUTEX_INITIALIZER;
static struct k7dn_io_state g_io;
static struct k7display_session g_session;
static struct k7fb g_frame;
static struct k7dt_resource_report g_resources;
static struct k7dg_report g_guard;
static unsigned g_attempted;
static int g_error;
static int g_blanked = -1, g_backlight_error;
static unsigned g_video_frames, g_video_errors;
static uint64_t g_prepare_us, g_commit_us, g_max_prepare_us, g_max_commit_us;
static uint64_t video_usec(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000u;
}

static int live_resources(struct k7dr_report *report)
{
  const struct k7dr_io io = {&g_io,k7dn_read};
  if (!g_resources.ready || !g_io.bus_ready) return -EACCES;
  return k7dr_check(&io,K7DR_ALL_PROOFS,100000000u,report);
}
static int ready(void *arg, const struct k7vop_frame *frame, uint32_t hz)
{
  struct k7dr_report r;
  uint32_t mmu[2];
  int ret;
  (void)arg;
  if (!frame || k7fb_check_frame(&g_frame,frame) || hz != K7VOP_PIXEL_HZ ||
      !g_frame.leased || !g_frame.armed) return -EACCES;
  ret = live_resources(&r);
  if (!ret) ret = k7dn_iommu_bypass(&g_io,mmu);
  return ret;
}
static int publish(void *arg, const struct k7vop_frame *frame)
{ (void)arg; return k7fb_publish(&g_frame,frame); }
static int preflight(void *arg, unsigned *panel_proofs, uint32_t *sys_hz)
{
  const struct k7dt_io io = {&g_io,k7dn_read,k7dn_write,
                            k7dn_barrier,k7dn_delay_us};
  struct k7dr_report r;
  uint32_t mmu[2];
  int ret;
  (void)arg;
  *panel_proofs = 0; *sys_hz = 0;
  ret = k7fb_verify(&g_frame);
  if (ret) return ret;
  /* Firmware exclusivity is insufficient: reject a live bootloader display
   * before modifying any clock, gate, reset, supply or panel GPIO. */
  {
    const struct k7dg_io guard_io = {&g_io,k7dn_read};
    ret = k7dg_check(&guard_io,&g_guard);
    diag_line("K7DISPLAY guard ret=%d proofs=%x reads=%u\n",
           ret,g_guard.proofs,g_guard.sampled_device);
    if (ret || g_guard.proofs != K7DG_ALL_QUIET) return ret ? ret : -EACCES;
  }
  /* The dedicated build supplies software ownership; the guard above supplies
   * observed hardware quiescence. Fixed 24 MHz reference and logic supply
   * are board facts, not an electrical measurement or panel response. */
  ret = k7dt_resources_on(&io,K7DT_REQUIRED_PROOFS,&g_resources);
  diag_line("K7DISPLAY resources ret=%d stage=%u power_stage=%u\n",
            ret,g_resources.stage,g_resources.power_stage);
  diag_line("K7DISPLAY resources writes=%u clock_writes=%u cleanup=%d\n",
            g_resources.writes,g_resources.clocks.write_attempts,
            g_resources.cleanup_error);
  diag_line("K7DISPLAY resources before.missing=%08x clocks.missing=%08x\n",
            g_resources.before.missing,g_resources.clocks.missing);
  diag_line("K7DISPLAY resources before.error=%d clocks.error=%d\n",
            g_resources.before.error,g_resources.clocks.error);
  for (unsigned i=0;i<8;i+=2)
    diag_line("K7DISPLAY bus_hz[%u]=%lu bus_hz[%u]=%lu\n",i,
              (unsigned long)g_resources.before.bus_hz[i],i+1,
              (unsigned long)g_resources.before.bus_hz[i+1]);
  diag_line("K7DISPLAY vp_sources_claimed=%x touched=%u\n",
            (unsigned)g_resources.vp_sources_claimed,g_resources.vp_sources_touched);
  diag_line("K7DISPLAY vop_bus_before=%08x after=%08x verified=%u\n",
            (unsigned)g_resources.vop_bus_before,(unsigned)g_resources.vop_bus_after,
            g_resources.vop_bus_verified);
  if (ret) return ret;
  g_io.bus_ready = 1;
  ret = live_resources(&r);
  if (!ret) ret = k7dn_iommu_bypass(&g_io,mmu);
  if (ret) return ret;
  diag_line("K7DISPLAY iommu_status=%08x,%08x vp1_hz=%lu dsi_hz=%lu\n",
         mmu[0],mmu[1],(unsigned long)r.pixel_hz,(unsigned long)r.dsi_sys_hz);
  /* Reset-default VP_CTRL may be zero. Only a prior power-off/full-reset
   * proof permits establishing standby after reset release; never do this
   * when adoption relied on an already-running register bank's standby. */
  if (g_guard.vop_cold) {
    for (unsigned i=0;i<3;++i) {
      uint32_t value;
      ret = k7dn_read(&g_io,0x27d00c00u+i*0x100u,&value);
      if (!ret) ret = k7dn_write(&g_io,0x27d00c00u+i*0x100u,value|(1u<<31));
      if (ret) return ret;
    }
    /* VP0/1/2 CFG_DONE with each high-word update bit, global cfg enable. */
    ret = k7dn_write(&g_io,0x27d00000u,0x00078007u);
    if (ret) return ret;
  }
  for (unsigned i=0;i<3;++i) {
    uint32_t value;
    ret = k7dn_read(&g_io,0x27d00c00u+i*0x100u,&value);
    if (ret) return ret;
    if (!(value&(1u<<31))) return -EBUSY;
  }
  *panel_proofs = K7PB_ALL_PROOFS;
  *sys_hz = (uint32_t)r.dsi_sys_hz;
  return 0;
}
static void report(void)
{
  /* -1 means stopped or visibility unknown after a backlight error. */
  diag_line("K7DISPLAY blanked=%d backlight_error=%d\n",
            g_session.started ? g_blanked : -1,g_backlight_error);
  diag_line("K7DISPLAY attempted=%u started=%d error=%d\n",
            g_attempted,g_session.started,g_error);
  diag_line("K7DISPLAY primary=%d cleanup=%d last_stop=%d\n",
            g_session.primary_error,g_session.cleanup_error,g_session.last_stop_error);
  diag_line("K7DISPLAY start_stage=%u cleanup_failures=%x panel_phase=%u\n",
            g_session.start_stage,g_session.cleanup_failures,(unsigned)g_session.panel.phase);
  diag_line("K7DISPLAY phy_pll_status=%08x valid=%u\n",
            (unsigned)g_session.dsi.phy.pll_status,g_session.dsi.phy.pll_status_valid);
  diag_line("K7DISPLAY dsi_poll_reg=%x value=%08x error=%d\n",
            (unsigned)g_session.dsi.failed_poll_reg,
            (unsigned)g_session.dsi.failed_poll_value,g_session.dsi.failed_poll_error);
  diag_line("K7DISPLAY dma_stopped=%u lease=%u resource_stage=%u\n",
            g_session.vop.dma_stopped,g_frame.leased,g_resources.stage);
  diag_line("K7DISPLAY resource_error=%d resource_cleanup=%d\n",
            g_resources.error,g_resources.cleanup_error);
  diag_line("K7DISPLAY video_frames=%u video_errors=%u\n",
            g_video_frames,g_video_errors);
  diag_line("K7DISPLAY video_prepare_avg_us=%" PRIu64 " video_prepare_max_us=%" PRIu64 "\n",
            g_video_frames ? g_prepare_us / g_video_frames : 0, g_max_prepare_us);
  diag_line("K7DISPLAY video_commit_avg_us=%" PRIu64 " video_commit_max_us=%" PRIu64 "\n",
            g_video_frames ? g_commit_us / g_video_frames : 0, g_max_commit_us);
  diag_line("K7DISPLAY stop_stage=%u stop_error=%d stop_valid=%x\n",
            g_session.vop.stop_stage,g_session.vop.stop_error,g_session.vop.stop_valid);
  diag_line("K7DISPLAY stop_raw=%08x ctrl=%08x plane=%08x cfg=%08x\n",
            g_session.vop.stop_raw,g_session.vop.stop_ctrl,
            g_session.vop.stop_plane,g_session.vop.stop_cfg);
  diag_line("K7DISPLAY resources before.missing=%08x clocks.missing=%08x\n",
            g_resources.before.missing,g_resources.clocks.missing);
}
int k7display_native_start(void)
{
  int ret = nxmutex_lock(&g_lock);
  if (ret) return ret;
  if (g_attempted) { nxmutex_unlock(&g_lock); return -EALREADY; }
  g_attempted = 1;
  ret = k7fb_native_acquire(&g_frame);
  if (!ret) ret = k7fb_colorbars(&g_frame);
  if (!ret) ret = k7fb_mark_inflight(&g_frame,&g_session.vop);
  if (!ret) {
    const struct k7display_platform p = {
      .panel = {&g_io,k7dn_read,k7dn_write,k7dn_delay_ms},
      .link = {k7dn_link_read,k7dn_link_write,k7dn_delay_us,k7dn_phy_reset,&g_io},
      .vop = {&g_io,ready,k7dn_vop_read,k7dn_vop_write,k7dn_delay_us,publish},
      .frame = g_frame.frame,.arg = &g_io,.preflight = preflight
    };
    ret = k7display_start(&g_session,&p,30);
  }
  g_blanked = ret ? -1 : 0;
  g_backlight_error = 0;
  g_error = ret;
  report();
  nxmutex_unlock(&g_lock);
  return ret;
}
int k7display_native_stop(void)
{
  int ret = nxmutex_lock(&g_lock);
  if (ret) return ret;
  ret = k7display_stop(&g_session);
  if (!ret && g_session.vop.dma_stopped && g_frame.leased)
    ret = k7fb_release(&g_frame,&g_session.vop);
  g_error = ret;
  report();
  /* Keep initialized domains and their shared bus clocks on. Stop does not
   * reprogram VPLL or remove parent-domain resources used by USB. */
  nxmutex_unlock(&g_lock);
  return ret;
}
int k7display_native_status(void)
{
  int ret = nxmutex_lock(&g_lock);
  if (ret) return ret;
  report();
  nxmutex_unlock(&g_lock);
  return 0;
}

static int video_ready_locked(void)
{
  if (!g_session.started || !g_session.vop.enabled || !g_frame.leased ||
      !g_frame.armed || g_session.vop.faulted || g_session.dsi.faulted)
    return -EPIPE;
  return 0;
}

int k7display_native_blank(int blank)
{
  int ret;
  if (blank!=0 && blank!=1) return -EINVAL;
  ret = nxmutex_lock(&g_lock);
  if (ret) return ret;
  ret = video_ready_locked();
  if (!ret) ret = k7pb_blank(&g_session.panel_io,&g_session.panel,blank);
  g_backlight_error = ret;
  g_blanked = ret ? -1 : blank;
  nxmutex_unlock(&g_lock);
  return ret;
}

int k7display_video_ready(void)
{
  int ret = nxmutex_lock(&g_lock);
  if (ret) return ret;
  ret = video_ready_locked();
  nxmutex_unlock(&g_lock);
  return ret;
}

int k7display_video_present_rgb888_box(const uint8_t *rgb, unsigned width,
                                      unsigned height, size_t stride, size_t bytes,
                                      const struct k7display_box *box)
{
  int ret = nxmutex_lock(&g_lock);
  if (ret) return ret;
  ret = video_ready_locked();
  uint64_t prepare_us = 0, commit_us = 0, started;
  if (!ret) {
    started = video_usec();
    ret = k7fb_prepare_rgb888(&g_frame,rgb,bytes,width,height,stride);
    if (!ret) ret = k7fb_overlay_box(&g_frame,box);
    prepare_us = video_usec() - started;
  }
  if (!ret) {
    started = video_usec();
    ret = k7fb_commit(&g_frame,&g_session.vop,200000);
    commit_us = video_usec() - started;
  }
  if (!ret) {
    ++g_video_frames;
    g_prepare_us += prepare_us; g_commit_us += commit_us;
    if (prepare_us > g_max_prepare_us) g_max_prepare_us = prepare_us;
    if (commit_us > g_max_commit_us) g_max_commit_us = commit_us;
  }
  else { ++g_video_errors; g_error=ret; }
  nxmutex_unlock(&g_lock);
  return ret;
}

int k7display_video_present_rgb888(const uint8_t *rgb, unsigned width,
                                  unsigned height, size_t stride, size_t bytes)
{
  return k7display_video_present_rgb888_box(rgb,width,height,stride,bytes,NULL);
}

int k7display_native_probe(void)
{
  struct k7dg_report r;
  const struct k7dg_io io = {&g_io,k7dn_read};
  int ret = nxmutex_lock(&g_lock);
  if (ret) return ret;
  ret = k7dg_check(&io,&r);
  diag_line("K7DISPLAY guard_probe ret=%d proofs=%x sampled=%u vop_cold=%u\n",
         ret,r.proofs,r.sampled_device,r.vop_cold);
  for (unsigned i=0;i<K7DG_COUNT;i++)
    diag_line("K7DISPLAY guard_reg index=%u before=%08x after=%08x\n",
           i,r.before.raw[i],r.after.raw[i]);
  diag_line("K7DISPLAY guard_devices vp=%08x,%08x,%08x\n",
            r.vp[0],r.vp[1],r.vp[2]);
  diag_line("K7DISPLAY guard_devices dsi=%08x phy_pll=%08x\n",
            r.dsi_power,r.phy_pll);
  diag_line("K7DISPLAY guard_lanes=%08x,%08x,%08x,%08x,%08x\n",
         r.phy_lane[0],r.phy_lane[1],r.phy_lane[2],r.phy_lane[3],r.phy_lane[4]);
  nxmutex_unlock(&g_lock);
  return ret;
}
