/* SPDX-License-Identifier: Apache-2.0 */
/* Encode the exact RGB160 inference input and frame it over the debug UART. */
#include "k7_preview.h"
#include <errno.h>
#include <stdio.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <time.h>

#define K7_PREVIEW_RGB_SIZE (160 * 120 * 3)
#define K7_PREVIEW_INITIAL_JPEG (16 * 1024)
#define K7_PREVIEW_MAX_JPEG (64 * 1024)
#define K7_PREVIEW_RAW_CHUNK 36

struct preview_error_s { struct jpeg_error_mgr pub; jmp_buf jump; };
struct preview_context_s
{
  struct jpeg_compress_struct info;
  struct preview_error_s error;
  unsigned char *initial, *jpeg;
  unsigned long jpeg_size;
  int created;
};

static void preview_failure(j_common_ptr info)
{
  struct preview_error_s *error=(struct preview_error_s *)info->err;
  longjmp(error->jump,1);
}

static uint32_t preview_crc32(const uint8_t *data,size_t size)
{
  uint32_t crc=0xffffffff;
  for(size_t i=0;i<size;i++)
    {
      crc^=data[i];
      for(unsigned int bit=0;bit<8;bit++)
        crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
  return ~crc;
}

static int emit_jpeg(const uint8_t *jpeg,size_t size,unsigned int sequence)
{
  static const char alphabet[]=
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char encoded[K7_PREVIEW_RAW_CHUNK*4/3+1];
  if(size>K7_PREVIEW_MAX_JPEG)return -EFBIG;
  if(printf("VIEW BEGIN q=%u n=%u crc=%08x\n",sequence,
            (unsigned int)size,preview_crc32(jpeg,size))<0)return -EIO;
  for(size_t offset=0;offset<size;offset+=K7_PREVIEW_RAW_CHUNK)
    {
      size_t count=size-offset;
      if(count>K7_PREVIEW_RAW_CHUNK)count=K7_PREVIEW_RAW_CHUNK;
      size_t in=0,out=0;
      while(in<count)
        {
          size_t left=count-in;
          uint32_t value=(uint32_t)jpeg[offset+in]<<16;
          if(left>1)value|=(uint32_t)jpeg[offset+in+1]<<8;
          if(left>2)value|=jpeg[offset+in+2];
          encoded[out++]=alphabet[(value>>18)&63];
          encoded[out++]=alphabet[(value>>12)&63];
          encoded[out++]=left>1?alphabet[(value>>6)&63]:'=';
          encoded[out++]=left>2?alphabet[value&63]:'=';
          in+=left>2?3:left;
        }
      encoded[out]='\0';
      if(printf("V %s\n",encoded)<0)return -EIO;
      struct timespec pace={0,1000000};nanosleep(&pace,NULL);
    }
  return printf("VIEW END q=%u\n",sequence)<0?-EIO:0;
}

int k7_preview_emit(const uint8_t *rgb,size_t size,unsigned int sequence)
{
  if(!rgb||size!=K7_PREVIEW_RGB_SIZE)return -EINVAL;
  struct preview_context_s *ctx=calloc(1,sizeof(*ctx));
  if(!ctx)return -ENOMEM;
  int ret=-EILSEQ;
  ctx->info.err=jpeg_std_error(&ctx->error.pub);
  ctx->error.pub.error_exit=preview_failure;
  if(setjmp(ctx->error.jump))goto done;
  ctx->created=1;jpeg_create_compress(&ctx->info);
  ctx->initial=malloc(K7_PREVIEW_INITIAL_JPEG);
  if(!ctx->initial){ret=-ENOMEM;goto done;}
  ctx->jpeg=ctx->initial;ctx->jpeg_size=K7_PREVIEW_INITIAL_JPEG;
  jpeg_mem_dest(&ctx->info,&ctx->jpeg,&ctx->jpeg_size);
  ctx->info.image_width=160;ctx->info.image_height=120;
  ctx->info.input_components=3;ctx->info.in_color_space=JCS_RGB;
  jpeg_set_defaults(&ctx->info);jpeg_set_quality(&ctx->info,55,TRUE);
  ctx->info.optimize_coding=FALSE;ctx->info.dct_method=JDCT_FASTEST;
  jpeg_start_compress(&ctx->info,TRUE);
  while(ctx->info.next_scanline<ctx->info.image_height)
    {
      JSAMPROW row=(JSAMPROW)(rgb+(size_t)ctx->info.next_scanline*160*3);
      if(jpeg_write_scanlines(&ctx->info,&row,1)!=1)goto done;
    }
  jpeg_finish_compress(&ctx->info);
  ret=emit_jpeg(ctx->jpeg,ctx->jpeg_size,sequence);
done:
  if(ctx->created)jpeg_destroy_compress(&ctx->info);
  if(ctx->jpeg!=ctx->initial)free(ctx->jpeg);
  free(ctx->initial);free(ctx);return ret;
}
