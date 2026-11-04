#include "gpt_readonly.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static uint8_t disk[100][512];static unsigned reads;static int fail;
static uint32_t crc(const uint8_t *p,unsigned n) {uint32_t c=~0u;unsigned j;while(n--){c^=*p++;for(j=0;j<8;j++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static void put(uint8_t *p,uint32_t n) {p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static void fixheader(unsigned sector) {put(disk[sector]+16,0);put(disk[sector]+16,crc(disk[sector],92));}
static void setup(void) {
  unsigned s;memset(disk,0,sizeof(disk));reads=0;fail=0;
  disk[0][510]=0x55;disk[0][511]=0xaa;disk[0][450]=0xee;put(disk[0]+454,1);
  disk[2][0]=1;disk[2][16]=2;put(disk[2]+32,3);put(disk[2]+40,10);disk[2][56]='x';
  memcpy(disk[98],disk[2],512);
  for(s=1;s<=99;s+=98) {
    uint8_t *b=disk[s];memcpy(b,"EFI PART",8);put(b+8,0x10000);put(b+12,92);
    put(b+24,s);put(b+32,s==1 ? 99:1);put(b+40,3);put(b+48,97);b[56]=1;
    put(b+72,s==1 ? 2:98);put(b+80,4);put(b+84,128);put(b+88,crc(disk[2],512));fixheader(s);
  }
}
static int rd(void *p,uint64_t lba,uint8_t *b) {(void)p;assert(lba<100);reads++;if(fail)return -ETIMEDOUT;memcpy(b,disk[lba],512);return 0;}
int main(void) {
  struct vv_gpt t;
  setup();assert(vv_gpt_read(NULL,rd,100,&t)==0 && t.count==1 && !strcmp(t.partitions[0].name,"x") && reads==5);
  setup();disk[1][56]^=2;assert(vv_gpt_read(NULL,rd,100,&t)==-EIO);
  setup();disk[98][0]^=1;assert(vv_gpt_read(NULL,rd,100,&t)==-EIO);
  setup();put(disk[1]+72,100);fixheader(1);assert(vv_gpt_read(NULL,rd,100,&t)==-EINVAL && reads==2);
  setup();put(disk[1]+80,129);fixheader(1);assert(vv_gpt_read(NULL,rd,100,&t)==-ENOTSUP);
  setup();put(disk[99]+48,96);fixheader(99);assert(vv_gpt_read(NULL,rd,100,&t)==-EINVAL);
  setup();memcpy(disk[2]+128,disk[2],128);disk[2][144]=3;memcpy(disk[98],disk[2],512);
  put(disk[1]+88,crc(disk[2],512));put(disk[99]+88,crc(disk[2],512));fixheader(1);fixheader(99);
  assert(vv_gpt_read(NULL,rd,100,&t)==-EINVAL);
  setup();fail=1;assert(vv_gpt_read(NULL,rd,100,&t)==-ETIMEDOUT && reads==1);
  puts("PASS: GPT primary/backup CRC, entries CRC, bounds, unsupported counts, backup mismatch, overlap, read failure");return 0;
}
