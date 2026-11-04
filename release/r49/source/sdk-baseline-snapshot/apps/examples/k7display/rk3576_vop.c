/* SPDX-License-Identifier: GPL-2.0-only
 * Fixed RK3576 VP1 / ESMART1 scanout, adapted from Rockchip U-Boot:
 * Copyright 2008-2017 Fuzhou Rockchip Electronics Co., Ltd.
 * SDK rk3576-linux-20260320, u-boot/drivers/video/drm/rockchip_vop2.c
 * blob 2cdb81634b1934ede38db5a4a4fc6e6e1ec6cb5a.
 * Sources: rk3576_vop2_if_cfg, vop2_post_config, vop3_setup_pipe_dly,
 * vop2_set_esmart_win and rk3576_win_data. Linux rockchip_vop2_reg.c and
 * rockchip_vop_reg.h supply VP1 frame-start raw status and clear semantics.
 * This is NOT a clock/power/IOMMU/DSI bring-up implementation.
 */
#include "rk3576_vop.h"
#include <errno.h>
#include <string.h>

#define BIT(n) (UINT32_C(1) << (n))
#define CFG_DONE 0x000u
#define VP1_CTRL 0xd00u
#define VP1_INT_CLR 0x0b4u
#define VP1_INT_RAW 0x0bcu
#define STANDBY BIT(31)
#define COMMIT (BIT(15) | BIT(1) | BIT(17))
#define E1_REGION0 0x1a10u

static int read_reg(struct k7vop *v, uint32_t off, uint32_t *value)
{
  return v->io.read32(v->io.arg, K7VOP_BASE + off, value);
}

static int write_reg(struct k7vop *v, uint32_t off, uint32_t value)
{
  return v->io.write32(v->io.arg, K7VOP_BASE + off, value);
}

static int update(struct k7vop *v, uint32_t off, uint32_t mask, uint32_t value)
{
  uint32_t old;
  int ret = read_reg(v, off, &old);
  if (ret < 0) return ret;
  return write_reg(v, off, (old & ~mask) | (value & mask));
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t end)
{
  return a < end && b < a + n;
}

int k7vop_validate_frame(const struct k7vop_frame *f)
{
  if (!f || f->stride != K7VOP_STRIDE || f->bytes < K7VOP_FRAME_BYTES ||
      (f->physical & 63) || !f->reserved_bytes ||
      f->reserved_start > UINT64_MAX - f->reserved_bytes ||
      f->physical > UINT64_MAX - f->bytes)
    return -EINVAL;
  /* This driver programs a 32-bit YRGB master register; never truncate DMA. */
  if (f->physical < UINT64_C(0x40000000) ||
      f->physical + f->bytes > UINT64_C(0x100000000) ||
      f->physical < f->reserved_start ||
      f->physical + f->bytes > f->reserved_start + f->reserved_bytes)
    return -ERANGE;
  /* Never borrow the kernel/heap/secure regions or existing 1 GiB model pool.
   * Other allocations MUST be excluded by the board's reservation checker.
   */
  if (overlaps(f->physical, f->bytes, UINT64_C(0x40000000),
               UINT64_C(0x49400000)) ||
      overlaps(f->physical, f->bytes, UINT64_C(0x60000000),
               UINT64_C(0xa0000000)))
    return -EACCES;
  return 0;
}

int k7vop_init(struct k7vop *v, const struct k7vop_io *io)
{
  if (!v || !io || !io->check_ready || !io->read32 || !io->write32 ||
      !io->delay_us || !io->publish_frame) return -EINVAL;
  memset(v, 0, sizeof(*v));
  v->io = *io;
  return 0;
}

/* Stop at first failed MMIO. The caller owns recovery of partial writes. */
#define DO(op) do { ret = (op); if (ret < 0) goto fail; } while (0)

