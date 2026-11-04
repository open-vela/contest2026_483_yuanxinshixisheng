/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K7DISPLAY_RK3576_VOP_H
#define K7DISPLAY_RK3576_VOP_H

#include <stddef.h>
#include <stdint.h>

#define K7VOP_BASE UINT32_C(0x27d00000)
#define K7VOP_WIDTH 720u
#define K7VOP_HEIGHT 1280u
#define K7VOP_PIXEL_HZ 84000000u
#define K7VOP_STRIDE 2880u
#define K7VOP_FRAME_BYTES (K7VOP_STRIDE * K7VOP_HEIGHT)

/* Only an exclusively reserved, CPU-mapped, DMA-accessible buffer is legal.
 * The caller owns allocation, cache maintenance and lifetime. Pixels are
 * little-endian XRGB8888 (B,G,R,X bytes); DSI output is RGB888, not ARGB.
 */
struct k7vop_frame
{
  uint64_t physical;
  size_t bytes;
  uint64_t reserved_start;
  uint64_t reserved_bytes;
  uint32_t stride;
};

struct k7vop_io
{
  void *arg;
  /* Called BEFORE any MMIO. Must freshly verify mapped VOP, live bus clocks,
   * powered VOP/ESMART domain, deasserted reset, exclusive ownership of VOP
   * (global line-buffer setup affects all planes), 84 MHz VP1 clock, and reserved
   * framebuffer accessible to AXI0 with IOMMU disabled or correctly mapped.
   * Missing board implementation MUST return -ENOSYS, not assume readiness.
   */
  int (*check_ready)(void *, const struct k7vop_frame *, uint32_t pixel_hz);
  int (*read32)(void *, uint32_t address, uint32_t *value);
  int (*write32)(void *, uint32_t address, uint32_t value);
  int (*delay_us)(void *, uint32_t microseconds);
  /* Clean framebuffer to PoC and issue required DMA/MMIO barriers. */
  int (*publish_frame)(void *, const struct k7vop_frame *);
};

struct k7vop
{
  struct k7vop_io io;
  struct k7vop_frame frame;
  unsigned configured;
  unsigned enabled;
  unsigned faulted;
  unsigned touched;
  unsigned dma_stopped;
  unsigned present_pending;
  unsigned present_count;
  struct k7vop_frame pending_frame;
  unsigned stop_stage;
  int stop_error;
  unsigned stop_valid;
  uint32_t stop_raw, stop_ctrl, stop_plane, stop_cfg;
};

/* init has no MMIO. configure requires VP1 already in standby and leaves it
 * there. enable waits at most timeout_us (1..200000) for a NEW frame-start;
 * success proves VOP scan activity only, not DSI/panel light or visible pixels.
 * Failure latches faulted; the board owner must reset/reinitialize explicitly.
 * disable also accepts partial configure; first it waits up to 500 ms for an
 * independently committed plane-off, then waits 100 ms for a fresh hardware
 * DSP_HOLD_VALID event and verifies standby/plane-off before dma_stopped=1.
 * On any stop error, the framebuffer lease MUST remain held until board reset
 * or independently verified DMA quiescence. A register write is not stop proof.
 */
int k7vop_validate_frame(const struct k7vop_frame *frame);
int k7vop_init(struct k7vop *, const struct k7vop_io *);
int k7vop_configure(struct k7vop *, const struct k7vop_frame *);
int k7vop_enable(struct k7vop *, unsigned timeout_us);
int k7vop_disable(struct k7vop *);
/* Change only the scanout address. Success requires cfg-done consumption and
 * a fresh frame-start AFTER that consumption, retiring the previous buffer.
 * Failure after address write retains both frames and latches faulted; neither
 * buffer may be overwritten until disable has proved DMA stopped. */
int k7vop_present(struct k7vop *, const struct k7vop_frame *, unsigned timeout_us);

#endif
