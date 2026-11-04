/* SPDX-License-Identifier: GPL-2.0-only */
#include "rk3576_display_framebuffer.h"
#include "rk3576_display_native.h"
#include <errno.h>
#include <string.h>

#if defined(__NuttX__) && defined(__aarch64__)
#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <nuttx/spinlock.h> /* SDK declares enter/leave_critical_section here. */
#ifndef CONFIG_BUILD_FLAT
#error "K7 native framebuffer AT/PAR and MMIO require the flat privileged build"
#endif
#ifdef CONFIG_RK3576_MODEL_ARENA
#include <nuttx/mm/k7_model_arena.h>
#endif
#endif

static struct k7fb *g_owner;

static int owned(const struct k7fb *fb)
{
  return fb && fb->leased &&
         __atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == fb;
}

int k7fb_acquire(struct k7fb *fb, const struct k7fb_io *io)
{
  struct k7fb *none = NULL;
  int ret;
  if (!fb || !io || !io->cpu || !io->verify_mapping ||
      !io->clean_to_poc || !io->barrier) return -EINVAL;
  if (fb->leased) return -EBUSY;
  if (((uintptr_t)io->cpu & 63) != 0) return -EINVAL;
  if (!__atomic_compare_exchange_n(&g_owner, &none, fb, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return -EBUSY;
  ret = io->verify_mapping(io->arg, K7FB_BASE, K7FB_RESERVED_BYTES);
  if (ret != 0)
    {
      __atomic_store_n(&g_owner, NULL, __ATOMIC_RELEASE);
      return ret < 0 ? ret : -EIO;
    }
  memset(fb, 0, sizeof(*fb));
  fb->io = *io;
  fb->frame.physical = K7FB_BASE;
  fb->frame.bytes = K7VOP_FRAME_BYTES;
  fb->frame.reserved_start = K7FB_BASE;
  fb->frame.reserved_bytes = K7FB_RESERVED_BYTES;
  fb->frame.stride = K7VOP_STRIDE;
  fb->leased = 1;
  return 0;
}

int k7fb_colorbars(struct k7fb *fb)
{
  static const uint32_t colors[8] =
    {0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
     0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000};
  unsigned x, y;
  if (!owned(fb)) return -EPERM;
  if (fb->armed) return -EBUSY; /* Never modify a live test pattern in place. */
  fb->dirty = 1;
  fb->border_slots = 0;
  for (y = 0; y < K7VOP_HEIGHT; ++y)
    for (x = 0; x < K7VOP_WIDTH; ++x)
      fb->io.cpu[y * K7VOP_WIDTH + x] = colors[(x * 8) / K7VOP_WIDTH];
  return 0;
}

int k7fb_verify(struct k7fb *fb)
{
  int ret;
  if (!owned(fb)) return -EPERM;
  ret = k7vop_validate_frame(&fb->frame);
  if (ret != 0) return ret;
  ret = fb->io.verify_mapping(fb->io.arg, K7FB_BASE, K7FB_RESERVED_BYTES);
  return ret > 0 ? -EIO : ret;
}

int k7fb_publish(void *arg, const struct k7vop_frame *frame)
{
  struct k7fb *fb = arg;
  uintptr_t start;
  int ret;
  if (!owned(fb) || !frame) return -EPERM;
  if (k7fb_check_frame(fb, frame)) return -EINVAL;
  start = (uintptr_t)fb->io.cpu + (uintptr_t)(frame->physical - K7FB_BASE);
  if (start > UINTPTR_MAX - frame->bytes) return -ERANGE;
  /* Always clean, even if another producer changed pixels after colorbars.
   * DMA only reads this buffer: invalidate would discard dirty CPU pixels.
   */
  ret = fb->io.clean_to_poc(fb->io.arg, start, start + frame->bytes);
  if (ret != 0) return ret < 0 ? ret : -EIO;
  ret = fb->io.barrier(fb->io.arg);
  if (ret != 0) return ret < 0 ? ret : -EIO;
  fb->dirty = 0;
  return 0;
}

int k7fb_check_frame(const struct k7fb *fb, const struct k7vop_frame *f)
{
  if (!owned(fb) || !f) return -EPERM;
  if ((f->physical != K7FB_BASE && f->physical != K7FB_BASE + K7FB_SLOT_BYTES) ||
      f->bytes != K7VOP_FRAME_BYTES || f->stride != K7VOP_STRIDE ||
      f->reserved_start != K7FB_BASE || f->reserved_bytes != K7FB_RESERVED_BYTES)
    return -EINVAL;
  return 0;
}

/* Center-aligned 3:2 enlargement. Fractions repeat 1/2, 1/6, 5/6;
 * Q8 weights keep the pixel loop integer-only. Clamp the two outer samples.
 */
static unsigned sample_axis(unsigned out, unsigned limit, unsigned *weight)
{
  unsigned pos, index;
  if (!out) { *weight = 0; return 0; }
  pos = out * 4u - 1u;
  index = pos / 6u;
  if (index >= limit - 1u) { *weight = 0; return limit - 1u; }
  *weight = ((pos % 6u) * 256u + 3u) / 6u;
  return index;
}

static uint32_t read_rgb(const uint8_t *p)
{
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t blend_rgb(uint32_t a, uint32_t b, unsigned weight)
{
  /* R and B occupy separate 16-bit lanes, including the weighted sum.
   * Round each channel independently; no alpha or inter-channel carry.
   */
  uint32_t rb = ((a & 0x00ff00ffu) * (256u - weight) +
                 (b & 0x00ff00ffu) * weight + 0x00800080u) >> 8;
  uint32_t g = ((a & 0x0000ff00u) * (256u - weight) +
                (b & 0x0000ff00u) * weight + 0x00008000u) >> 8;
  return (rb & 0x00ff00ffu) | (g & 0x0000ff00u);
}

#define RGB_TILE_ROWS 16u
/* Expand each source-row pair with the fixed center-aligned 3:2 phases:
 * odd sy emits 1/2; even sy emits 1/6 and 5/6. The two endpoints clamp.
 * This is bit-identical to sample_axis()/Q8 bilinear output, but removes
 * per-output-column division and cache-state branches. A 16-row strip keeps
 * source reads and transposed destination cache lines local; no frame scratch.
 * Reverse native rows inside the image to mirror horizontal landscape preview.
 * Source RGB, source vertical orientation and tracking coordinates are unchanged.
 */
static void landscape_rgb(uint32_t *dst, const uint8_t *rgb, size_t stride)
{
  unsigned base, j, sy;
  for (base = 0; base < 960u; base += RGB_TILE_ROWS)
    {
      unsigned sx[RGB_TILE_ROWS], wx[RGB_TILE_ROWS];
      uint32_t previous[RGB_TILE_ROWS];
      unsigned column = 718u;
      for (j = 0; j < RGB_TILE_ROWS; ++j)
        {
          const uint8_t *p;
          sx[j] = sample_axis(base + j, 640u, &wx[j]);
          p = rgb + sx[j] * 3u;
          previous[j] = blend_rgb(read_rgb(p), read_rgb(p + (sx[j] < 639u ? 3u : 0u)), wx[j]);
          dst[(1119u-base-j)*K7VOP_WIDTH+719u] = previous[j];
        }
      for (sy = 1; sy < 480u; ++sy)
        {
          const uint8_t *row = rgb + (size_t)sy * stride;
          for (j = 0; j < RGB_TILE_ROWS; ++j)
            {
              const uint8_t *p = row + sx[j]*3u;
              uint32_t value = blend_rgb(read_rgb(p), read_rgb(p + (sx[j] < 639u ? 3u : 0u)), wx[j]);
              uint32_t *line = dst + (1119u-base-j)*K7VOP_WIDTH + column;
              if (sy & 1u)
                line[0] = blend_rgb(previous[j], value, 128u);
              else
                {
                  line[0] = blend_rgb(previous[j], value, 43u);
                  line[-1] = blend_rgb(previous[j], value, 213u);
                }
              previous[j] = value;
            }
          column -= (sy & 1u) ? 1u : 2u;
        }
      for (j = 0; j < RGB_TILE_ROWS; ++j)
        dst[(1119u-base-j)*K7VOP_WIDTH] = previous[j];
    }
}

int k7fb_prepare_rgb888(struct k7fb *fb, const uint8_t *rgb, size_t bytes,
                        unsigned width, unsigned height, size_t stride)
{
  uint32_t *dst;
  unsigned slot;
  if (!owned(fb)) return -EPERM;
  if (!fb->armed || !fb->consumer || !fb->consumer->enabled ||
      fb->consumer->faulted || fb->consumer->present_pending || fb->frozen ||
      fb->prepared) return -EBUSY;
  if (!rgb || width != 640 || height != 480 || stride < 640u*3u ||
      stride > SIZE_MAX/480u || bytes < stride*479u + 640u*3u)
    return -EINVAL;
  /* RGB input must not alias either DMA slot, including the destination. */
  if ((uintptr_t)rgb > UINTPTR_MAX-bytes ||
      (uintptr_t)fb->io.cpu > UINTPTR_MAX-(uintptr_t)K7FB_RESERVED_BYTES)
    return -ERANGE;
  if ((uintptr_t)rgb < (uintptr_t)fb->io.cpu + K7FB_RESERVED_BYTES &&
      (uintptr_t)fb->io.cpu < (uintptr_t)rgb + bytes) return -EINVAL;
  if (fb->consumer->frame.physical != fb->frame.physical) return -EIO;
  slot = fb->active_slot ^ 1u;
  dst = fb->io.cpu + slot*(K7FB_SLOT_BYTES/sizeof(uint32_t));
  /* The image overwrites its entire rectangle. Black bars are immutable
   * for this lease and need clearing only once per inactive slot.
   */
  if (!(fb->border_slots & (1u << slot)))
    {
      memset(dst, 0, 160u * K7VOP_STRIDE);
      memset(dst + 1120u * K7VOP_WIDTH, 0, 160u * K7VOP_STRIDE);
      fb->border_slots |= 1u << slot;
    }
  landscape_rgb(dst, rgb, stride);
  fb->next_frame = fb->frame;
  fb->next_frame.physical = K7FB_BASE + slot*K7FB_SLOT_BYTES;
  fb->prepared = 1;
  fb->dirty = 1;
  return 0;
}

int k7fb_overlay_box(struct k7fb *fb, const struct k7display_box *box)
{
  int64_t x0, y0, x1, y1;
  unsigned left, right, top, bottom, row, col, t;
  uint32_t *dst;
  if (!owned(fb)) return -EPERM;
  if (!fb->prepared || fb->frozen || !fb->consumer ||
      !fb->consumer->enabled || fb->consumer->faulted ||
      fb->consumer->present_pending) return -EBUSY;
  if (!box || box->width <= 0 || box->height <= 0) return 0;
  /* Wide sums prevent hostile/malformed int32 coordinates from wrapping. */
  x0 = box->x; y0 = box->y;
  x1 = x0 + (int64_t)box->width; y1 = y0 + (int64_t)box->height;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > 640) x1 = 640;
  if (y1 > 480) y1 = 480;
  if (x0 >= x1 || y0 >= y1) return 0;
  /* Source half-open edges -> enclosing 1.5x pixels. Landscape mirror is
   * native row=1119-x*1.5; source vertical is native col=719-y*1.5.
   * Clamping precedes conversion, so black bars and slot padding are safe.
   */
  top = 1120u - (unsigned)((x1*3+1)/2);
  bottom = 1119u - (unsigned)(x0*3/2);
  left = 720u - (unsigned)((y1*3+1)/2);
  right = 719u - (unsigned)(y0*3/2);
  dst = fb->io.cpu + (fb->active_slot ^ 1u)*(K7FB_SLOT_BYTES/sizeof(uint32_t));
  /* Constant three-pixel stroke, bounded by even the narrowest rectangle. */
  for (t = 0; t < 3u && t <= bottom-top; ++t)
    for (col = left; col <= right; ++col)
      {
        dst[(top+t)*K7VOP_WIDTH+col] = 0x0000ff00u;
        dst[(bottom-t)*K7VOP_WIDTH+col] = 0x0000ff00u;
      }
  for (row = top; row <= bottom; ++row)
    for (t = 0; t < 3u && t <= right-left; ++t)
      {
        dst[row*K7VOP_WIDTH+left+t] = 0x0000ff00u;
        dst[row*K7VOP_WIDTH+right-t] = 0x0000ff00u;
      }
  fb->dirty = 1;
  return 0;
}

int k7fb_commit(struct k7fb *fb, struct k7vop *v, unsigned timeout_us)
{
  int ret;
  if (!owned(fb) || !v || v != fb->consumer) return -EPERM;
  if (!fb->prepared || fb->frozen) return -EBUSY;
  ret = k7vop_present(v, &fb->next_frame, timeout_us);
  if (ret) {
    /* Even pre-MMIO failure conservatively freezes this prepared image;
     * explicit stop is the only recovery, never an implicit duplicate flip. */
    fb->frozen = 1;
    return ret;
  }
  fb->frame = fb->next_frame;
  fb->active_slot ^= 1u;
  fb->prepared = 0;
  return 0;
}

int k7fb_mark_inflight(struct k7fb *fb, struct k7vop *consumer)
{
  if (!owned(fb) || !consumer) return -EPERM;
  if (fb->armed) return -EBUSY;
  fb->consumer = consumer;
  consumer->dma_stopped = 0; /* Cannot reuse a prior lease's stop receipt. */
  fb->armed = 1;
  return 0;
}

int k7fb_release(struct k7fb *fb, const struct k7vop *v)
{
  if (!owned(fb)) return -EPERM;
  if (fb->armed && (!v || v != fb->consumer || !v->dma_stopped || v->enabled ||
      !v->touched || k7fb_check_frame(fb, &v->frame))) return -EBUSY;
  fb->armed = 0;
  fb->leased = 0;
  __atomic_store_n(&g_owner, NULL, __ATOMIC_RELEASE);
  return 0;
}

#if defined(__NuttX__) && defined(__aarch64__)
static int native_mapping(void *arg, uint64_t base, size_t bytes)
{
  uintptr_t page;
  (void)arg;
  if (base != K7FB_BASE || bytes != K7FB_RESERVED_BYTES) return -EINVAL;
  /* The linker/system heap and existing model window must remain disjoint.
   * Parent boot integration reserves this exact range by a dedicated mapping.
   */
  if ((uint64_t)CONFIG_RAM_START < K7FB_BASE + K7FB_RESERVED_BYTES &&
      (uint64_t)CONFIG_RAM_START + CONFIG_RAM_SIZE > K7FB_BASE) return -EACCES;
#ifdef CONFIG_RK3576_MODEL_ARENA
  if (K7_MODEL_ARENA_BASE < K7FB_BASE + K7FB_RESERVED_BYTES &&
      K7_MODEL_ARENA_BASE + K7_MODEL_WINDOW_SIZE > K7FB_BASE) return -EACCES;
#endif
  for (page = (uintptr_t)base; page < base + bytes; page += 4096)
    {
      uint64_t par;
      irqstate_t irq = enter_critical_section();
      /* A writable translation is tested without touching an unmapped page.
       * Keep AT and PAR read on this CPU, with IRQ/preemption held briefly.
       */
      __asm__ volatile("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1"
                       : "=r"(par) : "r"(page) : "memory");
      leave_critical_section(irq);
      if ((par & 1) ||
          (par & UINT64_C(0x0000fffffffff000)) != page ||
          (par >> 56) != 0xff) return -EFAULT;
    }
  return 0;
}

static int native_clean(void *arg, uintptr_t start, uintptr_t end)
{
  (void)arg;
  if ((start != (uintptr_t)K7FB_BASE &&
       start != (uintptr_t)(K7FB_BASE + K7FB_SLOT_BYTES)) ||
      end != start + K7VOP_FRAME_BYTES)
    return -ERANGE;
  up_clean_dcache(start, end); /* NuttX range end is exclusive; clean to PoC. */
  return 0;
}

static int native_barrier(void *arg)
{
  (void)arg;
  __asm__ volatile("dsb sy" ::: "memory");
  return 0;
}
#endif

int k7fb_native_acquire(struct k7fb *fb)
{
#if defined(__NuttX__) && defined(__aarch64__)
  const struct k7fb_io io = {NULL, (uint32_t *)(uintptr_t)K7FB_BASE,
                            native_mapping, native_clean, native_barrier};
  return k7fb_acquire(fb, &io);
#else
  (void)fb;
  return -ENOSYS;
#endif
}
