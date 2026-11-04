/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_PHOTO_TARGET_H
#define K7_PHOTO_TARGET_H
#include "k7_yunet.h"
#include <math.h>
/* Geometry only, not identity recognition. Ambiguous candidates remain blocked. */
static inline int k7_photo_box_valid(const struct k7_face_s *f)
{
  return isfinite(f->x)&&isfinite(f->y)&&isfinite(f->width)&&isfinite(f->height)&&
    isfinite(f->score)&&f->score>=.7f&&f->score<=1&&f->x>=0&&f->y>=0&&
    f->width>0&&f->height>0&&f->x+f->width<=160.01f&&f->y+f->height<=120.01f;
}
static inline float k7_photo_intersection(const struct k7_face_s *a,const struct k7_face_s *b)
{
  return fmaxf(0,fminf(a->x+a->width,b->x+b->width)-fmaxf(a->x,b->x))*
         fmaxf(0,fminf(a->y+a->height,b->y+b->height)-fmaxf(a->y,b->y));
}
static inline unsigned k7_photo_target_count(const struct k7_faces_s *faces,const struct k7_face_s *target)
{
  if(!faces||!faces->count)return 0;
  if(faces->count>K7_FACE_MAX||!target||!k7_photo_box_valid(target))return 2;
  float area=target->width*target->height;
  int selected=-1;float best=.8f;
  for(unsigned i=0;i<faces->count;i++) {
    const struct k7_face_s *f=&faces->face[i];
    if(!k7_photo_box_valid(f))return 2;
    float intersection=k7_photo_intersection(target,f);
    float overlap=intersection/(area+f->width*f->height-intersection);
    if(overlap>=best){selected=(int)i;best=overlap;}
  }
  if(selected<0)return 2;
  for(unsigned i=0;i<faces->count;i++) {
    if((int)i==selected)continue;
    const struct k7_face_s *f=&faces->face[i];
    /* Ignore only clearly smaller, fully disjoint background detections. */
    if(f->width*f->height>=area*.25f || k7_photo_intersection(target,f)>0)return 2;
  }
  return 1;
}
#endif
