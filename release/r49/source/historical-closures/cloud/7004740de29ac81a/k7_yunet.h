/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_YUNET_H
#define K7_YUNET_H
#include <stddef.h>
#include <stdint.h>
#define K7_FACE_MAX 16
struct k7_face_s { float x, y, width, height, score; };
struct k7_faces_s
{
  unsigned int count;
  struct k7_face_s face[K7_FACE_MAX];
  uint64_t usec;
};
struct k7_yunet_s;
struct k7_yunet_s *k7_yunet_create(void);
void k7_yunet_destroy(struct k7_yunet_s *ctx);
/* Fixed RGB160x120 camera input, BGR160x160 letterbox, original FP32 weights.
 * Coordinates are clipped to the unpadded 160x120 camera image.
 * This function is not reentrant on the same context.
 */
int k7_yunet_detect(struct k7_yunet_s *ctx, const uint8_t *rgb,
                    size_t bytes, struct k7_faces_s *faces);
/* Read-only output tensor access for reference verification, until next run. */
const float *k7_yunet_output(struct k7_yunet_s *ctx,
                             unsigned int index, size_t *count);
#endif

