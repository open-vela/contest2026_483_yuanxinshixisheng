/* SPDX-License-Identifier: Apache-2.0 */
/* Bounded RGB decoding of the K7 camera's 640x480 JPEG frames.
 * Kept portable so the identical decoder/settings can be checked on a host.
 */
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jpeglib.h>
#include "k7_jpeg.h"

struct k7_jpeg_error_s
{
  struct jpeg_error_mgr pub;
  jmp_buf jump;
  unsigned int warnings;
};

struct k7_jpeg_context_s
{
  struct jpeg_decompress_struct info;
  struct k7_jpeg_error_s error;
  int created;
};

static uint64_t jpeg_usec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void jpeg_failure(j_common_ptr info)
{
  struct k7_jpeg_error_s *error = (struct k7_jpeg_error_s *)info->err;
  longjmp(error->jump, 1);
}

static void jpeg_message(j_common_ptr info, int level)
{
  struct k7_jpeg_error_s *error = (struct k7_jpeg_error_s *)info->err;
  if (level < 0) error->warnings++;
}

int k7_jpeg_decode(const uint8_t *jpeg, size_t size, unsigned int scale,
                   uint8_t *rgb, size_t capacity,
                   struct k7_jpeg_result_s *result)
{
  struct k7_jpeg_context_s *ctx;
  uint64_t started;
  int ret;

  if (!result) return -EINVAL;
  memset(result, 0, sizeof(*result));
  if (!jpeg || !rgb || size < 4 || size > 4 * 1024 * 1024 ||
      (scale != 1 && scale != 2 && scale != 4 && scale != 8))
    return -EINVAL;

  ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return -ENOMEM;
  started = jpeg_usec();
  ctx->info.err = jpeg_std_error(&ctx->error.pub);
  ctx->error.pub.error_exit = jpeg_failure;
  ctx->error.pub.emit_message = jpeg_message;

  /* State modified by libjpeg lives on the heap and survives longjmp.
   * Do not let malformed JPEGs invoke libjpeg's default process exit. */
  if (setjmp(ctx->error.jump))
    {
      ret = -EILSEQ;
      goto done;
    }

  ctx->created = 1;
  jpeg_create_decompress(&ctx->info);
  jpeg_mem_src(&ctx->info, jpeg, size);
  if (jpeg_read_header(&ctx->info, TRUE) != JPEG_HEADER_OK ||
      ctx->info.image_width != 640 || ctx->info.image_height != 480)
    {
      ret = -ENOTSUP;
      goto done;
    }

  ctx->info.out_color_space = JCS_RGB;
  ctx->info.scale_num = 1;
  ctx->info.scale_denom = scale;
  ctx->info.dct_method = JDCT_ISLOW;
  ctx->info.do_fancy_upsampling = TRUE;
  ctx->info.do_block_smoothing = FALSE;
  jpeg_calc_output_dimensions(&ctx->info);
  if (ctx->info.output_width != 640 / scale ||
      ctx->info.output_height != 480 / scale ||
      capacity < (size_t)ctx->info.output_width * ctx->info.output_height * 3)
    {
      ret = -ENOSPC;
      goto done;
    }

  if (!jpeg_start_decompress(&ctx->info) || ctx->info.output_components != 3)
    {
      ret = -EIO;
      goto done;
    }

  while (ctx->info.output_scanline < ctx->info.output_height)
    {
      JSAMPROW row = rgb + (size_t)ctx->info.output_scanline *
                          ctx->info.output_width * 3;
      if (jpeg_read_scanlines(&ctx->info, &row, 1) != 1)
        {
          ret = -EIO;
          goto done;
        }
    }

  ret = jpeg_finish_decompress(&ctx->info) ? 0 : -EIO;
  if (ctx->error.warnings) ret = -EILSEQ;
  if (ret == 0)
    {
      result->width = ctx->info.output_width;
      result->height = ctx->info.output_height;
      result->bytes = (size_t)result->width * result->height * 3;
    }

done:
  result->warnings = ctx->error.warnings;
  result->error_code = ret < 0 ? ctx->error.pub.msg_code : 0;
  if (ctx->created) jpeg_destroy_decompress(&ctx->info);
  free(ctx);
  result->usec = jpeg_usec() - started;
  return ret;
}

uint32_t k7_rgb_crc32(const uint8_t *data, size_t size)
{
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < size; i++)
    {
      crc ^= data[i];
      for (unsigned int bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
  return ~crc;
}

