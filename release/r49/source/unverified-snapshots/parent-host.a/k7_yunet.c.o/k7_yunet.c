/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed YuNet inference implementation. Model weights retain their MIT license. */
#include "k7_yunet.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void yn_conv(const float *in, float *out, const float *w,
                    const float *bias, int ci, int ih, int iw,
                    int co, int oh, int ow, int k, int stride, int pad,
                    int groups)
{
  int plane = oh * ow;
  if (k == 1 && stride == 1 && groups == 1)
    {
#if defined(K7_YUNET_POINTWISE_TILE) && K7_YUNET_POINTWISE_TILE
      /* Four output channels by four adjacent spatial elements. Keep every
       * accumulator live across input channels, in the exact original order.
       * memcpy permits unaligned graph offsets and does not add alias rules. */
      typedef float vec4 __attribute__((vector_size(16)));
      const int vector_plane = plane - plane % 4;
      const int vector_channels = co - co % 4;
      int oc = 0;
      for (; oc < vector_channels; oc += 4)
        {
          int p = 0;
          for (; p < vector_plane; p += 4)
            {
              vec4 a0 = {bias[oc],bias[oc],bias[oc],bias[oc]};
              vec4 a1 = {bias[oc+1],bias[oc+1],bias[oc+1],bias[oc+1]};
              vec4 a2 = {bias[oc+2],bias[oc+2],bias[oc+2],bias[oc+2]};
              vec4 a3 = {bias[oc+3],bias[oc+3],bias[oc+3],bias[oc+3]};
              for (int ic = 0; ic < ci; ic++)
                {
                  vec4 v;
                  memcpy(&v,in+ic*plane+p,sizeof(v));
                  float z0=w[oc*ci+ic],z1=w[(oc+1)*ci+ic];
                  float z2=w[(oc+2)*ci+ic],z3=w[(oc+3)*ci+ic];
                  vec4 b0={z0,z0,z0,z0},b1={z1,z1,z1,z1};
                  vec4 b2={z2,z2,z2,z2},b3={z3,z3,z3,z3};
                  a0 += v*b0; a1 += v*b1; a2 += v*b2; a3 += v*b3;
                }
              memcpy(out+oc*plane+p,&a0,sizeof(a0));
              memcpy(out+(oc+1)*plane+p,&a1,sizeof(a1));
              memcpy(out+(oc+2)*plane+p,&a2,sizeof(a2));
              memcpy(out+(oc+3)*plane+p,&a3,sizeof(a3));
            }
          for (; p < plane; p++)
            {
              float a0=bias[oc],a1=bias[oc+1];
              float a2=bias[oc+2],a3=bias[oc+3];
              for (int ic=0;ic<ci;ic++)
                {
                  float v=in[ic*plane+p];
                  a0+=v*w[oc*ci+ic]; a1+=v*w[(oc+1)*ci+ic];
                  a2+=v*w[(oc+2)*ci+ic]; a3+=v*w[(oc+3)*ci+ic];
                }
              out[oc*plane+p]=a0; out[(oc+1)*plane+p]=a1;
              out[(oc+2)*plane+p]=a2; out[(oc+3)*plane+p]=a3;
            }
        }
      for (; oc < co; oc++)
        {
          for (int p=0;p<plane;p++)
            {
              float value=bias[oc];
              for (int ic=0;ic<ci;ic++)
                value+=in[ic*plane+p]*w[oc*ci+ic];
              out[oc*plane+p]=value;
            }
        }
#else
      for (int oc = 0; oc < co; oc++)
        {
          float *dst = out + oc * plane;
          for (int p = 0; p < plane; p++) dst[p] = bias[oc];
          for (int ic = 0; ic < ci; ic++)
            {
              const float *src = in + ic * plane;
              float weight = w[oc * ci + ic];
              for (int p = 0; p < plane; p++) dst[p] += src[p] * weight;
            }
        }
#endif
      return;
    }

  int inputs_per_group = ci / groups;
  int outputs_per_group = co / groups;
  for (int oc = 0; oc < co; oc++)
    {
      int first_input = (oc / outputs_per_group) * inputs_per_group;
      for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
          {
            float value = bias[oc];
            for (int ic = 0; ic < inputs_per_group; ic++)
              for (int ky = 0; ky < k; ky++)
                {
                  int sy = y * stride + ky - pad;
                  if (sy < 0 || sy >= ih) continue;
                  for (int kx = 0; kx < k; kx++)
                    {
                      int sx = x * stride + kx - pad;
                      if (sx < 0 || sx >= iw) continue;
                      value += in[((first_input + ic) * ih + sy) * iw + sx] *
                        w[((oc * inputs_per_group + ic) * k + ky) * k + kx];
                    }
                }
            out[(oc * oh + y) * ow + x] = value;
          }
    }
}
static void yn_relu(const float *in, float *out, size_t n)
{ for (size_t i = 0; i < n; i++) out[i] = fmaxf(in[i], 0.0f); }
static void yn_add(const float *a, const float *b, float *out, size_t n)
{ for (size_t i = 0; i < n; i++) out[i] = a[i] + b[i]; }
static void yn_pool(const float *in, float *out, int c, int h, int w)
{
  for (int channel = 0; channel < c; channel++)
    for (int y = 0; y < h / 2; y++)
      for (int x = 0; x < w / 2; x++)
        {
          const float *p = in + channel * h * w + y * 2 * w + x * 2;
          out[(channel * (h / 2) + y) * (w / 2) + x] =
            fmaxf(fmaxf(p[0], p[1]), fmaxf(p[w], p[w + 1]));
        }
}
static void yn_resize2(const float *in, float *out, int c, int h, int w)
{
  for (int channel = 0; channel < c; channel++)
    for (int y = 0; y < h * 2; y++)
      for (int x = 0; x < w * 2; x++)
        out[(channel * h * 2 + y) * w * 2 + x] =
          in[(channel * h + y / 2) * w + x / 2];
}
static void yn_transpose(const float *in, float *out, int c, int plane)
{
  for (int p = 0; p < plane; p++)
    for (int channel = 0; channel < c; channel++)
      out[p * c + channel] = in[channel * plane + p];
}
static void yn_sigmoid(const float *in, float *out, size_t n)
{
  for (size_t i = 0; i < n; i++)
    {
      float e = expf(-fabsf(in[i]));
      out[i] = in[i] >= 0 ? 1.0f / (1.0f + e) : e / (1.0f + e);
    }
}
/* Generated from the hash-pinned ONNX; includes weights, offsets and graph. */
#include "k7_yunet_model.inc"

