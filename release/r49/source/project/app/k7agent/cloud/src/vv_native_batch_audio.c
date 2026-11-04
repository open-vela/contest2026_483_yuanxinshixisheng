#include "vv_native_batch_audio.h"
#include "capture_stream.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static int owner;
struct recording {unsigned char *pcm;size_t capacity,used;int error;const struct vv_https_cancel *cancel;};
static bool cancelled(const struct vv_https_cancel *c){return c&&c->is_cancelled&&c->is_cancelled(c->arg);}
static int sample(void *arg,uint32_t left,uint32_t right){
 struct recording *r=arg;(void)right;
 if(r->error)return r->error;
 if(cancelled(r->cancel))return r->error=-ECANCELED;
 if(r->capacity-r->used<2)return r->error=-EOVERFLOW;
 r->pcm[r->used++]=(unsigned char)(left>>16);r->pcm[r->used++]=(unsigned char)(left>>24);return 0;
}
/* Reject malformed request configuration before microphone activation. */
static bool endpoint_ok(const struct vv_asr_batch_request *q){
 if(!q->host||!*q->host||!q->path||*q->path!='/'||!q->boundary_hex)return false;
 size_t n=0;
 for(const unsigned char *p=(const unsigned char *)q->host;*p;p++,n++)
  if(n>=128||!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='-'))return false;
 n=0;
 for(const unsigned char *p=(const unsigned char *)q->path;*p;p++,n++)
  if(n>=255||*p<=32||*p>=127||*p=='?'||*p=='#')return false;
 for(size_t i=0;i<32;i++){unsigned char c=(unsigned char)q->boundary_hex[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
 return q->boundary_hex[32]==0;
}
int vv_native_batch_audio_perform(struct vv_https_client *client,const struct vv_native_batch_audio_request *q,const struct vv_https_cancel *cancel,struct vv_asr_batch_result *out){
 if(out)memset(out,0,sizeof(*out));
 if(!client||!q||!out||!q->ready||!endpoint_ok(&q->endpoint)||q->samples<320||q->samples>48000||q->samples%320)return -EINVAL;
 int expected=0;if(!__atomic_compare_exchange_n(&owner,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -EBUSY;
 int rc=0;struct recording r={0};r.cancel=cancel;r.capacity=(size_t)q->samples*2;
 if(cancelled(cancel)){rc=-ECANCELED;goto done;}
 if(!q->ready(q->ready_arg)){rc=-ENETDOWN;goto done;}
 r.pcm=malloc(r.capacity);if(!r.pcm){rc=-ENOMEM;goto done;}
 /* No socket access from sample callback; synchronous capture releases codec
  * ownership/SAI resources before returning, including callback failure. */
 rc=k7sound_capture_stream_cued(q->samples,sample,&r);
 if(r.error)rc=r.error;
 if(rc)goto done;
 if(cancelled(cancel)){rc=-ECANCELED;goto done;}
 if(r.used!=r.capacity){rc=-EIO;goto done;}
 if(!q->ready(q->ready_arg)){rc=-ENETDOWN;goto done;}
 struct vv_asr_batch_request request=q->endpoint;request.pcm=r.pcm;request.pcm_bytes=r.used;
 request.sample_rate=16000;request.channels=1;request.bits_per_sample=16;
 rc=vv_asr_batch_perform(client,&request,cancel,out);
 done:
 if(r.pcm){volatile unsigned char *p=r.pcm;for(size_t i=0;i<r.capacity;i++)p[i]=0;free(r.pcm);}
 if(rc)memset(out,0,sizeof(*out));
 __atomic_store_n(&owner,0,__ATOMIC_RELEASE);return rc;
}
