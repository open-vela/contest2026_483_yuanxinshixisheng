#include "k7boot_business.h"
#include <errno.h>
#include <string.h>
static bool identifier(const char *p){
 if(!p||!*p)return false;
 size_t n=0;for(;*p;p++,n++)if(n>=128||!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("_.:-",*p)))return false;
 return true;
}
static unsigned missing(void *a){struct k7boot_business_callbacks*s=a;return s->base.missing(s->base.arg);}
static int claim(void*a){struct k7boot_business_callbacks*s=a;return s->base.claim(s->base.arg);}
static void release(void*a){struct k7boot_business_callbacks*s=a;s->base.release(s->base.arg);}
static void sleep_ms(void*a,unsigned n){struct k7boot_business_callbacks*s=a;s->base.sleep_ms(s->base.arg,n);}
static void clear(struct k7boot_business_callbacks*s){memset(s->session,0,sizeof(s->session));memset(s->boundary,0,sizeof(s->boundary));memset(s->key,0,sizeof(s->key));s->active=false;}
static int request(void*a,struct vv_assessment_request*q){
 struct k7boot_business_callbacks*s=a;if(!q)return -EINVAL;if(s->active)return -EBUSY;
 unsigned char bytes[48]={0};int rc=s->config.random_bytes(s->config.random_arg,bytes,sizeof(bytes));
 if(rc){memset(bytes,0,sizeof(bytes));return rc<0?rc:-EIO;}
 const char hex[]="0123456789abcdef";char *out[]={s->session,s->boundary,s->key};
 for(unsigned j=0;j<3;j++){for(unsigned i=0;i<16;i++){unsigned b=bytes[j*16+i];out[j][2*i]=hex[b>>4];out[j][2*i+1]=hex[b&15];}out[j][32]=0;}
 memset(bytes,0,sizeof(bytes));
 if(!strcmp(s->session,s->boundary)||!strcmp(s->boundary,s->key)||!strcmp(s->session,s->key)){clear(s);return -EIO;}
 *q=(struct vv_assessment_request){.capture_session=s->session,.consent_ref=s->config.consent_ref,.boundary_hex=s->boundary,.idempotency_key=s->key,.poll_interval_ms=s->config.poll_interval_ms,.max_polls=s->config.max_polls,.total_timeout_ms=s->config.total_timeout_ms};s->active=true;return 0;
}
static void release_request(void*a,struct vv_assessment_request*q){struct k7boot_business_callbacks*s=a;if(q)memset(q,0,sizeof(*q));clear(s);}
int k7boot_business_apply(struct vv_native_platform_config*c,struct k7boot_business_callbacks*s,const struct k7boot_business_config*b){
 if(!c||!s||!b||!b->load||!b->random_bytes||!identifier(b->consent_ref)||!c->missing||!c->claim||!c->release||!c->sleep_ms||!b->poll_interval_ms||b->poll_interval_ms>60000||!b->max_polls||b->max_polls>1000||!b->total_timeout_ms||b->total_timeout_ms>3600000)return -EINVAL;
 memset(s,0,sizeof(*s));s->base=*c;s->config=*b;
 c->missing=missing;c->claim=claim;c->release=release;c->sleep_ms=sleep_ms;c->make_request=request;c->release_request=release_request;c->arg=s;return 0;
}
