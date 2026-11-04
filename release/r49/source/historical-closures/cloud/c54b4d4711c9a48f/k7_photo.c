/* SPDX-License-Identifier: Apache-2.0 */
#include "k7_photo.h"
#include "k7_pose.h"
#include "k7_jpeg.h"
#include "fusion_core.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
static pthread_mutex_t store_owner=PTHREAD_MUTEX_INITIALIZER;
static struct k7_photo_store *native_store;
static pthread_mutex_t ack_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned ack_q,ack_epoch,ack_side;
static bool ack_ok,ack_valid;
static atomic_bool reset_requested;
static atomic_bool retry_required;
bool k7_photo_needs_restart(void) { return atomic_load(&retry_required); }
static unsigned epoch_counter;
/* Operator confirmed: own-left head turn produces positive native yaw. */
static const struct vf_config config={45,7,12,3,80,60,500,350,700,500,1,13};
struct k7_photo_s {
  struct k7_photo_store *store;
  struct k7_pose_s *pose;
  struct vf_state gate;
  uint8_t *rgb;
  float input[12288],gray[160*160];
  struct vf_pose history[3],filtered,pending_raw,pending_filtered;
  unsigned hn,hp,epoch;
  uint64_t last_filter,missing_since,pending_at,last_print;
  int last_x,last_y;
  bool have_target,locked,lost;
  struct k7_face_s previous;
  float pending_sharp,pending_size;
};
static uint64_t now_ms(void) {
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
void k7_photo_ack(unsigned q,unsigned epoch,unsigned side,bool ok) {
  pthread_mutex_lock(&ack_lock);
  ack_q=q;ack_epoch=epoch;ack_side=side;ack_ok=ok;ack_valid=true;
  pthread_mutex_unlock(&ack_lock);
}
void k7_photo_reset(void) {atomic_store(&reset_requested,true);}
static void reset_observation(struct k7_photo_s *p) {
  vf_init(&p->gate,&config,p->epoch);
  p->hn=p->hp=0;p->last_filter=0;p->missing_since=0;
  p->locked=false;p->lost=false;p->last_print=0;
  atomic_store(&retry_required,false);
}
static void reset(struct k7_photo_s *p) {
  p->epoch=++epoch_counter;
  if(p->store && k7_photo_store_begin(p->store,p->epoch))
    puts("PHOTO RAM reset_failed=1");
  reset_observation(p);
}
struct k7_photo_s *k7_photo_create(void) {
  struct k7_photo_s *p=calloc(1,sizeof(*p));if(!p)return NULL;
  p->pose=k7_pose_create();p->rgb=malloc(640*480*3);
  if(!p->pose||!p->rgb){k7_photo_destroy(p);return NULL;}
  atomic_store(&reset_requested,false);
  pthread_mutex_lock(&ack_lock);ack_valid=false;pthread_mutex_unlock(&ack_lock);
  reset(p);return p;
}
void k7_photo_destroy(struct k7_photo_s *p) {
  if(p){
    pthread_mutex_lock(&store_owner);
    if(native_store==p->store)native_store=NULL;
    k7_photo_store_destroy(p->store);
    pthread_mutex_unlock(&store_owner);
    k7_pose_destroy(p->pose);free(p->rgb);free(p);
  }
}
struct k7_photo_s *k7_photo_create_native(void) {
  struct k7_photo_s *p=k7_photo_create();if(!p)return NULL;
  p->store=k7_photo_store_create(1024*1024);
  if(!p->store || k7_photo_store_begin(p->store,p->epoch))
    {k7_photo_destroy(p);return NULL;}
  pthread_mutex_lock(&store_owner);
  if(native_store){pthread_mutex_unlock(&store_owner);k7_photo_destroy(p);return NULL;}
  native_store=p->store;
  pthread_mutex_unlock(&store_owner);
  return p;
}
int k7_photo_native_copy(uint32_t epoch,unsigned side,struct k7_photo_record *r,void *dst,size_t cap) {
  pthread_mutex_lock(&store_owner);
  int rc=native_store?k7_photo_store_copy(native_store,epoch,side,r,dst,cap):-ENODEV;
  pthread_mutex_unlock(&store_owner);return rc;
}
int k7_photo_native_discard(uint32_t epoch) {
  pthread_mutex_lock(&store_owner);
  int rc=native_store?k7_photo_store_discard(native_store,epoch):-ENODEV;
  pthread_mutex_unlock(&store_owner);return rc;
}
int k7_photo_native_complete(uint32_t epoch) {
  pthread_mutex_lock(&store_owner);
  int rc=native_store?k7_photo_store_complete(native_store,epoch):-ENODEV;
  pthread_mutex_unlock(&store_owner);return rc;
}
int k7_photo_native_status(uint32_t *epoch,unsigned *done) {
  pthread_mutex_lock(&store_owner);
  int rc=native_store?k7_photo_store_status(native_store,epoch,done):-ENODEV;
  pthread_mutex_unlock(&store_owner);return rc;
}
static float overlap(struct k7_face_s a,struct k7_face_s b) {
  float w=fmaxf(0,fminf(a.x+a.width,b.x+b.width)-fmaxf(a.x,b.x));
  float h=fmaxf(0,fminf(a.y+a.height,b.y+b.height)-fmaxf(a.y,b.y));
  float v=w*h;return v/fmaxf(1,a.width*a.height+b.width*b.height-v);
}
static float pixel(const uint8_t *rgb,int x0,int y0,int side,int n,int x,int y,int c) {
  float sx=fmaxf(0,((float)x+.5f)*side/n-.5f);
  float sy=fmaxf(0,((float)y+.5f)*side/n-.5f);
  int ix=(int)sx,iy=(int)sy,jx=ix+1<side?ix+1:ix,jy=iy+1<side?iy+1:iy;
  float fx=sx-ix,fy=sy-iy;
  float a=rgb[((y0+iy)*640+x0+ix)*3+c]*(1-fx)+rgb[((y0+iy)*640+x0+jx)*3+c]*fx;
  float b=rgb[((y0+jy)*640+x0+ix)*3+c]*(1-fx)+rgb[((y0+jy)*640+x0+jx)*3+c]*fx;
  return roundf(a*(1-fy)+b*fy);
}
static float prepare(struct k7_photo_s *p,struct k7_face_s f) {
  float side=fminf(480,fmaxf(f.width,f.height)*4*1.35f);
  int n=(int)lrintf(side),x=(int)lrintf((f.x+f.width/2)*4-side/2);
  int y=(int)lrintf((f.y+f.height/2)*4-side*.04f-side/2);
  if(n<1)n=1;
  if(x<0)x=0;
  if(x>640-n)x=640-n;
  if(y<0)y=0;
  if(y>480-n)y=480-n;
  for(int j=0;j<64;j++)for(int i=0;i<64;i++)for(int c=0;c<3;c++)
    p->input[c*4096+j*64+i]=(pixel(p->rgb,x,y,n,64,i,j,2-c)-127.5f)/128;
  for(int j=0;j<160;j++)for(int i=0;i<160;i++) {
    float r=pixel(p->rgb,x,y,n,160,i,j,0),g=pixel(p->rgb,x,y,n,160,i,j,1),b=pixel(p->rgb,x,y,n,160,i,j,2);
    p->gray[j*160+i]=roundf(.299f*r+.587f*g+.114f*b);
  }
  double sum=0,sq=0;
  for(int j=1;j<159;j++)for(int i=1;i<159;i++) {
    float v=p->gray[(j-1)*160+i]+p->gray[(j+1)*160+i]+p->gray[j*160+i-1]+p->gray[j*160+i+1]-4*p->gray[j*160+i];
    sum+=v;sq+=v*v;
  }
  double count=158*158;return (float)(sq/count-(sum/count)*(sum/count));
}
static float median3(float *v,unsigned n) {
  for(unsigned i=0;i<n;i++)for(unsigned j=i+1;j<n;j++)if(v[i]>v[j]){float t=v[i];v[i]=v[j];v[j]=t;}
  return n==2?(v[0]+v[1])*.5f:v[n/2];
}
static struct vf_pose filter(struct k7_photo_s *p,struct vf_pose raw,uint64_t stamp) {
  if(!p->last_filter||stamp<=p->last_filter||stamp-p->last_filter>350)p->hn=p->hp=0;
  p->history[p->hp]=raw;p->hp=(p->hp+1)%3;if(p->hn<3)p->hn++;
  float alpha=p->hn==1?1:1-expf(-(float)(stamp-p->last_filter)/120);
  float v[3];
#define SMOOTH(field) for(unsigned i=0;i<p->hn;i++)v[i]=p->history[i].field; \
  p->filtered.field+=alpha*(median3(v,p->hn)-p->filtered.field)
  SMOOTH(yaw);SMOOTH(pitch);SMOOTH(roll);
#undef SMOOTH
  p->filtered.valid=true;p->last_filter=stamp;return p->filtered;
}
static void pace(void){struct timespec t={0,2000000};nanosleep(&t,NULL);}
static void pending_line(struct k7_photo_s *p) {
  pace();printf("PHOTO q=%u e=%u s=%u y=%.3f p=%.3f r=%.3f\n",
    (unsigned)p->gate.pending_frame,p->epoch,p->gate.pending_shot,
    p->pending_raw.yaw,p->pending_raw.pitch,p->pending_raw.roll);
  pace();printf("PHOTOQ q=%u sy=%.3f sp=%.3f sr=%.3f sharp=%.1f size=%.1f\n",
    (unsigned)p->gate.pending_frame,p->pending_filtered.yaw,p->pending_filtered.pitch,
    p->pending_filtered.roll,p->pending_sharp,p->pending_size);
  pace();p->last_print=now_ms();
}
void k7_photo_process(struct k7_photo_s *p,const uint8_t *jpeg,size_t bytes,unsigned q,
                     uint64_t stamp_us,const struct k7_track_result_s *track,unsigned faces,bool halt) {
  if(atomic_exchange(&reset_requested,false))reset(p);
  pthread_mutex_lock(&ack_lock);
  if(ack_valid){if(!p->store)vf_saved(&p->gate,ack_q,ack_epoch,(enum vf_shot)ack_side,ack_ok);ack_valid=false;}
  pthread_mutex_unlock(&ack_lock);
  uint64_t now=now_ms(),stamp=stamp_us/1000;
  unsigned next=!(p->gate.done&VF_FRONT)?VF_FRONT:!(p->gate.done&VF_LEFT)?VF_LEFT:!(p->gate.done&VF_RIGHT)?VF_RIGHT:0;
  if(p->gate.pending) {
    if(now-p->pending_at>5000)vf_saved(&p->gate,p->gate.pending_frame,p->epoch,p->gate.pending_shot,false);
    else if(!halt && now-p->last_print>=500)pending_line(p);
    return;
  }
  struct vf_frame f={.frame_id=q,.captured_ms=stamp,.now_ms=now,.lock_epoch=p->epoch};
  bool face_ok=track->has_face && faces==1 && !halt;
  /* Before the first successful photograph there is no set to mix. Recover
   * automatically when the tracker has reacquired one stable foreground face.
   * Keep this admission's epoch and empty store: changing it would cancel the
   * waiting voice operation as if a different assessment had started. Pending
   * saves returned above; a partly saved set still needs an explicit reset.
   */
  if(p->lost && !p->gate.done && !p->gate.pending &&
     face_ok && track->state==TRACK_ACTIVE) {
    reset_observation(p);
  }
  bool was_lost=p->lost;
  unsigned loss_reason=0;
  if(!face_ok) {
    p->hn=0;p->last_filter=0;
    if(p->locked && !p->missing_since)p->missing_since=now;
    if(faces>1)loss_reason=1;
    else if(p->missing_since && now-p->missing_since>350)loss_reason=2;
    if(loss_reason)p->lost=true;
  } else if(!p->locked) {p->previous=track->box;p->locked=true;}
  else {
    if(overlap(p->previous,track->box)<.30f)loss_reason=3;
    else if(p->missing_since && now-p->missing_since>350)loss_reason=2;
    if(loss_reason)p->lost=true;
  }
  if(!was_lost && p->lost) {
    atomic_store(&retry_required,true);
    printf("PHOTO LOCK_LOST q=%u e=%u reason=%u faces=%u has_face=%u gap_ms=%llu done=%u\n",
      q,p->epoch,loss_reason,faces,track->has_face,
      (unsigned long long)(p->missing_since?now-p->missing_since:0),p->gate.done);
  }
  if(face_ok && !p->lost){p->previous=track->box;p->missing_since=0;}
  f.face_locked=face_ok&&!p->lost;f.ambiguous=p->lost;
  f.gimbal_settled=p->have_target&&abs(track->x-p->last_x)<=2&&abs(track->y-p->last_y)<=2;
  p->last_x=track->x;p->last_y=track->y;p->have_target=true;
  /* Give movement corrections priority. Pose is useful when framing settles. */
  if(f.face_locked && next && f.gimbal_settled) {
    struct k7_jpeg_result_s decoded;
    float angles[3];
    if(k7_jpeg_decode(jpeg,bytes,1,p->rgb,640*480*3,&decoded)==0 && decoded.width==640 && decoded.height==480) {
      f.sharpness=prepare(p,track->box);f.face_size=fminf(track->box.width,track->box.height)*4;
      if(k7_pose_infer(p->pose,p->input,angles)==0) {
        f.raw_pose=(struct vf_pose){angles[0],angles[1],angles[2],true};
        f.filtered_pose=filter(p,f.raw_pose,stamp);
      }
    }
  }
  f.now_ms=now_ms();
  /* Enforce front -> left -> right, based on observed yaw, never servo angle. */
  float desired=next==VF_FRONT?0:
    config.side_angle*config.left_yaw_sign*(next==VF_LEFT?1:-1);
  float tolerance=next==VF_FRONT?config.tolerance:config.side_tolerance;
  if(next && (!f.raw_pose.valid||fabsf(f.raw_pose.yaw-desired)>tolerance))f.filtered_pose.valid=false;
  struct vf_result result=vf_step(&p->gate,&f);
  pace();printf("POSE q=%u e=%u y=%.2f p=%.2f r=%.2f v=%u st=%u next=%u done=%u\n",
    q,p->epoch,f.raw_pose.yaw,f.raw_pose.pitch,f.raw_pose.roll,
    f.raw_pose.valid,result.reason,next,p->gate.done);
  pace();printf("POSEQ q=%u sy=%.2f sharp=%.1f settle=%u lost=%u age=%u\n",
    q,f.filtered_pose.yaw,f.sharpness,f.gimbal_settled,p->lost,(unsigned)(f.now_ms-stamp));
  pace();
  if(result.shot) {
    p->pending_raw=f.raw_pose;p->pending_filtered=f.filtered_pose;
    p->pending_sharp=f.sharpness;p->pending_size=f.face_size;
    p->pending_at=now_ms();
    if(p->store) {
      struct k7_photo_record r={.epoch=p->epoch,.sequence=q,.side=result.shot,.bytes=bytes};
      int stored=k7_photo_store_put(p->store,&r,jpeg);
      vf_saved(&p->gate,q,p->epoch,result.shot,stored==0);
      printf("PHOTO RAM q=%u e=%u s=%u bytes=%lu result=%d complete=%d durable=0\n",
        q,p->epoch,(unsigned)result.shot,(unsigned long)bytes,stored,
        k7_photo_store_complete(p->store,p->epoch));
    } else pending_line(p);
  }
}
