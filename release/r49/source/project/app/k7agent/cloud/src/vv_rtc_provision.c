#include "vv_rtc_provision.h"
#include "vv_rtc_time.h"
#include <errno.h>
#include <string.h>

static unsigned leap(unsigned y){return y%4==0&&(y%100!=0||y%400==0);}
static uint8_t bcd(unsigned v){return (uint8_t)((v/10)*16+v%10);}
int vv_rtc_hym8563_encode(uint64_t epoch,uint8_t out[7]){
 static const unsigned lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
 if(!out)return -EINVAL;
 memset(out,0,7);
 if(epoch<VV_RTC_PROVISION_MIN_EPOCH||epoch>VV_RTC_PROVISION_MAX_EPOCH)return -ERANGE;
 uint64_t days=(epoch-VV_RTC_PROVISION_MIN_EPOCH)/86400;
 unsigned y=2000,m=1;
 while(days>=365+leap(y)){days-=365+leap(y);y++;}
 while(days>=lengths[m-1]+(m==2&&leap(y))){days-=lengths[m-1]+(m==2&&leap(y));m++;}
 out[0]=bcd((unsigned)(epoch%60));out[1]=bcd((unsigned)((epoch/60)%60));
 out[2]=bcd((unsigned)((epoch/3600)%24));out[3]=bcd((unsigned)days+1);
 out[4]=(uint8_t)((epoch/86400+4)%7);out[5]=bcd(m);out[6]=bcd(y-2000);
 return 0;
}
static int norm(int rc){return rc>0?-EIO:rc;}
int vv_rtc_provision_parse_epoch(const char*text,uint64_t*out){
 uint64_t value=0;unsigned count=0;
 if(out)*out=0;
 if(!text||!out||!*text)return -EINVAL;
 for(;*text;text++){
  if(*text<'0'||*text>'9'||++count>10)return -EINVAL;
  value=value*10+(unsigned)(*text-'0');
 }
 if(value<VV_RTC_PROVISION_MIN_EPOCH||value>VV_RTC_PROVISION_MAX_EPOCH)return -ERANGE;
 *out=value;return 0;
}
int vv_rtc_provision_cli(struct vv_rtc_provision*s,bool authorized,
                         const char*text,uint64_t deadline,
                         struct vv_rtc_provision_result*r){
 uint64_t epoch=0;if(r)memset(r,0,sizeof(*r));
 int rc=vv_rtc_provision_parse_epoch(text,&epoch);if(rc)return rc;
 return vv_rtc_provision_once(s,authorized,epoch,deadline,r);
}
static int gate(struct vv_rtc_provision*s,uint64_t d,uint64_t*last,bool cleanup){
 struct vv_rtc_reader_ops*o=&s->ops.reader;
 if(!cleanup&&o->cancelled&&o->cancelled(o->arg))return -ECANCELED;
 uint64_t n=o->now_ms(o->arg);
 if(n==UINT64_MAX||n<*last)return -EIO;
 *last=n;return n>=d?-ETIMEDOUT:0;
}
static int provision(struct vv_rtc_provision*s,bool authorized,bool initialize,
                          uint64_t epoch,uint64_t deadline,
                          struct vv_rtc_provision_result*r){
 uint8_t encoded[7],snap[2][9]={{0}},expected[9]={0},ctl=0,sec=0,final_ctl=0,restore_ctl=0;
 uint64_t last=0,observed=0;int rc,cleanup;
 if(r)memset(r,0,sizeof(*r));
 if(!s||!r||!s->ops.reader.acquire||!s->ops.reader.release||
    !s->ops.reader.read_register||!s->ops.reader.now_ms||!s->ops.write_calendar||
    !s->ops.write_control1||!s->ops.poison)return -EINVAL;
 r->initialize_requested=initialize;
 if(!authorized)return -EACCES;
 if(s->attempted)return -EALREADY;
 rc=vv_rtc_hym8563_encode(epoch,encoded);if(rc)return rc;
 s->attempted=true;
 if((rc=gate(s,deadline,&last,false)))return rc;
 if(last>UINT64_MAX-100)return -EIO;
 if(deadline>last+100)deadline=last+100;
 struct vv_rtc_reader_ops*o=&s->ops.reader;
 rc=norm(o->acquire(o->arg));if(rc)return rc;
 if((rc=gate(s,deadline,&last,false)))goto release;
 rc=norm(o->read_register(o->arg,0,&ctl,deadline));if(rc)goto release;
 r->initial_control1_valid=true;r->initial_control1=ctl;
 if((rc=gate(s,deadline,&last,false)))goto release;
 rc=norm(o->read_register(o->arg,2,&sec,deadline));if(rc)goto release;
 r->initial_seconds_valid=true;r->initial_seconds=sec;
 if((rc=gate(s,deadline,&last,false)))goto release;
 r->initial_vl=(sec&0x80)!=0;
 /* Old calendar bytes are deliberately not decoded: this is the explicit
  * repair entry. TESTC is set by POR, not bus ownership. Explicit init clears
  * it as Linux hym8563_init_device does; TEST1 external-clock mode is excluded.
  * All reserved CTL1 bits must be written zero (PCF8563 Table 5). */
 r->control_blocked=ctl&(initialize?0x80:0xa8);
 if(r->control_blocked){rc=-EBUSY;goto release;}
 restore_ctl=initialize?(uint8_t)(ctl&0x20):ctl;
 r->stop_attempted=true;
 rc=norm(s->ops.write_control1(o->arg,initialize?0x20:(uint8_t)(ctl|0x20),deadline));
 if(rc)goto restore;
 r->stop_acked=true;
 if((rc=gate(s,deadline,&last,false)))goto restore;
 r->write_attempted=true;
 rc=norm(s->ops.write_calendar(o->arg,encoded,deadline));
 if(!rc)r->write_acked=true;
 if(!rc)rc=gate(s,deadline,&last,false);
restore:
 r->operation_rc=rc;
 /* Commit starts an explicitly initialized clock; failed programming instead
  * returns CTL1 to its original state, including an original STOP bit. */
 final_ctl=(!rc&&initialize)?0:restore_ctl;
 /* Restoring CTL1 is cleanup, not retrying a failed provisioning operation.
  * An ACK failure may still have applied STOP, so attempt restoration once.
  * A failed/expired restoration latches poison before release. */
 r->restore_rc=gate(s,deadline,&last,true);
 if(!r->restore_rc){r->restore_attempted=true;r->restore_rc=norm(s->ops.write_control1(o->arg,final_ctl,deadline));}
 if(!r->restore_rc)r->restore_rc=gate(s,deadline,&last,true);
 if(r->restore_rc){
  /* Failed STOP-clear may have partially applied. One rollback to the
   * different original value is cleanup, not a retry of the failed start. */
  if(final_ctl!=restore_ctl&&r->restore_attempted){
   r->rollback_rc=gate(s,deadline,&last,true);
   if(!r->rollback_rc){r->rollback_attempted=true;r->rollback_rc=norm(s->ops.write_control1(o->arg,restore_ctl,deadline));}
   if(!r->rollback_rc)r->rollback_rc=gate(s,deadline,&last,true);
   if(!r->rollback_rc)r->restored=true;
  }
  if(!r->restored)s->ops.poison(o->arg);
  rc=r->restore_rc;goto release;
 }
 r->restored=true;
 if(rc)goto release;
 for(unsigned pass=0;pass<2;pass++)for(uint8_t reg=0;reg<9;reg++){
  if((rc=gate(s,deadline,&last,false)))goto release;
  rc=norm(o->read_register(o->arg,reg,&snap[pass][reg],deadline));if(rc)goto release;
  r->readback[pass][reg]=snap[pass][reg];r->readback_count[pass]=reg+1;
 }
 if((rc=gate(s,deadline,&last,false)))goto release;
 if(!vv_rtc_hym8563_same_fields(snap[0],snap[1])){rc=-EAGAIN;goto release;}
 if((snap[0][0]&0xa8)!=(final_ctl&0xa8)){if(final_ctl==restore_ctl)s->ops.poison(o->arg);rc=-EIO;goto release;}
 rc=vv_rtc_hym8563_decode(snap[0],&observed);if(rc)goto release;
 if(observed!=epoch&&observed!=epoch+1){rc=-EIO;goto release;}
 /* Decoder checks weekday range, not correspondence to date. Verify every
  * calendar byte too, including weekday and cleared VL/century bits. */
 rc=vv_rtc_hym8563_encode(observed,expected+2);if(rc)goto release;
 if(!vv_rtc_hym8563_same_fields(snap[0],expected)){rc=-EIO;goto release;}
release:
 if(!r->operation_rc)r->operation_rc=rc;
 /* If a newly started clock fails verification, also restore its original
  * stopped control state. This never rewrites the calendar or retries init. */
 if(rc&&final_ctl!=restore_ctl&&r->restored&&!r->rollback_attempted){
  r->rollback_rc=gate(s,deadline,&last,true);
  if(!r->rollback_rc){r->rollback_attempted=true;r->rollback_rc=norm(s->ops.write_control1(o->arg,restore_ctl,deadline));}
  if(!r->rollback_rc)r->rollback_rc=gate(s,deadline,&last,true);
  r->restored=!r->rollback_rc;
  if(r->rollback_rc){s->ops.poison(o->arg);rc=r->rollback_rc;}
 }
 cleanup=norm(o->release(o->arg,deadline));r->cleanup_rc=cleanup;
 if(cleanup)rc=cleanup;
 if(!rc)rc=gate(s,deadline,&last,false);
 if(!rc){r->readback_valid=true;r->running_verified=true;r->observed_epoch=observed;}
 return rc;
}
int vv_rtc_provision_once(struct vv_rtc_provision*s,bool authorized,
                          uint64_t epoch,uint64_t deadline,
                          struct vv_rtc_provision_result*r){
 return provision(s,authorized,false,epoch,deadline,r);
}
int vv_rtc_provision_initialize_cli(struct vv_rtc_provision*s,bool authorized,
                         const char*text,uint64_t deadline,
                         struct vv_rtc_provision_result*r){
 uint64_t epoch=0;if(r)memset(r,0,sizeof(*r));
 int rc=vv_rtc_provision_parse_epoch(text,&epoch);if(rc)return rc;
 return provision(s,authorized,true,epoch,deadline,r);
}
