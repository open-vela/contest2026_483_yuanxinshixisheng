#include "vv_rtc_reader.h"
#include "vv_rtc_time.h"
#include <errno.h>
#include <string.h>
static int gate(struct vv_rtc_reader*s,uint64_t deadline,uint64_t*last){
 if(s->ops.cancelled&&s->ops.cancelled(s->ops.arg))return -ECANCELED;
 uint64_t now=s->ops.now_ms(s->ops.arg);
 if(now==UINT64_MAX||now<*last)return -EIO;
 *last=now;return now>=deadline?-ETIMEDOUT:0;
}
int vv_rtc_reader_read(struct vv_rtc_reader*s,uint64_t deadline,uint64_t*utc){
 uint8_t snapshots[2][9]={{0}};uint64_t value=0,last=0;int rc,cleanup;
 if(utc)*utc=0;
 if(!s||!utc||!s->ops.acquire||!s->ops.read_register||!s->ops.release||!s->ops.now_ms)return -EINVAL;
 if(s->entered)return -EBUSY;
 s->entered=true;
 if((rc=gate(s,deadline,&last)))goto done;
 /* Keep both snapshots within 100 ms, never a whole seconds cycle. */
 if(last>UINT64_MAX-100){rc=-EIO;goto done;}
 if(deadline>last+100)deadline=last+100;
 rc=s->ops.acquire(s->ops.arg);if(rc){rc=rc<0?rc:-EIO;goto done;}
 for(unsigned pass=0;pass<2;pass++){
  for(uint8_t reg=0;reg<9;reg++){
   if((rc=gate(s,deadline,&last)))goto release;
   rc=s->ops.read_register(s->ops.arg,reg,&snapshots[pass][reg],deadline);
   if(rc){rc=rc<0?rc:-EIO;goto release;}
  }
 }
 if((rc=gate(s,deadline,&last)))goto release;
 if(!vv_rtc_hym8563_same_fields(snapshots[0],snapshots[1])){rc=-EAGAIN;goto release;}
 rc=vv_rtc_hym8563_decode(snapshots[0],&value);
release:
 /* Exactly one release after successful acquire, even on cancellation.
  * The backend owns safe retention/poisoning if cleanup cannot finish. */
 cleanup=s->ops.release(s->ops.arg,deadline);
 if(cleanup)rc=cleanup<0?cleanup:-EIO;
 if(!rc)rc=gate(s,deadline,&last);
 if(!rc)*utc=value;
done:
 memset(snapshots,0,sizeof(snapshots));s->entered=false;return rc;
}
int vv_rtc_reader_raw(struct vv_rtc_reader*s,uint64_t deadline,uint8_t raw[9],unsigned*count){
 uint64_t last=0;int rc,cleanup;
 if(raw)memset(raw,0,9);
 if(count)*count=0;
 if(!s||!raw||!count||!s->ops.acquire||!s->ops.read_register||!s->ops.release||!s->ops.now_ms)return -EINVAL;
 if(s->entered)return -EBUSY;
 s->entered=true;
 if((rc=gate(s,deadline,&last)))goto done;
 if(last>UINT64_MAX-100){rc=-EIO;goto done;}
 if(deadline>last+100)deadline=last+100;
 rc=s->ops.acquire(s->ops.arg);if(rc){if(rc>0)rc=-EIO;goto done;}
 for(uint8_t reg=0;reg<9;reg++){
  if((rc=gate(s,deadline,&last)))break;
  rc=s->ops.read_register(s->ops.arg,reg,&raw[reg],deadline);
  if(rc){if(rc>0)rc=-EIO;break;}*count=reg+1;
 }
 cleanup=s->ops.release(s->ops.arg,deadline);
 if(cleanup)rc=cleanup<0?cleanup:-EIO;
 if(!rc)rc=gate(s,deadline,&last);
done:s->entered=false;return rc;
}
