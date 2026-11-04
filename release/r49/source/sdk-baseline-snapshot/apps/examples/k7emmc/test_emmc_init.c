#include "emmc_init.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fake { unsigned n, mode; uint64_t time; };
static int command(void *p, unsigned cmd, uint32_t arg, enum vv_response type,
                   uint32_t r[4], uint8_t *data, uint32_t budget) {
  struct fake *f=p; const unsigned seq[]={0,1,2,3,9,7,8,13};
  (void)type; assert(budget>0);
  if (f->mode==1 && cmd==1) { r[0]=0x40ff8000; return 0; }
  assert(f->n<8 && seq[f->n++]==cmd);
  if (f->mode==2 && cmd==3) return -EIO;
  if (cmd==1) r[0]=f->mode==3 ? 0x80ff8000 : 0xc0ff8000;
  if (cmd==3 || cmd==9 || cmd==7 || cmd==13) assert(arg==0x10000);
  if (cmd==8) {
    assert(data); memset(data,0,512); data[192]=8; data[214]=0x80; data[215]=3;
    if (f->mode==4) data[179]=1;
    if (f->mode==5) data[61]=1;
    if (f->mode==6) data[183]=2;
    if (f->mode==7) data[185]=3;
    if (f->mode==8) data[214]=data[215]=0;
  } else assert(!data);
  if (cmd==13) r[0]=f->mode==9 ? 0x700 : 0x900;
  return 0;
}
static uint64_t now(void *p) { return ((struct fake *)p)->time; }
static void pause_io(void *p) { ((struct fake *)p)->time+=1000; }
int main(void) {
  struct fake f; struct vv_card c;
  struct vv_card_io io={&f,command,now,pause_io}; unsigned mode;
  for (mode=0; mode<10; ++mode) {
    int expected=mode==0 ? 0 : mode==1 ? -ETIMEDOUT :
                 (mode==2 || mode==9) ? -EIO : -ENOTSUP;
    memset(&f,0,sizeof(f)); f.mode=mode;
    assert(vv_emmc_initialize(&io,&c,10000)==expected);
    assert(c.ready==(mode==0));
    if (!mode) assert(c.sectors==0x03800000 && f.n==8);
  }
  puts("PASS: init sequence, OCR timeout, command error, byte-address rejection, partition/sector/bus/timing/capacity guards, final TRAN check");
  return 0;
}
