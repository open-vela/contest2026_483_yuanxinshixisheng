#include "vv_rtc_i2c2.h"
#include <errno.h>
#include <string.h>
#define CRU ((uintptr_t)0x27200000)
#define IOC ((uintptr_t)0x26040000)
#define I2C ((uintptr_t)0x2ac50000)
static uint32_t rd(struct vv_rtc_i2c2*s,uintptr_t a){return s->io.read32(s->io.arg,a);}
static void wr(struct vv_rtc_i2c2*s,uintptr_t a,uint32_t v){s->io.write32(s->io.arg,a,v);}
static void mask(struct vv_rtc_i2c2*s,uintptr_t a,uint32_t m,uint32_t v){wr(s,a,(m<<16)|(v&m));}
static int gate(struct vv_rtc_i2c2*s,uint64_t d,uint64_t*last){uint64_t n=s->io.now_ms(s->io.arg);
 if(n==UINT64_MAX||n<*last)return -EIO;
 *last=n;
 if(n>=d)return -ETIMEDOUT;
 if(s->io.cancelled&&s->io.cancelled(s->io.arg))return -ECANCELED;
 return 0;
}
int vv_rtc_i2c2_prepare(struct vv_rtc_i2c2*s,bool approved,uint64_t deadline){
 if(!s||!s->io.read32||!s->io.write32||!s->io.now_ms||!s->io.wait_us)return -EINVAL;
 if(!approved)return -EACCES;
 if(__sync_lock_test_and_set(&s->owner,1))return -EBUSY;
 if(s->poisoned||s->active){s->poisoned=true;return -EIO;}
 int rc;bool changed=false;uint64_t last=0;s->approved=false;
 struct vv_rtc_i2c2_diag *q=&s->diag;
 memset(q,0,sizeof(*q));q->phase=VV_RTC_PHASE_PREFLIGHT;
 if((rc=gate(s,deadline,&last)))goto done;
 /* Existing shared bus root is never reparented or ungated here. */
 q->root_select=rd(s,CRU+0x3dc);q->root_gate=rd(s,CRU+0x800);
 q->scl=rd(s,IOC+0x2000);q->sda=rd(s,IOC+0x2004);
 q->bus_gate=rd(s,CRU+0x82c);q->reset=rd(s,CRU+0xa30);
 uint32_t parent=(q->root_select>>2)&3,b=q->scl&0xf000,c=q->sda&15;
 if(parent==3||(parent==0&&(q->root_gate&2))||(parent==1&&(q->root_gate&1)))q->blocked|=VV_RTC_BLOCK_ROOT;
 if(q->bus_gate&2)q->blocked|=VV_RTC_BLOCK_BUS;
 if(q->reset&0x2002)q->blocked|=VV_RTC_BLOCK_RESET;
 if(b&&b!=0x9000)q->blocked|=VV_RTC_BLOCK_SCL;
 if(c&&c!=9)q->blocked|=VV_RTC_BLOCK_SDA;
 if(q->blocked){rc=-EBUSY;goto done;}
 if((rc=gate(s,deadline,&last)))goto done;
 /* Only this controller PCLK; do not touch root/reset or RTC CLKOUT. */
 q->phase=VV_RTC_PHASE_PCLK;
 changed=true;mask(s,CRU+0x830,2,0);s->io.wait_us(s->io.arg,10);
 if((rc=gate(s,deadline,&last)))goto done;
 q->pclk=rd(s,CRU+0x830);
 if(q->pclk&2){rc=-EIO;goto done;}
 q->phase=VV_RTC_PHASE_CONTROLLER;
 q->con=rd(s,I2C);q->ien=rd(s,I2C+0x18);q->controller_valid=true;
 if(q->con&VV_RTC_I2C2_CON_BUSY_MASK)q->blocked|=VV_RTC_BLOCK_CON;
 if(q->ien)q->blocked|=VV_RTC_BLOCK_IEN;
 if(q->blocked){rc=-EBUSY;goto done;}
 q->phase=VV_RTC_PHASE_SCLK_GATE;
 mask(s,CRU+0x830,0x2000,0x2000);
 if(!(rd(s,CRU+0x830)&0x2000)){rc=-EIO;goto done;}
 if((rc=gate(s,deadline,&last)))goto done;
 q->phase=VV_RTC_PHASE_CONFIGURE;
 mask(s,CRU+0x3e4,12,12);mask(s,CRU+0x830,0x2000,0);
 mask(s,IOC+0x2000,0xf000,0x9000);mask(s,IOC+0x2004,15,9);
 wr(s,I2C+4,0x000e000e);s->io.wait_us(s->io.arg,20);
 if((rc=gate(s,deadline,&last)))goto done;
 q->phase=VV_RTC_PHASE_VERIFY;
 if((rd(s,CRU+0x830)&0x2002)||(rd(s,CRU+0x3e4)&12)!=12||
    (rd(s,IOC+0x2000)&0xf000)!=0x9000||(rd(s,IOC+0x2004)&15)!=9||
    rd(s,I2C+4)!=0xe000e){rc=-EIO;goto done;}
 s->approved=true;q->phase=VV_RTC_PHASE_READY;rc=0;
done:
 /* Partial setup is not automatically undone/retried under unknown hardware.
  * It has sent no bus START, but retains ownership for explicit recovery. */
 if(rc&&changed){s->poisoned=true;return rc;}
 __sync_lock_release(&s->owner);return rc;
}