int k7vop_configure(struct k7vop *v, const struct k7vop_frame *f)
{
  uint32_t ctrl;
  int ret;
  if (!v || !v->io.check_ready) return -EINVAL;
  if (v->enabled || v->faulted) return -EBUSY;
  ret = k7vop_validate_frame(f);
  if (ret < 0) return ret;
  ret = v->io.check_ready(v->io.arg, f, K7VOP_PIXEL_HZ);
  if (ret < 0) return ret;
  DO(read_reg(v, VP1_CTRL, &ctrl));
  if (!(ctrl & STANDBY)) return -EBUSY;
  DO(read_reg(v, 0xc00, &ctrl));
  if (!(ctrl & STANDBY)) return -EBUSY;
  DO(read_reg(v, 0xe00, &ctrl));
  if (!(ctrl & STANDBY)) return -EBUSY;
  v->configured = 0;
  DO(v->io.publish_frame(v->io.arg, f));
  v->frame = *f;
  v->touched = 1;
  v->dma_stopped = 0;

  /* Exclusive cold-start global initialization, from vop2_global_initial.
   * Configure line-buffer mode 3 using the register's high-word write mask.
   * Disable the documented ACLK pre auto-gate workaround and LUT DMA; preserve
   * unrelated fields. AXI MMU mode is owned by check_ready's board backend.
   */
  DO(write_reg(v, 0x034, (3u << 22) | (3u << 6)));
  DO(update(v, 0x008, BIT(7), 0));
  DO(update(v, 0x024, BIT(16) | BIT(0), 0));
  DO(update(v, 0x028, 7u, 0));
  DO(update(v, 0x600, BIT(28), BIT(28)));
  DO(write_reg(v, 0xda0, 0xc8));
  DO(write_reg(v, 0xda4, 0x01000100));
  DO(write_reg(v, 0xda8, 0x03ff0100));
  /* Only VP1 layer zero participates. Mix-stage enables are zero (single
   * opaque layer bypass); other VP1 layer selectors remain reset and disabled.
   */
  DO(write_reg(v, 0x700, 0));
  DO(write_reg(v, 0x704, 2));
  DO(write_reg(v, 0x720, 0));
  DO(write_reg(v, 0x724, 0));
  DO(write_reg(v, 0x728, 0));
  DO(write_reg(v, 0x72c, 0));
  DO(write_reg(v, 0x730, 0));
  DO(write_reg(v, 0x734, 0));
  DO(write_reg(v, 0x738, 0));
  DO(write_reg(v, 0x73c, 0));
  DO(write_reg(v, 0x740, 0));
  DO(write_reg(v, 0x744, 0));
  DO(write_reg(v, 0x748, 0));
  DO(write_reg(v, 0x74c, 0));

  /* VP1 has one pixel per core clock. DSI interface pixel clock selects /4;
   * no split, command mode or additional core/out divider. Output stays gated
   * until k7vop_enable, which must follow DSI pre-enable / panel preparation.
   */
  DO(update(v, 0xd0c, BIT(0) | BIT(2), 0));
  DO(update(v, 0x180, BIT(31) | BIT(21) | BIT(20) | BIT(11) |
            BIT(9) | BIT(8) | (7u << 4) | (3u << 2) | 3u,
            BIT(31) | BIT(20) | (3u << 4) | (1u << 2)));
  DO(write_reg(v, VP1_CTRL, STANDBY)); /* P888, no dither/LUT/CSC/interlace. */
  DO(write_reg(v, 0xd04, 0));
  DO(write_reg(v, 0xd08, 0));
  DO(write_reg(v, 0xd2c, 0));
  DO(write_reg(v, 0xd48, (799u << 16) | 15u));
  DO(write_reg(v, 0xd4c, (47u << 16) | 767u));
  DO(write_reg(v, 0xd50, (1328u << 16) | 15u));
  DO(write_reg(v, 0xd54, (27u << 16) | 1307u));
  DO(write_reg(v, 0xd34, (47u << 16) | 767u));
  DO(write_reg(v, 0xd38, (27u << 16) | 1307u));
  DO(write_reg(v, 0xd40, 0)); /* Post-scaler bypass. */
  DO(write_reg(v, 0xd60, 0)); /* BCSH RGB/YUV conversions bypassed. */
  DO(update(v, 0x524, 0xffu, 0));
  DO(update(v, 0x770, 0xff000000u, 16u << 24));
  DO(write_reg(v, 0xd30, (375u << 16) | 15u));

  /* One ESMART1 region, opaque XRGB8888, unity dimensions/no scaling.
   * ESMART0 CANNOT connect to VP1 on RK3576. ESMART1 uses scale engine 1
   * under the required global 4K/4K/2K/2K line-buffer allocation (mode 3).
   */
  DO(update(v, 0x700, 1u, 0));
  DO(update(v, 0x704, 0xfu, 2u));
  DO(write_reg(v, 0x1a00, 1u << 12));
  DO(write_reg(v, 0x1a04, (0x0cu << 4) | (0x0du << 12)));
  DO(update(v, 0x1a08, BIT(1), 0));
  DO(write_reg(v, 0x1af4, 1u));
  DO(write_reg(v, 0x1af8, 0));
  DO(write_reg(v, E1_REGION0, 0));
  DO(write_reg(v, 0x1a40, 0));
  DO(write_reg(v, 0x1a70, 0));
  DO(write_reg(v, 0x1aa0, 0));
  DO(write_reg(v, 0x1ad0, 0));
  DO(write_reg(v, 0x1ad8, 0x8000ff00u));
  DO(write_reg(v, 0x1a1c, f->stride / 4));
  DO(write_reg(v, 0x1a14, (uint32_t)f->physical));
  DO(write_reg(v, 0x1a20, (1279u << 16) | 719u));
  DO(write_reg(v, 0x1a24, (1279u << 16) | 719u));
  DO(write_reg(v, 0x1a28, 0));
  DO(write_reg(v, 0x1a30, 0));
  DO(write_reg(v, 0x1a34, 0));
  DO(write_reg(v, E1_REGION0, 1u));
  DO(write_reg(v, CFG_DONE, COMMIT));
  v->configured = 1;
  return 0;
fail:
  v->faulted = 1;
  return ret;
}

