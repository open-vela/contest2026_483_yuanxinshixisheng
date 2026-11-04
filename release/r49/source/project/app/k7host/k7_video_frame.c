/* SPDX-License-Identifier: Apache-2.0 */
#include "k7_video_frame.h"
#include <errno.h>
#include <string.h>
static void le(uint8_t *p,uint64_t v,unsigned int n)
{for(unsigned int i=0;i<n;i++){p[i]=(uint8_t)v;v>>=8;}}
uint32_t k7_video_crc32(const uint8_t *data,size_t size)
{
 uint32_t crc=0xffffffffu;
 for(size_t i=0;i<size;i++)
  {
   crc^=data[i];
   for(unsigned int bit=0;bit<8;bit++)
    crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
  }
 return ~crc;
}
int k7_video_header(uint8_t header[K7_VIDEO_HEADER_BYTES],uint32_t sequence,
                    uint64_t capture_us,uint16_t width,uint16_t height,
                    const uint8_t *jpeg,size_t size)
{
 if(!header || !jpeg || size<4 || size>K7_VIDEO_MAX_BYTES ||
    !width || width>4096 || !height || height>4096 ||
    jpeg[0]!=0xff || jpeg[1]!=0xd8 || jpeg[size-2]!=0xff || jpeg[size-1]!=0xd9)
   return -EINVAL;
 memcpy(header,"K7MJPG1\0",8);
 le(header+8,sequence,4);le(header+12,capture_us,8);le(header+20,size,4);
 le(header+24,width,2);le(header+26,height,2);
 le(header+28,k7_video_crc32(jpeg,size),4);
 le(header+32,k7_video_crc32(header,32),4);
 return 0;
}
