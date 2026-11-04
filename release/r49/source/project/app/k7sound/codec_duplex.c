#include "codec_duplex.h"
#include <errno.h>
#include <string.h>
/* Engineering candidate: Linux probe baseline, audited field overrides.
 * No KC review-bit changes, no hardware acceptance claim. See HANDOFF. */
struct item { uint8_t reg,value,verify; unsigned delay; };
static const struct item init[] = {
 {0x00,0x80,0,0},{0x00,0x00,0,0}, /* Linux reset, do not compare reset bit */
 {0x01,0x60,0,0},{0x02,0xf3,0,0},{0x02,0xf0,0,0},
 {0x2b,0x80,0,0},{0x00,0x36,0,0},{0x08,0x00,0xe0,0},
 {0x04,0xc0,0,0}, /* hold all DAC outputs disabled while preparing */
 /* Normal-power comparison: Linux set_bias(STANDBY) register order.
  * Keep both ADCs enabled later (03=00), do not copy 03=59. */
 {0x07,0x7c,0x7f,0},{0x05,0x00,0xe8,0},{0x06,0x00,0xc3,0},
 {0x19,0x06,0x04,0},{0x0f,0x34,0x04,0},
 {0x09,0x44,0xff,0}, /* fixed +12 dB PGA, explicit ALC off below */
 {0x0a,0xf0,0xf0,0},{0x0b,0x82,0x9c,0},
 {0x0c,0x0c,0xff,0},{0x0d,0x02,0x3f,0},
 {0x10,0x00,0xff,0},{0x11,0x00,0xff,0},
 {0x12,0x00,0xc0,0},{0x16,0x00,0x01,0}, /* ALC/noise gate off */
 {0x17,0x18,0x7e,0},{0x18,0x02,0x3f,0},
 {0x1a,0x30,0xff,0},{0x1b,0x30,0xff,0}, /* DAC -24dB */
 {0x26,0x00,0,0},{0x27,0xb8,0xc0,0},{0x2a,0xb8,0xc0,20},
 /* Deliberately omit unexplained Linux 0x35 and do not enable OUT1. */
 {0x2e,0x00,0,0},{0x2f,0x00,0,0},
 {0x30,0x14,0x3f,0},{0x31,0x14,0x3f,0}, /* OUT2 -15dB */
 {0x03,0x00,0,0},{0x02,0x00,0,20},
 {0x04,0x0c,0xfc,0} /* OUT2 only, amp remains disabled; ADC/DAC remain muted */
};
static int tick(struct kd_codec *c,uint64_t d){
 uint64_t n=c->io.now(c->io.arg);
 if(n<c->last)return -EIO;
 c->last=n;return n>=d?-ETIMEDOUT:0;
}
static int begin(struct kd_codec*c,uint64_t d){
 if(!c)return -EINVAL;
 if(c->busy)return -EBUSY;
 c->busy=true;
 return tick(c,d);
}
static int finish(struct kd_codec*c,int e){
 if(e){c->error=e;c->state=KD_FAULT;c->amp_armed=false;}
 c->busy=false;return e;
}
static int after(struct kd_codec*c,int e,uint64_t d){int t=tick(c,d);return e?e:t;}
static int writev(struct kd_codec*c,uint8_t r,uint8_t v,uint8_t mask,uint64_t d){
 int e=tick(c,d);uint8_t got=0;
 if(e)return e;
 e=after(c,c->io.write(c->io.arg,0x10,r,v,d),d);if(e)return e;
 if(mask){e=after(c,c->io.read(c->io.arg,0x10,r,&got,d),d);if(e)return e;
  if((got&mask)!=(v&mask))return -EIO;}
 return 0;
}
static int delayv(struct kd_codec*c,unsigned ms,uint64_t d){
 int e=tick(c,d);uint64_t n=c->last;if(e)return e;
 if((uint64_t)ms>=d-n)return -ETIMEDOUT;
 e=after(c,c->io.delay(c->io.arg,ms,d),d);
 if(!e && c->last-n<ms)e=-EIO;
 return e;
}
int kd_bind(struct kd_codec*c,const struct kd_port*p){
 if(!c||!p||!p->now||!p->write||!p->read||!p->delay||!p->amp||!p->ready||!p->quiesce)return -EINVAL;
 memset(c,0,sizeof(*c));c->io=*p;return 0;
}
int kd_prepare_adc_variant(struct kd_codec*c,unsigned bits,enum kd_adc_variant variant,uint64_t d){
 int e;if(!c || (bits!=16 && bits!=32) ||
   (variant!=KD_ADC_BASELINE && variant!=KD_ADC_COMMON_NORMAL && variant!=KD_ADC_VMID_50K && variant!=KD_ADC_PGA24))return -EINVAL;
 if(c->busy)return -EBUSY;
 if(c->state!=KD_OFF)return -EBUSY;
 e=begin(c,d);if(e)return finish(c,e);c->state=KD_CONFIGURING;
 c->word_bits=bits;
 e=after(c,c->io.amp(c->io.arg,false,d),d);
 if(!e)e=after(c,c->io.ready(c->io.arg,bits,4096000,32000u*bits,false,false,d),d);
 for(size_t i=0;!e && i<sizeof(init)/sizeof(init[0]);i++){
  uint8_t value=init[i].value,verify=init[i].verify;
  if(variant==KD_ADC_COMMON_NORMAL && init[i].reg==0x01 && value==0x60){value=0x40;verify=0x20;}
  if(variant==KD_ADC_VMID_50K && init[i].reg==0x00 && value==0x36){value=0x35;verify=0x03;}
  if(variant==KD_ADC_PGA24 && init[i].reg==0x09){value=0x88;verify=0xff;}
  if(bits==32 && init[i].reg==0x0c)value=0x10;
  if(bits==32 && init[i].reg==0x17)value=0x20;
  e=writev(c,init[i].reg,value,verify,d);
  if(!e && init[i].delay)e=delayv(c,init[i].delay,d);
 }
 if(!e)c->state=KD_PREPARED;
 return finish(c,e);
}
int kd_prepare(struct kd_codec*c,unsigned bits,uint64_t d){return kd_prepare_adc_variant(c,bits,KD_ADC_BASELINE,d);}
int kd_set_playback_gain(struct kd_codec*c,unsigned gain_db,uint64_t d){
 int e;uint8_t value;
 if(!c || gain_db>24u)return -EINVAL;
 if(c->busy || c->state!=KD_PREPARED)return -EBUSY;
 e=begin(c,d);if(e)return finish(c,e);
 c->amp_armed=false;
 e=after(c,c->io.amp(c->io.arg,false,d),d);
 value=(uint8_t)(48u-2u*gain_db);
 if(!e)e=writev(c,0x1a,value,0xff,d);
 if(!e)e=writev(c,0x1b,value,0xff,d);
 if(e && !tick(c,d))(void)c->io.amp(c->io.arg,false,d);
 return finish(c,e);
}
int kd_set_playback_max(struct kd_codec*c,uint64_t d){
 int e;
 if(!c)return -EINVAL;
 if(c->busy || c->state!=KD_PREPARED)return -EBUSY;
 e=begin(c,d);if(e)return finish(c,e);
 c->amp_armed=false;
 e=after(c,c->io.amp(c->io.arg,false,d),d);
 if(!e)e=writev(c,0x1a,0x00,0xff,d);
 if(!e)e=writev(c,0x1b,0x00,0xff,d);
 if(!e)e=writev(c,0x30,0x21,0x3f,d);
 if(!e)e=writev(c,0x31,0x21,0x3f,d);
 if(e && !tick(c,d))(void)c->io.amp(c->io.arg,false,d);
 return finish(c,e);
}
static int activate(struct kd_codec*c,bool cap,bool play,bool immediate,uint64_t d){
 int e;if(!c||(!cap&&!play))return -EINVAL;if(c->busy)return -EBUSY;
 if(c->state!=KD_PREPARED && c->state!=KD_ACTIVE)return -EBUSY;
 e=begin(c,d);if(e)return finish(c,e);
 c->amp_armed=false;
 e=after(c,c->io.amp(c->io.arg,false,d),d);
 if(!e)e=after(c,c->io.ready(c->io.arg,c->word_bits,4096000,32000u*c->word_bits,cap,play,d),d);
 if(!e)e=writev(c,0x0f,cap?0x30:0x34,4,d);
 if(!e)e=writev(c,0x19,play?0x02:0x06,4,d);
 if(!e && play)e=delayv(c,30,d);
 if(!e && play && immediate)e=after(c,c->io.amp(c->io.arg,true,d),d);
 if(e){/* Best effort disable, original error retained; stop still mandatory. */
  if(!tick(c,d))(void)c->io.amp(c->io.arg,false,d);
 }else {c->state=KD_ACTIVE;c->amp_armed=play&&!immediate;}
 return finish(c,e);
}
int kd_activate(struct kd_codec*c,bool cap,bool play,uint64_t d){return activate(c,cap,play,true,d);}
int kd_arm(struct kd_codec*c,bool cap,bool play,uint64_t d){return activate(c,cap,play,false,d);}
int kd_enable_amp(struct kd_codec*c,uint64_t d){
 int e;if(!c)return -EINVAL;if(c->busy)return -EBUSY;
 if(c->state!=KD_ACTIVE || !c->amp_armed)return -EINVAL;
 e=begin(c,d);if(e)return finish(c,e);
 c->amp_armed=false;
 e=after(c,c->io.amp(c->io.arg,true,d),d);
 if(e && !tick(c,d))(void)c->io.amp(c->io.arg,false,d);
 return finish(c,e);
}
int kd_mute(struct kd_codec*c,uint64_t d){
 int e;if(!c)return -EINVAL;if(c->busy)return -EBUSY;
 if(c->state!=KD_ACTIVE && c->state!=KD_PREPARED)return -EBUSY;
 c->amp_armed=false;
 e=begin(c,d);if(e)return finish(c,e);
 e=after(c,c->io.amp(c->io.arg,false,d),d);
 if(!e)e=writev(c,0x19,0x06,4,d);
 if(!e)e=writev(c,0x0f,0x34,4,d);
 /* Does not quiesce or release stream resources. */
 return finish(c,e);
}
int kd_stop(struct kd_codec*c,uint64_t d){
 int e,first=0;if(!c)return -EINVAL;if(c->busy)return -EBUSY;
 if(c->state==KD_OFF)return 0;
 c->amp_armed=false;
 e=begin(c,d);if(e)return finish(c,e);
 e=after(c,c->io.amp(c->io.arg,false,d),d);if(e)first=e;
 /* Even after an I2C mute failure, attempt bounded stream quiescence. */
 e=writev(c,0x19,0x06,4,d);if(e&&!first)first=e;
 e=writev(c,0x0f,0x34,4,d);if(e&&!first)first=e;
 e=tick(c,d);if(!e)e=after(c,c->io.quiesce(c->io.arg,d),d);
 if(e)return finish(c,first?first:e);
 e=writev(c,0x02,0xf3,0,d);if(e&&!first)first=e;
 e=writev(c,0x03,0xfc,0,d);if(e&&!first)first=e;
 e=writev(c,0x04,0xc0,0,d);if(e&&!first)first=e;
 if(!first)first=delayv(c,40,d);
 if(!first){c->state=KD_OFF;c->error=0;}
 return finish(c,first);
}