int k7vop_enable(struct k7vop *v, unsigned timeout_us)
{
  uint32_t raw;
  unsigned elapsed = 0;
  int ret;
  if (!v || !v->configured || v->faulted || v->enabled ||
      !timeout_us || timeout_us > 200000u) return -EINVAL;
  ret = v->io.check_ready(v->io.arg, &v->frame, K7VOP_PIXEL_HZ);
  if (ret < 0) return ret;
  DO(v->io.publish_frame(v->io.arg, &v->frame));
  v->dma_stopped = 0; /* A previous successful stop does not cover this run. */
  DO(write_reg(v, VP1_INT_CLR, BIT(16) | BIT(0))); /* W1C hiword mask. */
  DO(read_reg(v, VP1_INT_RAW, &raw));
  if (raw & BIT(0)) { ret = -EIO; goto fail; }
  DO(update(v, 0x180, 3u, 3u));
  DO(update(v, VP1_CTRL, STANDBY, 0));
  DO(write_reg(v, CFG_DONE, COMMIT));
  while (elapsed < timeout_us)
    {
      unsigned step = timeout_us - elapsed;
      if (step > 100u) step = 100u;
      DO(v->io.delay_us(v->io.arg, step));
      elapsed += step;
      DO(read_reg(v, VP1_INT_RAW, &raw));
      if (raw & BIT(0)) { v->enabled = 1; return 0; }
    }
  ret = -ETIMEDOUT;
fail:
  v->faulted = 1;
  return ret;
}

int k7vop_disable(struct k7vop *v)
{
  uint32_t raw, ctrl, plane;
  unsigned elapsed;
  int ret;
  if (!v || !v->touched) return -EINVAL;
  /* Preserve the first stop failure until board-owned reinitialization. */
  if (v->stop_error) return v->stop_error;
  v->stop_stage = 1;
  v->stop_valid = 0;
  /* Even a timed-out enable may have started scanout. Always permit explicit
   * stop, but first revalidate register access through the board owner.
   */
  ret = v->io.check_ready(v->io.arg, &v->frame, K7VOP_PIXEL_HZ);
  if (ret < 0) { v->stop_error=ret; return ret; }
  v->dma_stopped = 0;
  /* Linux rockchip_drm_vop2.c:3186-3207,5038 disables windows and
   * waits for their effective enable bits before requesting VP standby.
   * A combined plane-off/standby commit can stop FS before the plane change
   * latches (observed on RK3576: standby=1, plane=1, cfg_done pending).
   * Keep VP scanning until the independently committed plane is really off.
   */
  v->stop_stage = 3;
  DO(update(v, E1_REGION0, BIT(0), 0));
  v->stop_stage = 10;
  DO(write_reg(v, CFG_DONE, COMMIT));
  v->stop_stage = 11;
  for (elapsed = 0; ; elapsed += 100)
    {
      DO(read_reg(v, E1_REGION0, &plane));
      v->stop_plane=plane; v->stop_valid|=4u;
      DO(read_reg(v, CFG_DONE, &v->stop_cfg));
      v->stop_valid|=8u;
      if (!(plane & BIT(0))) break;
      if (elapsed >= 500000) { ret=-ETIMEDOUT; goto fail; }
      DO(v->io.delay_us(v->io.arg, 100));
    }
  v->stop_stage = 2;
  DO(write_reg(v, VP1_INT_CLR, BIT(22) | BIT(6)));
  DO(read_reg(v, VP1_INT_RAW, &raw));
  v->stop_raw=raw; v->stop_valid|=1u;
  if (raw & BIT(6)) { ret = -EIO; goto fail; }
  v->stop_stage = 4;
  DO(update(v, VP1_CTRL, STANDBY, STANDBY));
  v->stop_stage = 5;
  DO(write_reg(v, CFG_DONE, COMMIT));
  DO(read_reg(v, CFG_DONE, &v->stop_cfg));
  v->stop_valid|=8u;
  /* Linux vop2_crtc_atomic_disable waits 100 ms on DSP_HOLD_VALID before
   * disabling resources. Use raw bit 6 here rather than enabling an IRQ.
   * An already-idle partial setup may not produce a fresh event: timeout is
   * deliberately inconclusive and requires board-owned reset/quiescence.
   */
  v->stop_stage = 6;
  for (elapsed = 0; elapsed < 100000; elapsed += 100)
    {
      DO(v->io.delay_us(v->io.arg, 100));
      DO(read_reg(v, VP1_INT_RAW, &raw));
      v->stop_raw=raw;
      if (raw & BIT(6)) break;
    }
  if (!(raw & BIT(6))) { ret = -ETIMEDOUT; goto fail; }
  v->stop_stage = 7;
  DO(read_reg(v, VP1_CTRL, &ctrl));
  v->stop_ctrl=ctrl; v->stop_valid|=2u;
  DO(read_reg(v, E1_REGION0, &plane));
  v->stop_plane=plane; v->stop_valid|=4u;
  DO(read_reg(v, CFG_DONE, &v->stop_cfg));
  if (!(ctrl & STANDBY) || (plane & BIT(0)))
    { ret = -EIO; goto fail; }
  v->stop_stage = 8;
  DO(update(v, 0x180, 3u, 0));
  v->enabled = 0;
  v->dma_stopped = 1;
  v->present_pending = 0;
  v->stop_stage = 9;
  return 0;
fail:
  v->stop_error = ret;
  v->faulted = 1;
  return ret;
}