struct k7_yunet_s
{
  float *arena;
  struct k7_face_s candidates[525];
};
static uint64_t yn_usec(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
struct k7_yunet_s *k7_yunet_create(void)
{
  struct k7_yunet_s *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->arena = malloc(YN_FLOATS * sizeof(float));
  if (!ctx->arena) { free(ctx); return NULL; }
  return ctx;
}
void k7_yunet_destroy(struct k7_yunet_s *ctx)
{
  if (ctx) { free(ctx->arena); free(ctx); }
}
const float *k7_yunet_output(struct k7_yunet_s *ctx,
                             unsigned int index, size_t *count)
{
  if (!ctx || !count || index >= 12) return NULL;
  *count = yn_output_lengths[index];
  return ctx->arena + yn_output_offsets[index];
}
static int compare_score(const void *a, const void *b)
{
  float d = ((const struct k7_face_s *)b)->score -
            ((const struct k7_face_s *)a)->score;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}
/* Match the integer box conversion used by OpenCV FaceDetectorYN NMS. */
static float overlap(const struct k7_face_s *a, const struct k7_face_s *b)
{
  int ax = (int)a->x, ay = (int)a->y, aw = (int)a->width, ah = (int)a->height;
  int bx = (int)b->x, by = (int)b->y, bw = (int)b->width, bh = (int)b->height;
  int x1 = ax > bx ? ax : bx, y1 = ay > by ? ay : by;
  int x2 = ax + aw < bx + bw ? ax + aw : bx + bw;
  int y2 = ay + ah < by + bh ? ay + ah : by + bh;
  float area = (float)aw * ah + (float)bw * bh;
  float intersection = x2 > x1 && y2 > y1 ? (float)(x2 - x1) * (y2 - y1) : 0;
  return area > intersection ? intersection / (area - intersection) : 0;
}
int k7_yunet_detect(struct k7_yunet_s *ctx, const uint8_t *rgb,
                    size_t bytes, struct k7_faces_s *faces)
{
  if (!ctx || !rgb || !faces || bytes != 160 * 120 * 3) return -EINVAL;
  uint64_t start = yn_usec();
  memset(faces, 0, sizeof(*faces));
  memset(ctx->arena, 0, 3 * 160 * 160 * sizeof(float));
  for (int y = 0; y < 120; y++)
    for (int x = 0; x < 160; x++)
      for (int c = 0; c < 3; c++)
        ctx->arena[c * 160 * 160 + (y + 20) * 160 + x] =
          rgb[(y * 160 + x) * 3 + 2 - c];
  yn_graph(ctx->arena);
  unsigned int count = 0;
  for (int level = 0; level < 3; level++)
    {
      int stride = 8 << level, side = 160 / stride, n = side * side;
      const float *cls = ctx->arena + yn_output_offsets[level];
      const float *obj = ctx->arena + yn_output_offsets[level + 3];
      const float *box = ctx->arena + yn_output_offsets[level + 6];
      for (int i = 0; i < n; i++)
        {
          float score = sqrtf(fmaxf(0, fminf(1, cls[i])) *
                              fmaxf(0, fminf(1, obj[i])));
          if (!isfinite(score) || score < 0.7f) continue;
          float cx = ((i % side) + box[i * 4]) * stride;
          float cy = ((i / side) + box[i * 4 + 1]) * stride;
          float width = expf(box[i * 4 + 2]) * stride;
          float height = expf(box[i * 4 + 3]) * stride;
          if (!isfinite(cx) || !isfinite(cy) || !isfinite(width) ||
              !isfinite(height) || width <= 0 || height <= 0 ||
              fabsf(cx) > 1000000 || fabsf(cy) > 1000000 ||
              width > 1000000 || height > 1000000) continue;
          ctx->candidates[count++] = (struct k7_face_s){
            cx - width / 2, cy - height / 2, width, height, score};
        }
    }
  qsort(ctx->candidates, count, sizeof(ctx->candidates[0]), compare_score);
  /* Suppress before clipping or removing letterbox, as the reference does. */
  for (unsigned int i = 0; i < count; i++)
    {
      struct k7_face_s *a = &ctx->candidates[i];
      if (a->score < 0) continue;
      for (unsigned int j = i + 1; j < count; j++)
        if (ctx->candidates[j].score >= 0 &&
            overlap(a, &ctx->candidates[j]) > 0.3f)
          ctx->candidates[j].score = -1;
      float x1 = fmaxf(0, a->x), y1 = fmaxf(0, a->y - 20);
      float x2 = fminf(160, a->x + a->width);
      float y2 = fminf(120, a->y + a->height - 20);
      if (x2 > x1 && y2 > y1 && faces->count < K7_FACE_MAX)
        faces->face[faces->count++] = (struct k7_face_s){
          x1, y1, x2 - x1, y2 - y1, a->score};
    }
  faces->usec = yn_usec() - start;
  return 0;
}

