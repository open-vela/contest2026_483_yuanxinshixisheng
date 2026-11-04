#include "sdhci_command.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fake { uint64_t time; unsigned mode, issued, ack, words, writes; };
static uint32_t read32(void *p, unsigned reg) {
  struct fake *f=p;
  if (reg==0x24) return f->mode==1 ? 3 : (1u<<11);
  if (reg==0x10) return f->mode==3 ? (1u<<22) : 0x900;
  if (reg==0x20) { assert(f->ack && f->words<128); ++f->words; return 0x44332211; }
  assert(reg==0x30 && f->issued);
  if (f->mode==2) return 0;
  if (f->mode==6 && f->time<60) return 0;
  if (f->mode==4) return 0x28000;
  if (!f->ack) return 1;
  return f->words==128 ? (f->mode==5 || f->mode==6 ? 0 : 2) : 32;
}
static void write32(void *p, unsigned reg, uint32_t v) {
  struct fake *f=p; ++f->writes;
  if (reg==0x08) { assert(v==7); return; }
  assert(reg==0x30);
  if (v==1) { assert(f->issued); f->ack=1; }
  else assert(v==32 || v==2 || v==0xffff8033u);
}
static void write16(void *p, unsigned reg, uint16_t v) {
  struct fake *f=p; ++f->writes;
  switch(reg) {
    case 4: assert(v==512); break;
    case 6: assert(v==1); break;
    case 12: assert(v==0x12); break;
    case 14: assert(v==0x113a && !f->issued); f->issued=1; break;
    default: assert(0);
  }
}
static uint64_t now(void *p) { return ((struct fake *)p)->time; }
static void pause_io(void *p) { ((struct fake *)p)->time+=10; }
int main(void) {
  struct fake f;
  struct vv_sdhci_host h={{&f,read32,write32,now,pause_io},write16,100,1,0};
  struct vv_sdhci_command_result r;
  uint8_t b[512]; unsigned mode, writes;
  const int expected[]={0,-ETIMEDOUT,-ETIMEDOUT,-EIO,-EIO,-ETIMEDOUT,-ETIMEDOUT};
  for (mode=0; mode<7; ++mode) {
    memset(&f,0,sizeof(f)); f.mode=mode; h.needs_recovery=0;
    assert(vv_sdhci_read_sector(&h,7,b,sizeof(b),100,&r)==expected[mode]);
    if (mode==6) assert(r.elapsed_us==100 && r.data.bytes_read==512);
    if (mode==0) assert(r.data.bytes_read==512 && b[0]==0x11 && !h.needs_recovery);
    if (mode==1) assert(!f.writes && !r.command_issued);
    if (mode>=2) {
      assert(h.needs_recovery && r.command_issued);
      writes=f.writes;
      assert(vv_sdhci_read_sector(&h,7,b,sizeof(b),100,&r)==-EAGAIN);
      assert(f.writes==writes);
    }
    if (mode==3 || mode==4) assert(f.words==0);
  }
  memset(&f,0,sizeof(f)); h.needs_recovery=0;
  f.time=UINT64_MAX-5;
  assert(vv_sdhci_read_sector(&h,7,b,512,100,&r)==0 && r.elapsed_us==10);
  memset(&f,0,sizeof(f));
  assert(vv_sdhci_read_sector(&h,100,b,512,100,&r)==-ERANGE);
  h.sector_count=UINT64_MAX;
  assert(vv_sdhci_read_sector(&h,UINT64_C(0x100000000),b,512,100,&r)==-ERANGE);
  h.ready=0;
  assert(vv_sdhci_read_sector(&h,7,b,512,100,&r)==-EAGAIN);
  assert(!f.writes);
  puts("PASS: CMD17 read, inhibit deadline, missing response, R1 rejection, controller error, missing data end, shared deadline, clock wrap, recovery gate, range/init guards");
  return 0;
}
