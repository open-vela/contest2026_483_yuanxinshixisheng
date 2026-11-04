/* SPDX-License-Identifier: Apache-2.0 */
#include "rk3576_panel_board.h"
#include <errno.h>

/* Facts/sequence sources:
 * local official rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi:
 * GPIO0_C6 active-high supply, GPIO0_A2 active-low reset, PWM1_CH1_M0,
 * 25000ns normal-polarity PWM; prepare/reset/init/enable=120ms,
 * disable/unprepare=60ms.
 * https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/gpu/drm/panel/panel-simple.c
 * prepare/unprepare ordering was reviewed; this code is original, not copied.
 * TRM PWM scale=1 gives /2; use 24MHz /2 =12MHz, period=300.
 */
#define IOC_A  ((uintptr_t)0x26040000u)
#define IOC_B  ((uintptr_t)0x26042000u)
#define IOC_C  ((uintptr_t)0x26042008u)
#define GDRL   ((uintptr_t)0x27320000u)
#define GDRH   ((uintptr_t)0x27320004u)
#define GDDRL  ((uintptr_t)0x27320008u)
#define GDDRH  ((uintptr_t)0x2732000cu)
#define GVER   ((uintptr_t)0x27320078u)
#define GGATE  ((uintptr_t)0x2722081cu)
#define PGATE  ((uintptr_t)0x27200840u)
#define PMUX   ((uintptr_t)0x2720041cu)
#define PWM    ((uintptr_t)0x2add1000u)
#define BIT(n) (1u << (n))

static int valid(const struct k7pb_io *p, const struct k7pb_state *s)
{
  return p && s && p->read && p->write && p->delay_ms;
}
static int readv(const struct k7pb_io *p, uintptr_t a, uint32_t *v)
{
  int r=p->read(p->arg,a,v); return r>0 ? -EIO : r;
}
static int delay(const struct k7pb_io *p, unsigned ms)
{
  int r=p->delay_ms(p->arg,ms); return r>0 ? -EIO : r;
}
static int put(const struct k7pb_io *p, uintptr_t a, uint32_t mask,
               uint32_t value, int hiword)
{
  uint32_t got;
  int r=p->write(p->arg,a,hiword ? (mask<<16)|(value&mask) : value);
  if (r) return r<0 ? r : -EIO;
  r=readv(p,a,&got);
  return r ? r : ((got&mask)==(value&mask) ? 0 : -EIO);
}
static void first(int *r, int next) { if (!*r && next) *r=next; }

/* Stage output latch before direction/mux. Continue each independent safety
 * write even when one fails; no retries or assumption of successful cleanup. */
static int low_pin(const struct k7pb_io *p, uintptr_t dr, uintptr_t ddr,
                   uint32_t bit, uintptr_t mux, uint32_t field)
{
  int r=0;
  first(&r,put(p,dr,bit,0,1));
  first(&r,put(p,ddr,bit,bit,1));
  first(&r,put(p,mux,field,0,1));
  return r;
}
static int dark(const struct k7pb_io *p)
{
  return low_pin(p,GDRL,GDDRL,BIT(13),IOC_B,0xf0);
}
static int shutdown(const struct k7pb_io *p, struct k7pb_state *s)
{
  int r=0;
  first(&r,dark(p));
  if (s->pwm_accessible) first(&r,put(p,PWM+4,3,0,1));
  first(&r,delay(p,60));
  first(&r,low_pin(p,GDRL,GDDRL,BIT(2),IOC_A,0xf00));
  first(&r,low_pin(p,GDRH,GDDRH,BIT(6),IOC_C,0xf00));
  first(&r,delay(p,60));
  s->last_cleanup_error=r;
  if(r && !s->cleanup_error) s->cleanup_error=r;
  s->phase=r ? K7PB_FAULT : K7PB_OFF;
  return r;
}
static int fail(const struct k7pb_io *p, struct k7pb_state *s, int r)
{
  s->primary_error=r;
  if (s->touched) (void)shutdown(p,s);
  s->phase=K7PB_FAULT;
  return r;
}

