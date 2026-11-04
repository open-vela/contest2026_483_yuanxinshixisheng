#include "sdhci_control.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fake { unsigned mode, writes, reset, cmd; uint64_t time; };
static uint32_t rd(void *p,unsigned o) {
  struct fake *f=p;
  if(o==0x24) return f->mode==4 ? 2 : 0;
  if(o==0x30) return f->mode==2 ? 0 : f->mode==3 ? 0x28000 : 1;
  assert(o>=0x10 && o<=0x1c); return 0;
}
static void wr(void *p,unsigned o,uint32_t v) { struct fake *f=p; (void)o;(void)v;f->writes++; }
static void w16(void *p,unsigned o,uint16_t v) {
  struct fake *f=p; f->writes++; if(o==0x0e) f->cmd=v>>8;
}
static uint8_t r8(void *p,unsigned o) { struct fake *f=p;assert(o==0x2f);return f->mode==1 ? 6:0; }
static void w8(void *p,unsigned o,uint8_t v) { struct fake *f=p; assert(o==0x2f && v==6);f->reset++; }
static uint64_t now(void *p) { return ((struct fake *)p)->time; }
static void pause_io(void *p) { ((struct fake *)p)->time+=10; }
int main(void) {
  struct fake f={0}; uint32_t r[4]; unsigned before,mode;
  struct vv_sdhci_host h={{&f,rd,wr,now,pause_io},w16,0,0,0};
  struct vv_sdhci_control c={&h,r8,w8,0,0,0};
  assert(vv_sdhci_reset_lines(&c,100)==0 && !h.ready && !h.needs_recovery);
  assert(vv_sdhci_init_command(&c,0,0,VV_NONE,r,NULL,100)==0);
  before=f.writes;
  assert(vv_sdhci_init_command(&c,24,0,VV_R1,r,NULL,100)==-EINVAL);
  assert(vv_sdhci_init_command(&c,6,0,VV_R1,r,NULL,100)==-EINVAL);
  assert(vv_sdhci_init_command(&c,38,0,VV_R1B,r,NULL,100)==-EINVAL);
  assert(f.writes==before);
  for(mode=1;mode<=4;mode++) {
    memset(&f,0,sizeof(f));f.mode=mode;h.ready=0;h.needs_recovery=0;
    if(mode==1) { assert(vv_sdhci_reset_lines(&c,100)==-ETIMEDOUT); assert(h.needs_recovery); }
    else {
      assert(vv_sdhci_init_command(&c,0,0,VV_NONE,r,NULL,100)==(mode==3 ? -EIO:-ETIMEDOUT));
      assert(h.needs_recovery==(mode!=4));
    }
  }
  puts("PASS: reset, command completion, write/switch/erase rejection, reset timeout, command timeout/error, inhibit timeout");
  return 0;
}
