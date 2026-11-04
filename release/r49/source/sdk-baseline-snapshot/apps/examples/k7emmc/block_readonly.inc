/* NuttX-facing read-only views; included after the controller and mutex.
 * No storage write path. Registration is explicit and pins the controller
 * configuration for the rest of this RAM boot. All reads share the mutex.
 */
struct emmc_view { uint64_t first,count; };
static struct emmc_view views[VV_GPT_MAX_PARTITIONS+1];
static int registered;
static int block_open(struct inode *inode) {
  int rc;(void)inode;if(nxmutex_trylock(&lock)<0)return -EBUSY;
  rc=registered && host.ready && !host.needs_recovery ? 0:-ENODEV;
  nxmutex_unlock(&lock);return rc;
}
static int block_close(struct inode *inode) { (void)inode;return 0; }
static ssize_t block_read(struct inode *inode,unsigned char *buffer,
                           blkcnt_t first,unsigned int count) {
  struct emmc_view *v=inode->i_private;unsigned done=0;int rc=0;
  uint64_t start=now(NULL);
  if(!count)return 0;
  if(!buffer || first<0 || (uint64_t)first>=v->count || count>v->count-(uint64_t)first)return -EINVAL;
  if(count>128)return -E2BIG;
  if(nxmutex_trylock(&lock)<0)return -EBUSY;
  if(!registered || !host.ready || host.needs_recovery)rc=-ENODEV;
  while(!rc && done<count) {
    struct vv_sdhci_command_result r;uint64_t elapsed=now(NULL)-start;
    if(elapsed>=5000000) {rc=-ETIMEDOUT;break;}
    uint32_t left=(uint32_t)(5000000-elapsed);
    rc=vv_sdhci_read_sector(&host,v->first+(uint64_t)first+done,buffer+done*512,512,
                            left<500000 ? left:500000,&r);
    if(!rc)done++;
  }
  nxmutex_unlock(&lock);return done ? (ssize_t)done:rc;
}
static ssize_t block_write(struct inode *inode,const unsigned char *buffer,
                            blkcnt_t first,unsigned int count) {
  (void)inode;(void)buffer;(void)first;(void)count;return -EROFS;
}
static int block_geometry(struct inode *inode,struct geometry *g) {
  struct emmc_view *v=inode->i_private;
  if(!g)return -EINVAL;
  if(nxmutex_trylock(&lock)<0)return -EBUSY;
  memset(g,0,sizeof(*g));
  g->geo_available=registered && host.ready && !host.needs_recovery;
  g->geo_writeenabled=false;g->geo_nsectors=(blkcnt_t)v->count;g->geo_sectorsize=512;
  snprintf(g->geo_model,sizeof(g->geo_model),"K7 eMMC read-only");
  nxmutex_unlock(&lock);return 0;
}
static int block_ioctl(struct inode *inode,int cmd,unsigned long arg) {
  (void)inode;(void)cmd;(void)arg;return -ENOTTY;
}
static const struct block_operations block_ops={
  .open=block_open,.close=block_close,.read=block_read,.write=block_write,
  .geometry=block_geometry,.ioctl=block_ioctl
};
static void block_path(char *path,size_t size,unsigned index) {
  if(index)snprintf(path,size,"/dev/emmc0rop%u",index);
  else snprintf(path,size,"/dev/emmc0ro");
}
static int register_views(void) {
  unsigned i;int rc;char path[32];
  if(registered)return -EBUSY;
  views[0]=(struct emmc_view){0,host.sector_count};
  for(i=0;i<table.count;i++)views[i+1]=(struct emmc_view){table.partitions[i].first,
    table.partitions[i].last-table.partitions[i].first+1};
  for(i=0;i<=table.count;i++) {
    block_path(path,sizeof(path),i);rc=register_blockdriver(path,&block_ops,0444,&views[i]);
    if(rc) {
      while(i) {block_path(path,sizeof(path),--i);unregister_blockdriver(path);}
      return rc;
    }
  }
  registered=1;
  printf("EMMC block registered=%u readonly=1 root=/dev/emmc0ro\n",table.count+1);
  return 0;
}
static int block_check(void) {
  struct inode *parent=NULL,*child=NULL;struct geometry g;
  uint8_t a[512],b[512];int rc;ssize_t n;
  rc=open_blockdriver("/dev/emmc0ro",MS_RDONLY,&parent);if(rc)return rc;
  rc=parent->u.i_bops->geometry(parent,&g);
  if(!rc && (!g.geo_available || g.geo_writeenabled || g.geo_sectorsize!=512))rc=-EIO;
  if(!rc) {n=parent->u.i_bops->read(parent,a,0,1);if(n!=1)rc=-EIO;}
  if(!rc && (a[510]!=0x55 || a[511]!=0xaa))rc=-EIO;
  if(!rc && parent->u.i_bops->write(parent,a,0,1)!=-EROFS)rc=-EIO;
  if(!rc && parent->u.i_bops->read(parent,b,g.geo_nsectors,1)!=-EINVAL)rc=-EIO;
  if(!rc)rc=open_blockdriver("/dev/emmc0rop1",MS_RDONLY,&child);
  if(!rc && parent->u.i_bops->read(parent,a,(blkcnt_t)views[1].first,1)!=1)rc=-EIO;
  if(!rc && child->u.i_bops->read(child,b,0,1)!=1)rc=-EIO;
  if(!rc && memcmp(a,b,512))rc=-EIO;
  if(!rc)for(unsigned i=0;i<table.count;i++) {
    const char *name=table.partitions[i].name;
    if(strcmp(name,"cache") && strcmp(name,"userdata"))continue;
    if(views[i+1].count<3)continue;
    if(parent->u.i_bops->read(parent,b,(blkcnt_t)(views[i+1].first+2),1)!=1) {rc=-EIO;break;}
    const char *kind=b[56]==0x53 && b[57]==0xef ? "ext-family" :
      b[0]==0x10 && b[1]==0x20 && b[2]==0xf5 && b[3]==0xf2 ? "f2fs":"unknown";
    printf("EMMC filesystem_signature partition=%s signature=%s mounted=0\n",name,kind);
  }
  if(child)close_blockdriver(child);
  close_blockdriver(parent);
  printf("EMMC blockcheck rc=%d readonly_geometry=%s rejected_write=%s partition_translation=%s storage_writes=0\n",
    rc,rc ? "FAIL":"PASS",rc ? "FAIL":"PASS",rc ? "FAIL":"PASS");return rc;
}