int k7vop_present(struct k7vop *v, const struct k7vop_frame *f,
                   unsigned timeout_us)
{
  uint32_t raw, cfg, address;
  unsigned elapsed = 0;
  int ret;
  if (!v || !f || !v->enabled || !v->configured || v->faulted ||
      v->present_pending || !timeout_us || timeout_us > 200000u)
    return -EINVAL;
  ret = k7vop_validate_frame(f);
  if (ret) return ret;
  if (f->bytes != v->frame.bytes || f->stride != v->frame.stride ||
      f->reserved_start != v->frame.reserved_start ||
      f->reserved_bytes != v->frame.reserved_bytes ||
      overlaps(f->physical, f->bytes, v->frame.physical,
               v->frame.physical + v->frame.bytes)) return -EINVAL;
  ret = v->io.check_ready(v->io.arg, f, K7VOP_PIXEL_HZ);
  if (ret) return ret;
  ret = read_reg(v, CFG_DONE, &cfg);
  if (ret) return ret;
  if (cfg & BIT(1)) return -EAGAIN; /* Never overwrite a pending commit. */
  ret = v->io.publish_frame(v->io.arg, f);
  if (ret) return ret;
  v->pending_frame = *f;
  v->present_pending = 1;
  v->dma_stopped = 0;
  DO(write_reg(v, 0x1a14, (uint32_t)f->physical));
  DO(write_reg(v, CFG_DONE, COMMIT));
  /* Vendor rockchip_drm_vop2.c vop2_pending_done_bits reads CFG_DONE low
   * bits as not-yet-effective config; atomic_flush queues old FB retirement
   * on a later vblank (11821-11854). We are polling with IRQ ownership, so
   * first observe consumption, then CLEAR old FS and require another FS.
   * This intentionally holds the old buffer an extra frame; address readback
   * alone is not latch proof. No register or old pixels are rolled back. */
  for (;;) {
    DO(read_reg(v, CFG_DONE, &cfg));
    if (!(cfg & BIT(1))) break;
    if (elapsed >= timeout_us) { ret = -ETIMEDOUT; goto fail; }
    { unsigned step = timeout_us - elapsed;
      if (step > 100) step = 100;
      DO(v->io.delay_us(v->io.arg, step)); elapsed += step; }
  }
  DO(read_reg(v, 0x1a14, &address));
  if (address != f->physical) { ret = -EIO; goto fail; }
  DO(write_reg(v, VP1_INT_CLR, BIT(16) | BIT(0)));
  DO(read_reg(v, VP1_INT_RAW, &raw));
  if (raw & BIT(0)) { ret = -EIO; goto fail; }
  for (;;) {
    if (elapsed >= timeout_us) { ret = -ETIMEDOUT; goto fail; }
    { unsigned step = timeout_us - elapsed;
      if (step > 100) step = 100;
      DO(v->io.delay_us(v->io.arg, step)); elapsed += step; }
    DO(read_reg(v, VP1_INT_RAW, &raw));
    if (raw & BIT(0)) break;
  }
  DO(read_reg(v, CFG_DONE, &cfg));
  DO(read_reg(v, 0x1a14, &address));
  if ((cfg & BIT(1)) || address != f->physical) { ret = -EIO; goto fail; }
  v->frame = *f;
  v->present_pending = 0;
  ++v->present_count;
  return 0;
fail:
  v->faulted = 1;
  return ret;
}
