#include "vv_rk3576_trng.h"
#include <errno.h>
#include <string.h>
static void wipe(void *p,size_t n){volatile uint8_t*b=p;while(n--)*b++=0;}
void vv_rk3576_trng_disable(struct vv_rk3576_trng *s){
  if(s){s->enabled=false;s->failed=true;s->have_previous=false;wipe(s->previous,32);}
}
static int gate(struct vv_rk3576_trng*s,uint64_t deadline){
  if(!s->enabled||s->failed||!s->ops.authorized_and_healthy(s->ops.arg))return -EACCES;
  uint64_t now=s->ops.now_us(s->ops.arg);
  if(now==UINT64_MAX||now<s->last_us)return -EIO;
  s->last_us=now;return now>=deadline?-ETIMEDOUT:0;
}
int vv_rk3576_trng_configure(struct vv_rk3576_trng*s,const struct vv_rk3576_trng_ops*ops){
  if(!s)return -EINVAL;
  if(s->entered){vv_rk3576_trng_disable(s);return -EBUSY;}
  vv_rk3576_trng_disable(s);
  if(!ops||!ops->read32||!ops->write32||!ops->now_us||!ops->wait_us||!ops->authorized_and_healthy)return -EINVAL;
  if(!ops->authorized_and_healthy(ops->arg))return -EACCES;
  uint64_t now=ops->now_us(ops->arg);if(now==UINT64_MAX)return -EIO;
  s->ops=*ops;s->last_us=now;s->failed=false;s->enabled=true;return 0;
}
ssize_t vv_rk3576_trng_read_exact(void *arg,uint8_t*out,size_t n,uint64_t deadline_ms){
  struct vv_rk3576_trng*s=arg;uint8_t block[32]={0};int rc=-EINVAL;size_t copied=0;
  if(!out||!n)return -EINVAL;
  if(!s){wipe(out,n);return -EINVAL;}
  if(s->entered){wipe(out,n);vv_rk3576_trng_disable(s);return -EBUSY;}
  if(n>VV_RK3576_TRNG_MAX_BYTES||deadline_ms>UINT64_MAX/1000){wipe(out,n);return -EINVAL;}
  s->entered=true;uint64_t deadline=deadline_ms*1000;
  while(copied<n){
    if((rc=gate(s,deadline)))goto failed;
    uint64_t block_end=s->last_us>UINT64_MAX-10000?UINT64_MAX:s->last_us+10000;
    if(block_end>deadline)block_end=deadline;
    /* HIWORD write-mask for request bit4; do not touch DRNG bit3. */
    rc=s->ops.write32(s->ops.arg,VV_RK3576_TRNG_CTRL,
                      VV_RK3576_TRNG_READY|(VV_RK3576_TRNG_READY<<16));
    if(rc)goto failed;
    uint32_t state=0;unsigned polls=0;
    for(;;){
      if((rc=gate(s,block_end)))goto failed;
      rc=s->ops.read32(s->ops.arg,VV_RK3576_TRNG_STATE,&state);if(rc)goto failed;
      if((rc=gate(s,block_end)))goto failed;
      if(state&VV_RK3576_TRNG_READY)break;
      /* Additional cap prevents infinite spin if an injected clock stalls. */
      if(++polls>101){rc=-ETIMEDOUT;goto failed;}
      uint64_t left=block_end-s->last_us;
      rc=s->ops.wait_us(s->ops.arg,(uint32_t)(left<100?left:100));if(rc)goto failed;
    }
    rc=s->ops.write32(s->ops.arg,VV_RK3576_TRNG_STATE,VV_RK3576_TRNG_READY);if(rc)goto failed;
    for(unsigned i=0;i<8;i++){
      if((rc=gate(s,block_end)))goto failed;
      uint32_t word=0;rc=s->ops.read32(s->ops.arg,VV_RK3576_TRNG_DATA+4*i,&word);if(rc)goto failed;
      for(unsigned j=0;j<4;j++)block[4*i+j]=(uint8_t)(word>>(8*j));
    }
    if((rc=gate(s,block_end)))goto failed;
    if(s->have_previous&&!memcmp(block,s->previous,32)){rc=-EIO;goto failed;}
    memcpy(s->previous,block,32);s->have_previous=true;
    size_t take=n-copied;if(take>32)take=32;
    memcpy(out+copied,block,take);copied+=take;wipe(block,32);
  }
  if((rc=gate(s,deadline)))goto failed;
  s->entered=false;return (ssize_t)n;
failed:
  wipe(block,32);wipe(out,n);s->entered=false;vv_rk3576_trng_disable(s);
  return rc<0?rc:-EIO;
}
