/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <errno.h>
#include <stddef.h>
#include "rk3576_display_native_io.h"
#include "rk3576_display_guard.h"
#include <string.h>

static int inside(uintptr_t a, uintptr_t base, uintptr_t length)
{ return a >= base && a - base <= length - sizeof(uint32_t); }

static int mapped(const struct k7dn_io_state *s, uintptr_t a)
{
  if (!s || (a & 3u)) return 0;
  /* Always-on control blocks. GPIO0/PWM accessibility is established by
   * k7pb_prepare's gate checks before those registers are accessed. */
  if (inside(a,0x26040000,0x10000) || inside(a,0x27200000,0x10000) ||
      inside(a,0x27220000,0x10000) || inside(a,0x27380000,0x1000) ||
      inside(a,0x26024000,0x1000)) return 1;
  if (!s->bus_ready) return 0;
  return inside(a,0x27320000,0x1000) || inside(a,0x2add1000,0x1000) ||
         inside(a,0x2600a000,0x2000) || inside(a,0x2601a000,0x1000) ||
         inside(a,0x26034000,0x2000) || inside(a,0x27d00000,0x3000) ||
         inside(a,0x27d07000,0x1000) || inside(a,0x27d80000,0x10000) ||
         inside(a,0x2b020000,0x2000);
}
int k7dn_barrier(void *arg)
{
  (void)arg;
  __asm__ volatile("dsb sy" ::: "memory");
  return 0;
}
int k7dn_read(void *arg, uintptr_t a, uint32_t *value)
{
  if (!value || !arg || (a & 3u)) return -EACCES;
  if (!mapped(arg,a)) {
    /* AON snapshots are mapped above and cannot recurse into this branch.
     * Only specifically enumerated status words/QoS become readable early;
     * the write path retains its bus_ready restriction. */
    const struct k7dg_io io = {arg,k7dn_read};
    struct k7dg_snapshot before,after;
    int ret = k7dg_sample(&io,&before);
    if (!ret) ret = k7dg_sample(&io,&after);
    if (ret) return ret;
    if (memcmp(&before,&after,sizeof(before))) return -EAGAIN;
    if (!k7dg_device_readable(&after,a)) return -EACCES;
  }
  k7dn_barrier(arg);
  *value = *(volatile const uint32_t *)a;
  return k7dn_barrier(arg);
}
int k7dn_write(void *arg, uintptr_t a, uint32_t value)
{
  if (!mapped(arg,a)) return -EACCES;
  *(volatile uint32_t *)a = value;
  return k7dn_barrier(arg);
}
int k7dn_delay_us(void *arg, unsigned us)
{
  (void)arg;
  if (us > 200000u) return -ERANGE;
  up_udelay(us);
  return 0;
}
int k7dn_delay_ms(void *arg, unsigned ms)
{
  if (ms > 200u) return -ERANGE;
  return k7dn_delay_us(arg,ms * 1000u);
}
static uintptr_t block_address(enum k7_display_block b, uint32_t off)
{
  static const uintptr_t base[] = {
    0x27d80000, 0x2b020000, 0x2601a000, 0x26034000
  };
  static const uint32_t length[] = {0x10000,0x2000,0x1000,0x2000};
  if ((unsigned)b >= sizeof(base)/sizeof(base[0]) || (off & 3u) ||
      off > length[b] - 4u) return 0;
  return base[b] + off;
}
int k7dn_link_read(void *arg, enum k7_display_block b,
                   uint32_t off, uint32_t *v)
{
  uintptr_t a = block_address(b,off);
  return a ? k7dn_read(arg,a,v) : -EINVAL;
}
int k7dn_link_write(void *arg, enum k7_display_block b,
                    uint32_t off, uint32_t v)
{
  uintptr_t a = block_address(b,off);
  return a ? k7dn_write(arg,a,v) : -EINVAL;
}
int k7dn_phy_reset(void *arg, bool asserted)
{
  /* Vendor SRST_M_MIPI_DCPHY=PMU SOFTRST_CON1 bit3. Never change
   * slave/CSI/USB reset bits sharing this register. */
  const uint32_t mask = 1u << 3;
  uint32_t got;
  int ret = k7dn_write(arg,0x27220a04,(mask << 16) | (asserted ? mask : 0));
  if (!ret) ret = k7dn_read(arg,0x27220a04,&got);
  if (!ret && (got & mask) != (asserted ? mask : 0)) ret = -EIO;
  return ret;
}
int k7dn_vop_read(void *arg, uint32_t a, uint32_t *v)
{
  return inside(a,0x27d00000,0x3000) ? k7dn_read(arg,a,v) : -EINVAL;
}
int k7dn_vop_write(void *arg, uint32_t a, uint32_t v)
{
  return inside(a,0x27d00000,0x3000) ? k7dn_write(arg,a,v) : -EINVAL;
}
int k7dn_iommu_bypass(void *arg, uint32_t status[2])
{
  if (!status) return -EINVAL;
  for (unsigned i = 0; i < 2; ++i) {
    int ret = k7dn_read(arg,0x27d07e04u + 0x100u * i,&status[i]);
    if (ret) return ret;
    /* rk3576.dtsi: two iommu-v2 banks. STATUS paging/fault/stall bits
     * from official rockchip-iommu.c blob b28a3f7cb7d2cdccb925968481e42a258aac850d.
     * Observe bypass; do not silently discard someone else's mappings. */
    if (status[i] & 7u) return -EBUSY;
  }
  return 0;
}
