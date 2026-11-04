/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed FSA-Net runtime; weights retain the supplied model's license. */
#include "k7_pose.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
static void yn_conv(const float *in, float *out, const float *w,
                    const float *bias, int ci, int ih, int iw,
                    int co, int oh, int ow, int k, int stride, int pad,
                    int groups)
{
  int plane = oh * ow;
  if (k == 1 && stride == 1 && groups == 1)
    {
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

#include "pose_kernels.inc"
#include "k7_pose_model.inc"
struct k7_pose_s { float *arena; };
struct k7_pose_s *k7_pose_create(void) {
  struct k7_pose_s *p=calloc(1,sizeof(*p));
  if(!p)return NULL;
  p->arena=malloc(POSE_FLOATS*sizeof(float));
  if(!p->arena){free(p);return NULL;}return p;
}
void k7_pose_destroy(struct k7_pose_s *p) {if(p){free(p->arena);free(p);}}
const float *k7_pose_arena(struct k7_pose_s *p) {return p?p->arena:NULL;}
int k7_pose_infer(struct k7_pose_s *p,const float input[12288],float output[3]) {
  if(!p||!input||!output)return -1;
  for(int i=0;i<12288;i++)if(!isfinite(input[i]))return -1;
  memcpy(p->arena,input,12288*sizeof(float));pose_graph(p->arena);
  for(int i=0;i<3;i++){
    output[i]=p->arena[POSE_OUTPUT+i];
    if(!isfinite(output[i])||fabsf(output[i])>99)return -1;
  }
  return 0;
}
