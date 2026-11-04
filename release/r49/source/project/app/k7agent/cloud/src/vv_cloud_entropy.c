#include "vv_cloud_entropy.h"
#include <errno.h>
#include <string.h>
static void wipe(void *p,size_t n){volatile uint8_t*b=p;while(n--)*b++=0;}
void vv_cloud_entropy_disable(struct vv_cloud_entropy *s){if(s){s->enabled=false;s->failed=true;}}
int vv_cloud_entropy_configure(struct vv_cloud_entropy *s,const struct vv_cloud_entropy_config *cfg){
 if(!s||s->entered)return -EINVAL;
 vv_cloud_entropy_disable(s);
 if(!cfg||!cfg->read_exact||!cfg->approved_and_healthy||!cfg->now_ms||
    !cfg->max_wait_ms||cfg->max_wait_ms>1000)return -EINVAL;
 if(!cfg->approved_and_healthy(cfg->arg))return -EACCES;
 uint64_t now=cfg->now_ms(cfg->arg);
 if(now==UINT64_MAX)return -EIO;
 s->cfg=*cfg;s->last_ms=now;s->failed=false;s->enabled=true;return 0;
}
int vv_cloud_entropy_read(void *arg,unsigned char *out,size_t n){
 struct vv_cloud_entropy*s=arg;int rc=-EIO;
 if(!out||!n)return -EINVAL;
 /* Caller owns n writable bytes, even for a rejected oversized request. */
 if(s&&s->entered){wipe(out,n);vv_cloud_entropy_disable(s);return -EBUSY;}
 if(!s||!s->enabled||s->failed||n>VV_CLOUD_ENTROPY_MAX_REQUEST){
   wipe(out,n);return -EACCES;}
 s->entered=true;
 if(!s->cfg.approved_and_healthy(s->cfg.arg))goto fail;
 uint64_t before=s->cfg.now_ms(s->cfg.arg);
 if(before==UINT64_MAX||before<s->last_ms||before>UINT64_MAX-s->cfg.max_wait_ms)goto fail;
 uint64_t deadline=before+s->cfg.max_wait_ms;
 ssize_t count=s->cfg.read_exact(s->cfg.arg,out,n,deadline);
 uint64_t after=s->cfg.now_ms(s->cfg.arg);
 if(count!=(ssize_t)n||after==UINT64_MAX||after<before||after>=deadline||
    !s->cfg.approved_and_healthy(s->cfg.arg)||!s->enabled)goto fail;
 s->last_ms=after;s->entered=false;return 0;
fail:
 wipe(out,n);s->entered=false;vv_cloud_entropy_disable(s);return rc;
}
