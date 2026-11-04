/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K7_DISPLAY_FRAMEBUFFER_H
#define K7_DISPLAY_FRAMEBUFFER_H
#include "rk3576_vop.h"

/* Dedicated 8 MiB DDR window, two 4 MiB slots. Never add to malloc/model heap.
 * Requires matching MT_NORMAL|MT_RW mapping in rk3576_boot.c.
 */
#define K7FB_BASE UINT64_C(0xa0000000)
#define K7FB_SLOT_BYTES UINT64_C(0x400000)
#define K7FB_RESERVED_BYTES UINT64_C(0x800000)

struct k7fb_io
{
  void *arg;
  uint32_t *cpu;
  /* Verify the entire physical reservation is mapped read/write and reserved.
   * Native implementation checks AT S1E1W / PAR_EL1 for EVERY 4 KiB page.
   */
  int (*verify_mapping)(void *, uint64_t, size_t);
  int (*clean_to_poc)(void *, uintptr_t, uintptr_t);
  int (*barrier)(void *);
};

/* Zero-initialize once. Single global lease prevents two live owners.
 * The caller serializes operations on this object (including camera writes).
 */
struct k7fb
{
  struct k7fb_io io;
  struct k7vop_frame frame;
  struct k7vop *consumer;
  unsigned leased, dirty, armed;
  unsigned active_slot, prepared, frozen;
  unsigned border_slots; /* Inactive-slot black bars initialized for this lease. */
  struct k7vop_frame next_frame;
};

int k7fb_acquire(struct k7fb *, const struct k7fb_io *);
int k7fb_native_acquire(struct k7fb *);
int k7fb_verify(struct k7fb *);
int k7fb_colorbars(struct k7fb *);
int k7fb_publish(void *fb, const struct k7vop_frame *);
int k7fb_check_frame(const struct k7fb *, const struct k7vop_frame *);
/* Fixed RGB888 camera input 640x480, bilinear scaled to 960x720, then clockwise
 * 90 degrees: 720x960 at (0,160), black bars. Physical panel is viewed after
 * counter-clockwise 90-degree rotation; panel scanout remains 720x1280.
 * Producer and commit must be serialized by the native display owner lock. */
int k7fb_prepare_rgb888(struct k7fb *, const uint8_t *, size_t bytes,
                        unsigned width, unsigned height, size_t stride);
struct k7display_box;
/* Draw only into a prepared inactive slot; commit publishes/cache-cleans it. */
int k7fb_overlay_box(struct k7fb *, const struct k7display_box *);
int k7fb_commit(struct k7fb *, struct k7vop *, unsigned timeout_us);
/* Call immediately BEFORE VOP configure, even if configure later fails. */
int k7fb_mark_inflight(struct k7fb *, struct k7vop *consumer);
/* An armed frame requires successful VOP DMA stop before release. An unarmed
 * lease can be released with NULL. Failed/uncertain stop retains the lease.
 */
int k7fb_release(struct k7fb *, const struct k7vop *stopped_vop);

#endif
