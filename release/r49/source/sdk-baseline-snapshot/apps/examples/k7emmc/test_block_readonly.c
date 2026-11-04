/* Host fake inode registry: exercises driver range/ownership/error policy. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include "sdhci_command.h"
#include "gpt_readonly.h"
#define ssize_t intptr_t
#define blkcnt_t int64_t
#define MS_RDONLY 1
#define NAME_MAX 64
struct inode;
struct geometry { bool geo_available,geo_mediachanged,geo_writeenabled;blkcnt_t geo_nsectors;unsigned geo_sectorsize;char geo_model[65]; };
struct block_operations {
  int (*open)(struct inode *);int (*close)(struct inode *);
  ssize_t (*read)(struct inode *,unsigned char *,blkcnt_t,unsigned);
  ssize_t (*write)(struct inode *,const unsigned char *,blkcnt_t,unsigned);
  int (*geometry)(struct inode *,struct geometry *);int (*ioctl)(struct inode *,int,unsigned long);
};
struct inode { void *i_private;union { const struct block_operations *i_bops; } u; };
static int lock,io_count,fail_at,registry_count,registry_fail;
static uint64_t tick;
static struct vv_sdhci_host host;
static struct vv_gpt table;
static struct inode registry[130];static char names[130][32];
static int nxmutex_trylock(int *p) {if(*p)return -EBUSY;*p=1;return 0;}
static void nxmutex_unlock(int *p) {assert(*p);*p=0;}
static uint64_t now(void *p) {(void)p;return tick++;}
static int register_blockdriver(const char *p,const struct block_operations *ops,int mode,void *priv) {
  assert(mode==0444);if(registry_count==registry_fail)return -ENOMEM;
  snprintf(names[registry_count],32,"%s",p);registry[registry_count++]=(struct inode){.i_private=priv,.u={.i_bops=ops}};return 0;
}
static int unregister_blockdriver(const char *p) {assert(registry_count>0&&!strcmp(p,names[registry_count-1]));registry_count--;return 0;}
static int open_blockdriver(const char *p,int flags,struct inode **out) {
  int i;assert(flags==MS_RDONLY);for(i=0;i<registry_count;i++)if(!strcmp(p,names[i])){*out=&registry[i];return (*out)->u.i_bops->open(*out);}return -ENOENT;
}
static int close_blockdriver(struct inode *p) {return p->u.i_bops->close(p);}
int vv_sdhci_read_sector(struct vv_sdhci_host *h,uint64_t lba,uint8_t *b,size_t size,uint32_t budget,struct vv_sdhci_command_result *r) {
  assert(lock && size==512 && budget>0 && lba<h->sector_count);io_count++;
  memset(r,0,sizeof(*r));if(io_count==fail_at){h->needs_recovery=1;return -EIO;}
  memset(b,(int)(lba&255),512);if(lba==0){b[510]=0x55;b[511]=0xaa;}return 0;
}
#include "block_readonly.inc"
static void setup(void) {
  lock=io_count=registry_count=0;fail_at=registry_fail=-1;tick=0;registered=0;
  memset(&host,0,sizeof(host));host.ready=1;host.sector_count=1000;
  memset(&table,0,sizeof(table));table.count=2;table.partitions[0].first=10;table.partitions[0].last=19;
  table.partitions[1].first=20;table.partitions[1].last=29;
}
int main(void) {
  uint8_t b[1024];struct geometry g;int n;
  setup();assert(register_views()==0 && registry_count==3);
  assert(block_check()==0);assert(block_geometry(&registry[1],&g)==0 && g.geo_nsectors==10 && !g.geo_writeenabled);
  n=io_count;
  assert(block_read(&registry[1],b,-1,1)==-EINVAL);
  assert(block_read(&registry[1],b,9,2)==-EINVAL);
  assert(block_read(&registry[0],b,0,129)==-E2BIG);
  assert(block_write(&registry[0],b,0,1)==-EROFS && io_count==n);
  lock=1;assert(block_read(&registry[0],b,0,1)==-EBUSY && io_count==n);lock=0;
  fail_at=io_count+2;assert(block_read(&registry[0],b,0,2)==1 && host.needs_recovery);
  n=io_count;assert(block_read(&registry[0],b,0,1)==-ENODEV && io_count==n);
  setup();registry_fail=2;assert(register_views()==-ENOMEM && !registry_count && !registered);
  puts("PASS: block geometry, write rejection, range/size guards, busy, partial error/recovery gate, partition translation, registration rollback");return 0;
}
