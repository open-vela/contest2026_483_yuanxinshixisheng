#include "gpt_readonly.h"
#include <errno.h>
#include <string.h>
static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32; }
static uint32_t crc(uint32_t c,const uint8_t *p,unsigned n) {
  unsigned j;while(n--) {c^=*p++;for(j=0;j<8;j++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return c;
}
static int zero(const uint8_t *p,unsigned n) { while(n--)if(*p++)return 0;return 1; }
struct header { uint64_t first,last,entries;uint32_t count,crc;uint8_t guid[16]; };
static int header(uint8_t b[512],uint64_t sectors,int backup,struct header *h) {
  uint32_t size=u32(b+12),sum=u32(b+16);uint64_t blocks;
  if(memcmp(b,"EFI PART",8)||u32(b+8)!=0x10000||size<92||size>512||u32(b+20))return -EINVAL;
  memset(b+16,0,4);if(~crc(~0u,b,size)!=sum)return -EIO;
  if(u64(b+24)!=(backup ? sectors-1:1)||u64(b+32)!=(backup ? 1:sectors-1))return -EINVAL;
  h->first=u64(b+40);h->last=u64(b+48);h->entries=u64(b+72);
  h->count=u32(b+80);h->crc=u32(b+88);memcpy(h->guid,b+56,16);
  if(u32(b+84)!=128||!h->count||h->count>VV_GPT_MAX_PARTITIONS)return -ENOTSUP;
  blocks=((uint64_t)h->count*128+511)/512;
  if(h->first<3||h->first>h->last||h->last>=sectors-1||
     h->entries<2||h->entries>=sectors-1||blocks>sectors-1-h->entries)return -EINVAL;
  if(backup ? h->entries<=h->last : h->entries+blocks>h->first)return -EINVAL;
  if(!backup && h->last>=sectors-1-blocks)return -EINVAL;
  return 0;
}
static int entries(void *ctx,int (*read)(void *,uint64_t,uint8_t *),
                    const struct header *h,struct vv_gpt *out,int collect) {
  uint8_t b[512];uint32_t sum=~0u;unsigned offset,i,j,left=h->count*128,n;int rc;
  for(offset=0;left;offset++) {
    rc=read(ctx,h->entries+offset,b);if(rc)return rc;
    n=left<512 ? left:512;sum=crc(sum,b,n);
    if(collect)for(i=0;i<n;i+=128) {
      const uint8_t *p=b+i;struct vv_gpt_partition *part;
      if(zero(p,16))continue;
      if(zero(p+16,16)||u64(p+32)<h->first||u64(p+32)>u64(p+40)||u64(p+40)>h->last)return -EINVAL;
      part=&out->partitions[out->count];
      part->first=u64(p+32);part->last=u64(p+40);part->attributes=u64(p+48);
      memcpy(part->guid,p+16,16);
      for(j=0;j<out->count;j++) {
        const struct vv_gpt_partition *old=&out->partitions[j];
        if(!(part->last<old->first||part->first>old->last)||!memcmp(part->guid,old->guid,16))return -EINVAL;
      }
      /* Diagnostics only: printable ASCII names; never decode executable data. */
      for(j=0;j<36;j++) {
        unsigned ch=p[56+2*j]|(unsigned)p[57+2*j]<<8;
        if(!ch)break;
        part->name[j]=(ch>=32&&ch<127) ? (char)ch:'?';
      }
      part->name[j]=0;out->count++;
    }
    left-=n;
  }
  return ~sum==h->crc ? 0:-EIO;
}
int vv_gpt_read(void *ctx,int (*read)(void *,uint64_t,uint8_t *),
                uint64_t sectors,struct vv_gpt *out) {
  uint8_t b[512];struct header primary,backup;unsigned i;int rc,protective=0;
  if(!read||!out||sectors<6)return -EINVAL;
  memset(out,0,sizeof(*out));
  rc=read(ctx,0,b);if(rc)return rc;
  if(b[510]!=0x55||b[511]!=0xaa)return -EINVAL;
  for(i=0;i<4;i++)if(b[446+i*16+4]==0xee && u32(b+446+i*16+8)==1)protective=1;
  if(!protective)return -EINVAL;
  rc=read(ctx,1,b);if(rc)return rc;
  rc=header(b,sectors,0,&primary);if(rc)return rc;
  rc=read(ctx,sectors-1,b);if(rc)return rc;
  rc=header(b,sectors,1,&backup);if(rc)return rc;
  if(primary.first!=backup.first||primary.last!=backup.last||primary.count!=backup.count||
     primary.crc!=backup.crc||memcmp(primary.guid,backup.guid,16))return -EINVAL;
  out->first_usable=primary.first;out->last_usable=primary.last;out->entries_crc=primary.crc;
  rc=entries(ctx,read,&primary,out,1);if(rc)return rc;
  return entries(ctx,read,&backup,out,0);
}
