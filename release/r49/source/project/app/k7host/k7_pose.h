/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_POSE_H
#define K7_POSE_H
#include <stddef.h>
struct k7_pose_s;
struct k7_pose_s *k7_pose_create(void);
void k7_pose_destroy(struct k7_pose_s *);
int k7_pose_infer(struct k7_pose_s *,const float input[12288],float output[3]);
const float *k7_pose_arena(struct k7_pose_s *);
#endif
