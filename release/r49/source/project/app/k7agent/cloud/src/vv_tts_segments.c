#include "vv_tts_segments.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
static bool scalar(const char *text,size_t bytes,size_t at,uint32_t *value,size_t *end) {
  if(at>=bytes)return false;
  const unsigned char *s=(const unsigned char *)text;unsigned c=s[at++],n,min;
  uint32_t v;
  if(c<128){v=c;n=0;min=0;}
  else if(c>=0xc2 && c<=0xdf){v=c&31;n=1;min=0x80;}
  else if(c>=0xe0 && c<=0xef){v=c&15;n=2;min=0x800;}
  else if(c>=0xf0 && c<=0xf4){v=c&7;n=3;min=0x10000;}
  else return false;
  if(n>bytes-at)return false;
  while(n--){unsigned d=s[at++];if(d<0x80 || d>0xbf)return false;v=(v<<6)|(d&63);}
  if(v<min || v>0x10ffff || (v>=0xd800 && v<=0xdfff))return false;
  *value=v;*end=at;return true;
}
static bool space(uint32_t c) {
  return (c>=9 && c<=13)||c==32||c==0x85||c==0xa0||c==0x1680||
    (c>=0x2000 && c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000;
}
static bool allowed(uint32_t c){return c!=0 && c!=127 && (c>=32 || space(c));}
static bool ending(uint32_t c){return c=='.'||c=='!'||c=='?'||c==';'||c==0x3002||c==0xff01||c==0xff1f||c==0xff1b;}
int vv_tts_segments_init(struct vv_tts_segments *cursor,const char *text,size_t bytes) {
  if(!cursor || !text)return VV_TTS_SEGMENT_INVALID;
  bool content=false;
  for(size_t at=0;at<bytes;){uint32_t c;size_t end;
    if(!scalar(text,bytes,at,&c,&end)||!allowed(c))return VV_TTS_SEGMENT_INVALID;
    if(!space(c))content=true;
    at=end;
  }
  if(!content)return VV_TTS_SEGMENT_EMPTY;
  *cursor=(struct vv_tts_segments){text,bytes,0};return 0;
}
int vv_tts_segments_next_limit(struct vv_tts_segments *cursor,char *out,size_t cap,size_t *written,size_t max_scalars) {
  if(written)*written=0;
  if(out && cap)out[0]=0;
  if(!cursor || !cursor->text || cursor->offset>cursor->bytes || !out || !cap || !written||!max_scalars||max_scalars>VV_TTS_SEGMENT_SCALARS)return VV_TTS_SEGMENT_INVALID;
  size_t at=cursor->offset,end;uint32_t c;
  while(at<cursor->bytes){
    if(!scalar(cursor->text,cursor->bytes,at,&c,&end)||!allowed(c))return VV_TTS_SEGMENT_INVALID;
    if(!space(c))break;
    at=end;
  }
  if(at==cursor->bytes){cursor->offset=at;return VV_TTS_SEGMENT_DONE;}
  size_t start=at,count=0,last_nonspace=at,sentence=at;
  while(at<cursor->bytes){
    if(!scalar(cursor->text,cursor->bytes,at,&c,&end)||!allowed(c))return VV_TTS_SEGMENT_INVALID;
    if(c<32)break; /* Trim control whitespace at next boundary, never emit it. */
    if(count==max_scalars || end-start>VV_TTS_SEGMENT_BYTES)break;
    ++count;at=end;
    if(!space(c))last_nonspace=at;
    if(ending(c))sentence=at;
  }
  size_t chosen=at;
  if(at<cursor->bytes && sentence>start)chosen=sentence;
  /* Recompute trailing trim after selecting the preferred sentence boundary. */
  if(chosen==sentence && sentence>start)last_nonspace=sentence;
  size_t length=last_nonspace-start;
  if(!length)return VV_TTS_SEGMENT_INVALID;
  if(length>=cap)return VV_TTS_SEGMENT_SPACE;
  memcpy(out,cursor->text+start,length);out[length]=0;*written=length;
  cursor->offset=chosen;return VV_TTS_SEGMENT_READY;
}
int vv_tts_segments_next(struct vv_tts_segments *cursor,char *out,size_t cap,size_t *written){
 return vv_tts_segments_next_limit(cursor,out,cap,written,VV_TTS_SEGMENT_SCALARS);
}
