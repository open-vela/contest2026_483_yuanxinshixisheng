/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VV_MICRO_VOICE_AUDIO_H
#define VV_MICRO_VOICE_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
/* Short immutable 8 kHz clips, packed seven-bit amplitude indices. Decode
 * into the existing 16 kHz PCM sink; no new audio owner or network request. */
struct vv_micro_voice_clip {const char *text;const unsigned char *data;size_t bytes,samples;};
struct vv_micro_voice_decode {size_t position;int previous;};
static inline int vv_micro_voice_audio_next(const struct vv_micro_voice_clip *clip,const int16_t levels[128],struct vv_micro_voice_decode *state,unsigned char *pcm,size_t capacity,size_t *used){
 if(!clip||!clip->data||!levels||!state||!pcm||!used||capacity<4||!clip->samples||clip->samples>48000||clip->bytes!=(clip->samples*7+7)/8||state->position>clip->samples)return -EINVAL;
 *used=0;
 while(state->position<clip->samples&&capacity-*used>=4){
  size_t bit=state->position*7,at=bit/8;unsigned word=clip->data[at];
  if(at+1<clip->bytes)word|=(unsigned)clip->data[at+1]<<8;
  int sample=levels[(word>>(bit%8))&127u];int middle=state->position?(state->previous+sample)/2:sample;
  unsigned a=(unsigned)middle&65535u,b=(unsigned)sample&65535u;
  pcm[*used]=(unsigned char)a;pcm[*used+1]=(unsigned char)(a>>8);pcm[*used+2]=(unsigned char)b;pcm[*used+3]=(unsigned char)(b>>8);
  *used+=4;state->position++;state->previous=sample;
 }
 return 0;
}
#endif
