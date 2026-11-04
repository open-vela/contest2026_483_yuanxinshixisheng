#include "sdhci_read.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fake { uint64_t time; unsigned mode, words, writes; };
static uint32_t read32(void *p, unsigned reg) {
  struct fake *f=p;
  if (reg == 0x20) { assert(f->words < 128); f->words++; return 0x78563412; }
  if (reg == 0x24) return f->mode == 1 ? 0 : (1u << 11);
  assert(reg == 0x30);
  if (f->mode == 2 || (f->mode == 5 && f->words == 128)) return 0x200000;
  if (f->mode == 3) return 2;
  if (f->words == 128) return f->mode == 4 ? 0 : 2;
  return 32;
}
static void write32(void *p, unsigned reg, uint32_t v) {
  struct fake *f=p; assert(reg == 0x30 && (v == 32 || v == 2)); f->writes++;
}
static uint64_t now(void *p) {
  struct fake *f=p;
  if (f->mode == 6 && f->words) f->time += 10;
  return f->time;
}
static void pause_io(void *p) { ((struct fake *)p)->time += 10; }
int main(void) {
  struct fake f;
  struct vv_sdhci_result r;
  struct vv_sdhci_io io={&f,read32,write32,now,pause_io};
  uint8_t b[514];
  unsigned mode;
  const int expected[]={0,-ETIMEDOUT,-EIO,-EIO,-ETIMEDOUT,-EIO};
  for (mode=0; mode<6; ++mode) {
    memset(&f,0,sizeof(f)); f.mode=mode; memset(b,0xa5,sizeof(b));
    assert(vv_sdhci_read512(&io,b+1,512,100,&r)==expected[mode]);
    assert(b[0]==0xa5 && b[513]==0xa5);
    if (mode==0) { assert(r.bytes_read==512 && f.writes==2); assert(b[1]==0x12 && b[512]==0x78); }
    if (mode==1) { assert(f.words==0 && f.writes==0 && r.elapsed_us==100); }
    if (mode==4) assert(r.bytes_read==512 && f.writes==1);
  }
  memset(&f,0,sizeof(f)); f.time=UINT64_MAX-5;
  assert(vv_sdhci_read512(&io,b+1,512,100,&r)==0);
  assert(r.elapsed_us==10);
  memset(&f,0,sizeof(f)); f.mode=6;
  assert(vv_sdhci_read512(&io,b+1,512,100,&r)==-ETIMEDOUT);
  assert(r.bytes_read > 0 && r.bytes_read < 512);
  memset(&f,0,sizeof(f));
  assert(vv_sdhci_read512(&io,b,511,100,&r)==-EINVAL);
  assert(vv_sdhci_read512(&io,b,512,0,&r)==-EINVAL);
  assert(f.words==0 && f.writes==0);
  puts("PASS: data, timeout/status disagreement, error, premature end, missing end, late error, clock wrap, mid-PIO timeout, invalid arguments");
  return 0;
}