int k7pb_prepare(const struct k7pb_io *p, struct k7pb_state *s, unsigned proofs)
{
  uint32_t v;
  int r;
  if (!valid(p,s)) return -EINVAL;
  if ((proofs & K7PB_ALL_PROOFS)!=K7PB_ALL_PROOFS) return -EPERM;
  if (s->phase!=K7PB_OFF) return -EBUSY;
  s->proofs=proofs; s->primary_error=0; s->last_cleanup_error=0;
  s->pwm_accessible=0;
  /* Gate operation is explicitly authorized by prepare. GPIO0 gate failure
   * cannot safely be followed by GPIO MMIO, so it returns without cleanup. */
  if ((r=put(p,GGATE,BIT(6),0,1))<0) {
    s->phase=K7PB_FAULT; s->primary_error=r; return r;
  }
  if ((r=readv(p,GVER,&v))<0 || v!=0x010219c8u) {
    r=r ? r : -ENODEV; s->phase=K7PB_FAULT; s->primary_error=r; return r;
  }
  s->touched=1;
  if ((r=dark(p))<0) return fail(p,s,r);
  if ((r=low_pin(p,GDRL,GDDRL,BIT(2),IOC_A,0xf00))<0) return fail(p,s,r);
  if ((r=low_pin(p,GDRH,GDDRH,BIT(6),IOC_C,0xf00))<0) return fail(p,s,r);
  if ((r=put(p,PGATE,BIT(10)|BIT(11),0,1))<0) return fail(p,s,r);
  if ((r=readv(p,PWM,&v))<0) return fail(p,s,r);
  if ((v>>24)!=4 || ((v>>4)&15)!=1) return fail(p,s,-ENODEV);
  if ((r=readv(p,PWM+4,&v))<0) return fail(p,s,r);
  /* Global join is W1T; do not try to clear/steal a synchronized channel. */
  if (v&BIT(4)) return fail(p,s,-EBUSY);
  s->pwm_accessible=1;
  if ((r=put(p,PWM+4,3,0,1))<0) return fail(p,s,r);
  if ((r=put(p,PMUX,0x300,0x200,1))<0) return fail(p,s,r);
  if ((r=put(p,PWM+8,0xfff7,0x10,1))<0) return fail(p,s,r);
  if ((r=put(p,PWM+12,0x3f,5,1))<0) return fail(p,s,r);
  if ((r=put(p,PWM+16,UINT32_MAX,300,0))<0) return fail(p,s,r);
  if ((r=put(p,PWM+20,UINT32_MAX,0,0))<0) return fail(p,s,r);
  if ((r=put(p,GDRH,BIT(6),BIT(6),1))<0) return fail(p,s,r);
  if ((r=delay(p,120))<0) return fail(p,s,r);
  s->phase=K7PB_POWERED;
  return 0;
}

int k7pb_reset(const struct k7pb_io *p, struct k7pb_state *s)
{
  int r;
  if (!valid(p,s)) return -EINVAL;
  if (s->phase!=K7PB_POWERED) return -EPERM;
  if ((r=put(p,GDRL,BIT(2),0,1))<0 || (r=delay(p,120))<0 ||
      (r=put(p,GDRL,BIT(2),BIT(2),1))<0 || (r=delay(p,120))<0)
    return fail(p,s,r);
  s->phase=K7PB_RESET_DONE;
  return 0;
}

int k7pb_backlight(const struct k7pb_io *p, struct k7pb_state *s,
                  unsigned percent, int panel_and_video_ready)
{
  uint32_t v;
  unsigned n;
  int r;
  if (!valid(p,s) || percent>100) return -EINVAL;
  if (s->phase!=K7PB_RESET_DONE && s->phase!=K7PB_LIT) return -EPERM;
  if (percent && !panel_and_video_ready) return -EPERM;
  if ((r=dark(p))<0 || (r=put(p,PWM+4,3,0,1))<0) return fail(p,s,r);
  if (!percent) { s->phase=K7PB_RESET_DONE; return 0; }
  if ((r=delay(p,120))<0 ||
      (r=put(p,PWM+20,UINT32_MAX,percent*3,0))<0) return fail(p,s,r);
  /* W1T commit cannot be checked by reading back a persistent 1. Start while
   * the pin remains GPIO-low; bounded polling confirms the update consumed. */
  r=p->write(p->arg,PWM+4,(BIT(2)<<16)|BIT(2));
  if (r) return fail(p,s,r<0 ? r : -EIO);
  if ((r=put(p,PWM+4,3,3,1))<0) return fail(p,s,r);
  for (n=0;n<10;n++) {
    if ((r=readv(p,PWM+4,&v))<0) return fail(p,s,r);
    if (!(v&BIT(2))) break;
    if ((r=delay(p,1))<0) return fail(p,s,r);
  }
  if (n==10) return fail(p,s,-ETIMEDOUT);
  if ((r=put(p,IOC_B,0xf0,0xc0,1))<0) return fail(p,s,r);
  s->phase=K7PB_LIT;
  return 0;
}

int k7pb_blank(const struct k7pb_io *p, const struct k7pb_state *s, int blank)
{
  uint32_t control, period, duty;
  int r;
  if (!valid(p,s) || (blank!=0 && blank!=1)) return -EINVAL;
  if (s->phase!=K7PB_LIT || !s->touched || !s->pwm_accessible ||
      (s->proofs&K7PB_ALL_PROOFS)!=K7PB_ALL_PROOFS) return -EPERM;
  /* Keep the already configured PWM running. GPIO-low hides its output;
   * restoring the existing mux resumes exactly the previous brightness.
   * Unlike startup failure cleanup, visibility errors must not reset the
   * panel, stop scanout or release a live framebuffer. */
  if (blank) return dark(p);
  if ((r=readv(p,PWM+4,&control))<0 ||
      (r=readv(p,PWM+16,&period))<0 ||
      (r=readv(p,PWM+20,&duty))<0) return r;
  if ((control&7)!=3 || period!=300 || !duty || duty>period) return -EPIPE;
  return put(p,IOC_B,0xf0,0xc0,1);
}

int k7pb_off(const struct k7pb_io *p, struct k7pb_state *s)
{
  if (!valid(p,s)) return -EINVAL;
  if (s->phase==K7PB_OFF && !s->touched) return 0;
  if (!s->touched || (s->proofs&K7PB_ALL_PROOFS)!=K7PB_ALL_PROOFS)
    return -EPERM;
  return shutdown(p,s);
}
