#include "vv_rtc_provision_i2c2.h"
#include "vv_rtc_time.h"
#include <errno.h>
#include <string.h>
#define BASE ((uintptr_t)0x2ac50000)
/* RK3576 I2C TX mode=0, EN, ACTACK; established SDA timing 0x300.
 * Pinned Linux i2c-rk3x register facts: TX FIFO 0x100, MTXCNT 0x10,
 * IPD START=0x10, MBTF=0x04, STOP=0x20, NAK=0x40. */
#define CON 0x341u
static uint32_t rd(struct vv_rtc_i2c2*s,uintptr_t off){return s->io.read32(s->io.arg,BASE+off);}
static void wr(struct vv_rtc_i2c2*s,uintptr_t off,uint32_t v){s->io.write32(s->io.arg,BASE+off,v);}
static int gate(struct vv_rtc_i2c2*s,uint64_t d){
 uint64_t n=s->io.now_ms(s->io.arg);
 if(n==UINT64_MAX||n<s->last)return -EIO;
 s->last=n;return n>=d?-ETIMEDOUT:0;
}
static int waitbit(struct vv_rtc_i2c2*s,uint32_t bit,uint64_t d,bool cleanup){
 for(unsigned i=0;i<10000;i++){
  int rc=gate(s,d);if(rc)return rc;
  uint32_t ipd=rd(s,0x1c);
  if(!cleanup&&(ipd&0x40))return -ENXIO;
  if(ipd&bit)return 0;
  s->io.wait_us(s->io.arg,10);
 }
 return -ETIMEDOUT;
}
static int stop(struct vv_rtc_i2c2*s,uint64_t d){
 int rc=gate(s,d);
 if(rc){s->poisoned=true;return rc;}
 wr(s,0x1c,0x1ff);wr(s,0,CON|0x10);
 rc=waitbit(s,0x20,d,true);
 if(rc){s->poisoned=true;return rc;}
 wr(s,0,0);wr(s,0x1c,0x1ff);s->active=false;return 0;
}
/* Internal only: fixed public callbacks below are the sole permitted spans. */
static int send(struct vv_rtc_i2c2*s,uint8_t reg,const uint8_t*data,unsigned n,uint64_t d){
 if(!s->approved||!s->owner)return -EACCES;
 if(s->poisoned||s->active)return -EBUSY;
 int rc=gate(s,d);if(rc)return rc;
 uint8_t bytes[9]={0xa2,reg};memcpy(bytes+2,data,n);
 wr(s,0x1c,0x1ff);
 for(unsigned i=0;i<n+2;i+=4){
  uint32_t word=0;
  for(unsigned j=0;j<4&&i+j<n+2;j++)word|=(uint32_t)bytes[i+j]<<(8*j);
  wr(s,0x100+i,word);
 }
 s->active=true;wr(s,0,CON|8);
 rc=waitbit(s,0x10,d,false);
 if(!rc){wr(s,0x1c,0x10);wr(s,0,CON);wr(s,0x10,n+2);rc=waitbit(s,4,d,false);}
 int clean=stop(s,d);return clean?clean:rc;
}
static int calendar(void*p,const uint8_t data[7],uint64_t d){
 struct vv_rtc_i2c2*s=p;uint8_t r[9]={0},encoded[7];uint64_t epoch;
 if(!data)return -EINVAL;
 memcpy(r+2,data,7);int rc=vv_rtc_hym8563_decode(r,&epoch);if(rc)return rc;
 rc=vv_rtc_hym8563_encode(epoch,encoded);if(rc)return rc;
 if(memcmp(data,encoded,7))return -EINVAL;
 if(s->io.cancelled&&s->io.cancelled(s->io.arg))return -ECANCELED;
 return send(s,2,data,7,d);
}
static int control(void*p,uint8_t value,uint64_t d){
 /* CTL1 TEST/TESTC are never permitted, even by a direct callback caller. */
 if(value&0x88)return -EINVAL;
 return send(p,0,&value,1,d);
}
static void poison(void*p){struct vv_rtc_i2c2*s=p;s->poisoned=true;}
int vv_rtc_provision_i2c2_bind(struct vv_rtc_i2c2*s,struct vv_rtc_provision_ops*out){
 if(!out)return -EINVAL;
 struct vv_rtc_reader_ops reader;int rc=vv_rtc_i2c2_bind(s,&reader);if(rc)return rc;
 *out=(struct vv_rtc_provision_ops){reader,calendar,control,poison};return 0;
}
