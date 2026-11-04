/* SPDX-License-Identifier: Apache-2.0 */
#ifndef __EXAMPLES_K7HOST_K7_JPEG_H
#define __EXAMPLES_K7HOST_K7_JPEG_H
#include <stddef.h>
#include <stdint.h>
struct k7_jpeg_result_s
{
  unsigned int width;
  unsigned int height;
  unsigned int warnings;
  int error_code;
  size_t bytes;
  uint64_t usec;
};
int k7_jpeg_decode(const uint8_t *jpeg, size_t size, unsigned int scale,
                   uint8_t *rgb, size_t capacity,
                   struct k7_jpeg_result_s *result);
uint32_t k7_rgb_crc32(const uint8_t *data, size_t size);
#endif

