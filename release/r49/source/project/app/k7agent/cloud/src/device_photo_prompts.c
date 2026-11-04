#include "device_photo_prompts.h"
#include <errno.h>
int k7_photo_prompt_plan(const struct k7_photo_prompt_state *old,uint32_t epoch,unsigned done,const char *keys[4])
{
  if(!old||!epoch||!keys||(done!=0&&done!=1&&done!=3&&done!=7))return -EINVAL;
  if(epoch<old->epoch)return -ESTALE;
  if(epoch==old->epoch && (done&old->done)!=old->done)return -EIO;
  unsigned before=epoch==old->epoch?old->done:0;
  if(epoch==old->epoch && before==done)return 0;
  const char *saved[]={"front_saved","left_saved","right_saved"};
  int n=0;
  for(unsigned i=0;i<3;i++)if((done&(1u<<i))&&!(before&(1u<<i)))keys[n++]=saved[i];
  keys[n++]=done==7?"capture_retained":done==3?"right_aligning":done==1?"left_aligning":"front_aligning";
  return n;
}
