/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/fs/fs.h>
#include <sys/mount.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "sdhci_control.h"
#include "gpt_readonly.h"

#define BASE ((uintptr_t)0x2a330000)
#define CRUCLK ((uintptr_t)0x27200464) /* CLKSEL_CON89 */
static mutex_t lock=NXMUTEX_INITIALIZER;
static uint32_t rd(void *p,unsigned o) { (void)p; return *(volatile uint32_t *)(BASE+o); }
static void wr(void *p,unsigned o,uint32_t v) { (void)p; *(volatile uint32_t *)(BASE+o)=v; }
static uint8_t r8(void *p,unsigned o) { (void)p; return *(volatile uint8_t *)(BASE+o); }
static void w8(void *p,unsigned o,uint8_t v) { (void)p; *(volatile uint8_t *)(BASE+o)=v; }
static uint16_t r16(unsigned o) { return *(volatile uint16_t *)(BASE+o); }
static void w16(void *p,unsigned o,uint16_t v) { (void)p; *(volatile uint16_t *)(BASE+o)=v; }
static uint64_t now(void *p) {
  struct timespec t; (void)p; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static void pause_io(void *p) { (void)p; usleep(1000); }
static struct vv_sdhci_host host={{NULL,rd,wr,now,pause_io},w16,0,0,0};
static struct vv_sdhci_control ctl={&host,r8,w8,0,0,0};
static uint64_t card_now(void *p) { (void)p; return now(NULL); }
static void card_pause(void *p) { (void)p; pause_io(NULL); }
static struct vv_card_io cardio={&ctl,vv_sdhci_init_command,card_now,card_pause};
static struct vv_card card;
static struct vv_gpt table;
static uint64_t gpt_start;
static int read_gpt_sector(void *p,uint64_t lba,uint8_t *buffer) {
  struct vv_sdhci_command_result r; uint64_t elapsed=now(NULL)-gpt_start;
  (void)p;
  if(elapsed>=10000000)return -ETIMEDOUT;
  uint32_t left=(uint32_t)(10000000-elapsed);
  return vv_sdhci_read_sector(&host,lba,buffer,512,left<500000 ? left:500000,&r);
}

static void status(void) {
  printf("EMMC status version=%04x present=%08lx int=%08lx clock=%04x host=%02x power=%02x host2=%04x cru89=%08lx ready=%d recovery=%d\n",
    r16(0xfe),(unsigned long)rd(NULL,0x24),(unsigned long)rd(NULL,0x30),
    r16(0x2c),r8(NULL,0x28),r8(NULL,0x29),r16(0x3e),
    (unsigned long)*(volatile uint32_t *)CRUCLK,host.ready,host.needs_recovery);
}
static int prepare(void) {
  uint64_t start; int rc;
  /* Require a bootloader-powered controller. Do not guess supply/pinmux. */
  if (r16(0xfe)==0xffff || !(r8(NULL,0x29)&1)) return -ENODEV;
  rc=vv_sdhci_reset_lines(&ctl,100000);
  if (rc) return rc;
  w16(NULL,0x2c,0); /* card clock off before source/PHY transition */
  *(volatile uint32_t *)CRUCLK=0xff008000; /* hiword: OSC 24MHz /1 */
  if ((*(volatile uint32_t *)CRUCLK & 0xff00)!=0x8000) return -EIO;
  w8(NULL,0x28,r8(NULL,0x28)&~0x3e); /* legacy 1-bit, no DMA */
  w16(NULL,0x3e,r16(0x3e)&~0x0087); /* no HS400/tuning, keep 1.8V */
  wr(NULL,0x52c,rd(NULL,0x52c)&~0x101u); /* enhanced strobe/data strobe off */
  wr(NULL,0x508,rd(NULL,0x508)&~1u);
  wr(NULL,0x800,0x01000001); wr(NULL,0x804,0x80000000);
  wr(NULL,0x808,0); wr(NULL,0x810,0);
  wr(NULL,0x80c,0x0c100000); /* RK3576 U-Boot low-speed delay=16 */
  w8(NULL,0x2e,0x0e);
  wr(NULL,0x38,0); /* polling only; do not signal CPU IRQ */
  wr(NULL,0x34,0xffff8033u);
  w16(NULL,0x2c,0x1e01); /* 24MHz/(2*30)=400kHz */
  start=now(NULL);
  while (!(r16(0x2c)&2)) {
    if (now(NULL)-start>=100000) return -ETIMEDOUT;
    pause_io(NULL);
  }
  w16(NULL,0x2c,0x1e05);
  usleep(10000);
  return 0;
}
static uint32_t crc32(const uint8_t *b,size_t n) {
  uint32_t crc=~0u; unsigned j;
  while(n--) { crc^=*b++; for(j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1))); }
  return ~crc;
}
#include "block_readonly.inc"
int main(int argc,char **argv) {
  int rc=0;
  if (argc!=2 || (strcmp(argv[1],"status") && strcmp(argv[1],"probe") && strcmp(argv[1],"gpt") && strcmp(argv[1],"register") && strcmp(argv[1],"blockcheck"))) {
    puts("usage: k7emmc status|probe|gpt|register|blockcheck (read-only, no mount/write)"); return 1;
  }
  if(nxmutex_trylock(&lock)<0) { puts("EMMC busy"); return 1; }
  if(!strcmp(argv[1],"blockcheck")) {nxmutex_unlock(&lock);return block_check() ? 1:0;}
  if(registered && strcmp(argv[1],"status")) {nxmutex_unlock(&lock);puts("EMMC registered: reinitialization refused");return 1;}
  status();
  if (strcmp(argv[1],"status")) {
    uint8_t a[512], b[512]; struct vv_sdhci_command_result r; unsigned lba;
    memset(&card,0,sizeof(card));rc=prepare();
    if(!rc) rc=vv_emmc_initialize(&cardio,&card,2000000);
    printf("EMMC init rc=%d cmd=%u status=%08lx state=%08lx ocr=%08lx sectors=%lu revision=%u partition=%u bus=%u timing=%u\n",
      rc,ctl.last_command,(unsigned long)ctl.last_status,(unsigned long)ctl.last_state,
      (unsigned long)card.ocr,(unsigned long)card.sectors,card.revision,card.partition,card.bus_width,card.timing);
    if(!rc) { host.sector_count=card.sectors; host.ready=card.ready; }
    for(lba=0;lba<2 && !rc && !strcmp(argv[1],"probe");lba++) {
      rc=vv_sdhci_read_sector(&host,lba,a,sizeof(a),500000,&r);
      if(!rc) rc=vv_sdhci_read_sector(&host,lba,b,sizeof(b),500000,&r);
      if(!rc && memcmp(a,b,512)) rc=-EIO;
      printf("EMMC read lba=%u rc=%d bytes=%u status=%08lx r1=%08lx crc=%08lx repeated=%s signature=%s\n",
        lba,rc,(unsigned)r.data.bytes_read,(unsigned long)r.data.interrupt_status,
        (unsigned long)r.r1,(unsigned long)(rc ? 0 : crc32(a,512)),rc ? "FAIL":"PASS",
        rc ? "unknown" : lba==0 ? (a[510]==0x55 && a[511]==0xaa ? "MBR":"other") :
        (!memcmp(a,"EFI PART",8) ? "GPT":"other"));
    }
    if(!rc && (!strcmp(argv[1],"gpt") || !strcmp(argv[1],"register"))) {
      gpt_start=now(NULL);rc=vv_gpt_read(NULL,read_gpt_sector,host.sector_count,&table);
      printf("EMMC GPT rc=%d primary_backup_crc=%s partitions=%lu entries_crc=%08lx\n",rc,
        rc ? "FAIL":"PASS",(unsigned long)(rc ? 0:table.count),(unsigned long)table.entries_crc);
      if(!rc)for(unsigned i=0;i<table.count;i++) {
        const struct vv_gpt_partition *part=&table.partitions[i];
        printf("EMMC partition name=%s first=%llu last=%llu sectors=%llu\n",part->name,
          (unsigned long long)part->first,(unsigned long long)part->last,
          (unsigned long long)(part->last-part->first+1));
      }
      if(!rc && !strcmp(argv[1],"register"))rc=register_views();
    }
    if(rc) { host.ready=0; host.needs_recovery=1; }
    status(); printf("EMMC probe result=%d storage_writes=0\n",rc);
  }
  nxmutex_unlock(&lock);
  return rc ? 1:0;
}
