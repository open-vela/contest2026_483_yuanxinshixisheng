#include "vv_roughtime.h"
#include <errno.h>
#include <string.h>
#ifndef VV_ROUGHTIME_TEST_CRYPTO
#include <sodium.h>
#endif
/* Official Cloudflare legacy key, base64 0GD7c3yP8xEc4Zl2zeuN2SlLvDVVocjsPSL8/Rl/7zg=.
 * Protocol: roughtime.googlesource.com/roughtime/+/HEAD/PROTOCOL.md.
 * SHA-512/Ed25519 are library operations, never local cryptographic primitives. */
static const uint8_t root_key[32]={0xd0,0x60,0xfb,0x73,0x7c,0x8f,0xf3,0x11,0x1c,0xe1,0x99,0x76,0xcd,0xeb,0x8d,0xd9,0x29,0x4b,0xbc,0x35,0x55,0xa1,0xc8,0xec,0x3d,0x22,0xfc,0xfd,0x19,0x7f,0xef,0x38};
struct field {const uint8_t *p;size_t n;};
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const uint8_t *p){return (uint64_t)u32(p)|((uint64_t)u32(p+4)<<32);}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
/* Validate EVERY tag/offset, including fields not requested. Ascending tags
 * reject duplicates; offsets are relative to the message data region. */
static int field(const uint8_t *p,size_t n,const char tag[4],struct field *out){
 if(!p||!out||n<8||n%4||n>VV_ROUGHTIME_RESPONSE_MAX)return -EPROTO;
 uint32_t count=u32(p);if(!count||count>64||8u*count>n)return -EPROTO;
 size_t base=8u*count,start=0;uint32_t previous=0,wanted=u32((const uint8_t *)tag);int found=0;
 for(uint32_t i=0;i<count;i++){
  uint32_t t=u32(p+4u*count+4u*i);size_t end=i+1<count?u32(p+4u+4u*i):n-base;
  if((i&&t<=previous)||end<start||end>n-base||end%4)return -EPROTO;
  if(t==wanted){out->p=p+base+start;out->n=end-start;found=1;}
  previous=t;start=end;
 }
 return found?0:-EPROTO;
}
static int get(const struct field *m,const char tag[4],size_t length,struct field *out){
 int rc=field(m->p,m->n,tag,out);if(rc)return rc;
 return length!=SIZE_MAX&&out->n!=length?-EPROTO:0;
}
int vv_roughtime_request(const uint8_t nonce[64],uint8_t request[1024]){
 if(!nonce||!request)return -EINVAL;
 memset(request,0,1024);put32(request,2);put32(request+4,64);
 memcpy(request+8,"NONCPAD\xff",8);memcpy(request+16,nonce,64);return 0;
}
static int signature(const struct vv_roughtime_crypto *c,const struct field *sig,
 const struct field *payload,const uint8_t key[32],const char *context,size_t context_n){
 uint8_t message[VV_ROUGHTIME_RESPONSE_MAX+40];
 if(payload->n>VV_ROUGHTIME_RESPONSE_MAX||context_n>40)return -EPROTO;
 memcpy(message,context,context_n);memcpy(message+context_n,payload->p,payload->n);
 return c->verify(sig->p,message,context_n+payload->n,key)?-EACCES:0;
}
int vv_roughtime_verify_with_crypto(const uint8_t *response,size_t bytes,const uint8_t nonce[64],
 struct vv_roughtime_result *out,const struct vv_roughtime_crypto *c){
 if(out)memset(out,0,sizeof(*out));
 if(!response||!nonce||!out||!c||!c->verify||!c->sha512)return -EINVAL;
 struct field m={response,bytes},cert,dele,srep,sig,csig,pub,mint,maxt,mid,radi,root,path,index;
 if(get(&m,"CERT",SIZE_MAX,&cert)||get(&m,"SREP",SIZE_MAX,&srep)||get(&m,"SIG\0",64,&sig)||
    get(&m,"PATH",SIZE_MAX,&path)||get(&m,"INDX",4,&index)||
    get(&cert,"DELE",SIZE_MAX,&dele)||get(&cert,"SIG\0",64,&csig)||
    get(&dele,"PUBK",32,&pub)||get(&dele,"MINT",8,&mint)||get(&dele,"MAXT",8,&maxt)||
    get(&srep,"MIDP",8,&mid)||get(&srep,"RADI",4,&radi)||get(&srep,"ROOT",64,&root))return -EPROTO;
 if(path.n%64||path.n>32u*64u)return -EPROTO;
 static const char delegation_context[]="RoughTime v1 delegation signature--";
 static const char response_context[]="RoughTime v1 response signature";
 int rc=signature(c,&csig,&dele,root_key,delegation_context,sizeof(delegation_context));if(rc)return rc;
 rc=signature(c,&sig,&srep,pub.p,response_context,sizeof(response_context));if(rc)return rc;
 uint64_t midpoint=u64(mid.p),low=u64(mint.p),high=u64(maxt.p);uint32_t radius=u32(radi.p);
 if(low>high||midpoint<low||midpoint>high||radius>2000000||
    midpoint<UINT64_C(946684800000000)||midpoint>UINT64_C(4102444799999999))return -ERANGE;
 uint8_t hash[64],input[129];input[0]=0;memcpy(input+1,nonce,64);
 if(c->sha512(hash,input,65))return -EIO;
 uint32_t position=u32(index.p);
 for(size_t off=0;off<path.n;off+=64){
  input[0]=1;
  memcpy(input+1,(position&1)?path.p+off:hash,64);
  memcpy(input+65,(position&1)?hash:path.p+off,64);
  if(c->sha512(hash,input,129))return -EIO;
  position>>=1;
 }
 unsigned difference=0;for(unsigned i=0;i<64;i++)difference|=hash[i]^root.p[i];
 if(position||difference)return -EACCES;
 out->midpoint_us=midpoint;out->radius_us=radius;return 0;
}
#ifndef VV_ROUGHTIME_TEST_CRYPTO
static int sodium_verify(const uint8_t sig[64],const uint8_t *p,size_t n,const uint8_t key[32]){
 return crypto_sign_verify_detached(sig,p,(unsigned long long)n,key);
}
static int sodium_hash(uint8_t out[64],const uint8_t *p,size_t n){return crypto_hash_sha512(out,p,(unsigned long long)n);}
int vv_roughtime_verify(const uint8_t *p,size_t n,const uint8_t nonce[64],struct vv_roughtime_result *out){
 const struct vv_roughtime_crypto c={sodium_verify,sodium_hash};
 return vv_roughtime_verify_with_crypto(p,n,nonce,out,&c);
}
#endif
