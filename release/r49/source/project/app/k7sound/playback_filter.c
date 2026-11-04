#include "playback_filter.h"
#include <errno.h>
#include <limits.h>
static int overlap(uintptr_t a,size_t an,uintptr_t b,size_t bn){return a<b+bn && b<a+an;}
static int64_t signed_word(uint32_t w){return w<=INT32_MAX?(int64_t)w:-1-(int64_t)(UINT32_MAX-w);}
int af_filter(const uint32_t*src,size_t sw,uint32_t*dst,size_t dw,
 unsigned frames,enum af_mode mode,uint32_t*sat)
{
 size_t words,bytes;uintptr_t a=(uintptr_t)src,b=(uintptr_t)dst,c=(uintptr_t)sat;
 int64_t history[2][4]={{0}},sum[2]={0},prevx[2]={0},prevy[2]={0};
 uint32_t clipped=0;
 if(!src||!dst||!sat||!frames||frames>AF_MAX_FRAMES ||
   (mode!=AF_MA4&&mode!=AF_MA4_HP80))return -EINVAL;
 words=(size_t)frames*2;bytes=words*sizeof(uint32_t);
 if(sw<words||dw<words || a%_Alignof(uint32_t) || b%_Alignof(uint32_t) || c%_Alignof(uint32_t))return -EINVAL;
 if(a>UINTPTR_MAX-bytes || b>UINTPTR_MAX-bytes || c>UINTPTR_MAX-sizeof(*sat))return -EINVAL;
 if(overlap(a,bytes,b,bytes)||overlap(a,bytes,c,sizeof(*sat))||overlap(b,bytes,c,sizeof(*sat)))return -EINVAL;
 for(unsigned n=0;n<frames;n++)for(unsigned ch=0;ch<2;ch++){
  unsigned slot=n%4;int64_t x=signed_word(src[2u*n+ch]),y;
  sum[ch]+=x-history[ch][slot];history[ch][slot]=x;y=sum[ch]/4;
  if(mode==AF_MA4_HP80){
   int64_t current=y;
   y=(AF_HP_Q30*(prevy[ch]+current-prevx[ch]))/INT64_C(1073741824);
   prevx[ch]=current;
  }
  if(y>INT32_MAX){y=INT32_MAX;clipped++;}
  else if(y<INT32_MIN){y=INT32_MIN;clipped++;}
  prevy[ch]=y;dst[2u*n+ch]=(uint32_t)y;
 }
 *sat=clipped;return 0;
}
