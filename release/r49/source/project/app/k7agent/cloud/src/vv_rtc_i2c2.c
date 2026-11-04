#include "vv_rtc_i2c2.h"
#include <errno.h>
#include <string.h>
#define BASE ((uintptr_t)0x2ac50000)
#define CRU ((uintptr_t)0x27200000)
#define IOC ((uintptr_t)0x26040000)
#define CON 0x363u
static uint32_t rd(struct vv_rtc_i2c2*s,uintptr_t a){return s->io.read32(s->io.arg,a);}
static void wr(struct vv_rtc_i2c2*s,uintptr_t a,uint32_t v){s->io.write32(s->io.arg,a,v);}
static uint64_t clock_ms(void*p){struct vv_rtc_i2c2*s=p;return s->io.now_ms(s->io.arg);}
static bool cancelled(void*p){struct vv_rtc_i2c2*s=p;return s->io.cancelled&&s->io.cancelled(s->io.arg);}
static int gate(struct vv_rtc_i2c2*s,uint64_t d,bool cleanup){
 uint64_t n=clock_ms(s);if(n==UINT64_MAX||n<s->last)return -EIO;s->last=n;
 if(n>=d)return -ETIMEDOUT;
 if(!cleanup&&cancelled(s))return -ECANCELED;
 return 0;
}
static int waitbit(struct vv_rtc_i2c2*s,uint32_t bit,uint64_t d,bool cleanup){
 /* Cap iterations too: a stalled monotonic callback cannot hang the caller. */
 for(unsigned i=0;i<10000;i++){int rc=gate(s,d,cleanup);if(rc)return rc;
 uint32_t v=rd(s,BASE+0x1c);if(!cleanup&&(v&64))return -ENXIO;
 if(v&bit)return 0;
 s->io.wait_us(s->io.arg,10);}
 return -ETIMEDOUT;
}
static int acquire(void*p){struct vv_rtc_i2c2*s=p;
 if(!s->approved)return -EACCES;
 if(__sync_lock_test_and_set(&s->owner,1))return -EBUSY;
 if(s->poisoned)return -EIO;
 s->last=clock_ms(s);
 if(s->last==UINT64_MAX){__sync_lock_release(&s->owner);return -EIO;}
 /* Read only CRU/IOC first. Never access a gated/reset controller. */
 if((rd(s,CRU+0x830)&0x2002)||(rd(s,CRU+0xa30)&0x2002)||
 (rd(s,CRU+0x3e4)&12)!=12||(rd(s,IOC+0x2000)&0xf000)!=0x9000||
 (rd(s,IOC+0x2004)&15)!=9){__sync_lock_release(&s->owner);return -ENODEV;}
 if((rd(s,BASE)&VV_RTC_I2C2_CON_BUSY_MASK)||rd(s,BASE+0x18)||rd(s,BASE+4)!=0x000e000e){__sync_lock_release(&s->owner);return -EBUSY;}
 return 0;
}
static int stop(struct vv_rtc_i2c2*s,uint64_t d){
 int rc=gate(s,d,true);if(rc){s->poisoned=true;return rc;}
 wr(s,BASE+0x1c,0x1ff);wr(s,BASE,CON|16);
 rc=waitbit(s,32,d,true);
 if(rc){s->poisoned=true;return rc;}
 wr(s,BASE,0);wr(s,BASE+0x1c,0x1ff);s->active=false;return 0;
}
static int readreg(void*p,uint8_t reg,uint8_t*out,uint64_t d){struct vv_rtc_i2c2*s=p;int rc,clean;uint8_t v=0;
 if(!out||reg>8)return -EINVAL;
 *out=0;
 if(!s->owner||s->poisoned||s->active)return -EBUSY;
 rc=gate(s,d,false);if(rc)return rc;
 wr(s,BASE+0x1c,0x1ff);wr(s,BASE+8,0x010000a2);wr(s,BASE+12,0x01000000|reg);
 s->active=true;wr(s,BASE,CON|8);rc=waitbit(s,16,d,false);
 if(!rc){wr(s,BASE+0x1c,16);wr(s,BASE,CON);wr(s,BASE+0x14,1);rc=waitbit(s,8,d,false);if(!rc)v=(uint8_t)rd(s,BASE+0x200);}
 clean=stop(s,d);if(clean)return clean;if(!rc)*out=v;return rc;
}
static int release(void*p,uint64_t d){struct vv_rtc_i2c2*s=p;
 if(!s->owner)return -EPERM;
 if(s->poisoned)return -EIO;
 if(s->active){int rc=stop(s,d);if(rc)return rc;}
 __sync_lock_release(&s->owner);return 0;
}
int vv_rtc_i2c2_bind(struct vv_rtc_i2c2*s,struct vv_rtc_reader_ops*out){
 if(!s||!out||!s->io.read32||!s->io.write32||!s->io.now_ms||!s->io.wait_us)return -EINVAL;
 *out=(struct vv_rtc_reader_ops){acquire,readreg,release,clock_ms,cancelled,s};return 0;
}
