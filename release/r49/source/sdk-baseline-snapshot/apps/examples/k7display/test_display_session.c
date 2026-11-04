/* SPDX-License-Identifier: GPL-2.0-only
 * Orchestration test. Driver boundary fakes enforce prerequisites/order and
 * inject failures; this does not replace individual MMIO driver tests. */
#include "display_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum event { PREPARE=1, RESET_PANEL, PREPARE_DSI, PANEL_INIT, CONFIG_VOP,
             ENABLE_VOP, ENABLE_DSI, LIGHT, DARK, DISABLE_DSI, DISABLE_VOP,
             PANEL_EXIT, SHUT_DSI, POWER_OFF };
static enum event events[256]; static unsigned used;
static unsigned failures; static int preflight_failure;
static struct k7display_session *active;
static int event(enum event e)
{
  assert(used < 256); events[used++] = e;
  return failures & (1u << e) ? -EIO : 0;
}
static unsigned pos(enum event e)
{
  unsigned i; for (i=0;i<used;i++) if (events[i]==e) return i;
  return 256;
}
const unsigned char k7panel_init[] = {1}, k7panel_exit[] = {2};
const size_t k7panel_init_size = 1, k7panel_exit_size = 1;
int k7panel_validate(const unsigned char *data,size_t size)
{ (void)size; return data == k7panel_init ? 171 : 2; }
int k7panel_run(const unsigned char *d,size_t n,const struct k7panel_io *io)
{
  (void)n; (void)io; assert(active->dsi.prepared);
  return event(d == k7panel_init ? PANEL_INIT : PANEL_EXIT);
}
int k7pb_prepare(const struct k7pb_io *io,struct k7pb_state *s,unsigned proofs)
{
  int ret; (void)io; assert(proofs == K7PB_ALL_PROOFS);
  s->touched=1; ret=event(PREPARE);
  s->phase = ret ? K7PB_FAULT : K7PB_POWERED; return ret;
}
int k7pb_reset(const struct k7pb_io *io,struct k7pb_state *s)
{
  int ret; (void)io; assert(s->phase == K7PB_POWERED);
  ret=event(RESET_PANEL); s->phase=ret ? K7PB_FAULT : K7PB_RESET_DONE;
  return ret;
}
int k7pb_backlight(const struct k7pb_io *io,struct k7pb_state *s,unsigned pct,int ready)
{
  int ret; (void)io;
  if (pct) assert(ready && active->vop.enabled && active->dsi.video);
  ret=event(pct ? LIGHT : DARK);
  if (!ret) s->phase=pct ? K7PB_LIT : K7PB_RESET_DONE;
  return ret;
}
int k7pb_off(const struct k7pb_io *io,struct k7pb_state *s)
{ int ret; (void)io; ret=event(POWER_OFF); if (!ret) s->phase=K7PB_OFF; return ret; }
int k7_dsi2_init(struct k7_dsi2 *d,const struct k7_display_io *io)
{ memset(d,0,sizeof(*d)); d->io=*io; return 0; }
int k7_dsi2_prepare(struct k7_dsi2 *d,uint32_t sys,uint32_t ref,bool ready)
{
  int ret; assert(sys==100000000 && ref==24000000 && ready);
  assert(active->panel.phase==K7PB_RESET_DONE);
  d->touched=true; ret=event(PREPARE_DSI); d->prepared=ret==0; return ret;
}
int k7_dsi2_panel_tx(void *a,unsigned char t,const unsigned char *b,size_t n)
{ (void)a;(void)t;(void)b;return (int)n; }
int k7_dsi2_video_enable(struct k7_dsi2 *d,bool ready)
{
  int ret; assert(ready && active->vop.enabled);
  ret=event(ENABLE_DSI); d->video=ret==0; return ret;
}
int k7_dsi2_video_disable(struct k7_dsi2 *d)
{
  int ret; assert(active->vop.enabled);
  ret=event(DISABLE_DSI); if (!ret) d->video=false; return ret;
}
int k7_dsi2_shutdown(struct k7_dsi2 *d)
{
  int ret=event(SHUT_DSI);
  if (!ret) { d->touched=false; d->prepared=false; d->video=false; }
  return ret;
}
int k7vop_validate_frame(const struct k7vop_frame *f) { (void)f; return 0; }
int k7vop_init(struct k7vop *v,const struct k7vop_io *io)
{ memset(v,0,sizeof(*v));v->io=*io;return 0; }
int k7vop_configure(struct k7vop *v,const struct k7vop_frame *f)
{
  int ret; (void)f; assert(pos(PANEL_INIT)<used);
  v->touched=1; v->dma_stopped=0;
  ret=event(CONFIG_VOP); v->configured=ret==0; return ret;
}
int k7vop_enable(struct k7vop *v,unsigned timeout)
{
  int ret; assert(v->configured && timeout<=200000);
  ret=event(ENABLE_VOP); v->enabled=ret==0; return ret;
}
int k7vop_disable(struct k7vop *v)
{
  int ret=event(DISABLE_VOP);
  if (!ret) { v->enabled=0; v->touched=0; v->dma_stopped=1; }
  return ret;
}
static int unused_read(void *a,uintptr_t r,uint32_t *v)
{ (void)a;(void)r;(void)v;assert(0);return -EIO; }
static int unused_write(void *a,uintptr_t r,uint32_t v)
{ (void)a;(void)r;(void)v;assert(0);return -EIO; }
static int unused_delay(void *a,unsigned ms) { (void)a;(void)ms;return 0; }
static int preflight(void *a,unsigned *p,uint32_t *hz)
{ (void)a;*p=K7PB_ALL_PROOFS;*hz=100000000;return preflight_failure; }
static void fresh(struct k7display_session *s,struct k7display_platform *p)
{
  memset(s,0,sizeof(*s));memset(p,0,sizeof(*p));
  active=s;used=0;failures=0;preflight_failure=0;
  p->preflight=preflight;p->panel.read=unused_read;
  p->panel.write=unused_write;p->panel.delay_ms=unused_delay;
}
int main(void)
{
  struct k7display_session s;struct k7display_platform p;unsigned i;
  fresh(&s,&p);preflight_failure=-ENOSYS;
  assert(k7display_start(&s,&p,20)==-ENOSYS && used==0);
  fresh(&s,&p);assert(k7display_start(&s,&p,20)==0);
  assert(s.started && pos(ENABLE_VOP)<pos(ENABLE_DSI) && pos(ENABLE_DSI)<pos(LIGHT));
  assert(k7display_stop(&s)==0 && !s.touched && !s.started);
  assert(pos(DARK)<pos(DISABLE_DSI) && pos(DISABLE_DSI)<pos(DISABLE_VOP));
  assert(pos(DISABLE_VOP)<pos(PANEL_EXIT) && pos(PANEL_EXIT)<pos(SHUT_DSI));
  assert(pos(SHUT_DSI)<pos(POWER_OFF));
  /* All start stages abort subsequent enable and retain original failure. */
  for(i=PREPARE;i<=LIGHT;i++) {
    fresh(&s,&p);failures=1u<<i;
    assert(k7display_start(&s,&p,20)==-EIO && !s.started && s.primary_error==-EIO);
    assert(pos(POWER_OFF)<256);
    if(i<LIGHT) assert(pos(LIGHT)==256);
    if(i>=CONFIG_VOP) assert(pos(DISABLE_VOP)<256);
  }
  fresh(&s,&p);assert(k7display_start(&s,&p,20)==0);
  failures=(1u<<DISABLE_VOP)|(1u<<POWER_OFF);
  assert(k7display_stop(&s)==-EIO && s.touched && s.cleanup_error==-EIO);
  assert(s.cleanup_failures & 4u);assert(s.cleanup_failures & 32u);
  /* Explicit successful cleanup later frees lease but keeps failure evidence. */
  failures=0;
  assert(k7display_stop(&s)==0 && !s.touched && s.cleanup_error==-EIO);
  assert(s.last_stop_error==0);
  puts("PASS session: ordered enable/stop, preflight gate, each start failure,");
  puts("multi-stage stop failure, retained lease then explicit cleanup/history");
  return 0;
}
