#include "k7_photo_store.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct k7_photo_store {
  pthread_mutex_t lock;
  size_t capacity;
  uint32_t epoch;
  unsigned count;
  struct k7_photo_record records[3];
  unsigned char *data;
};
static int index_of(unsigned side)
{return side==1?0:side==2?1:side==4?2:-1;}

struct k7_photo_store *k7_photo_store_create(size_t capacity)
{
  /* Three exact images, at most 3 MiB total; caller chooses camera limit. */
  if(capacity<4 || capacity>1024*1024)return NULL;
  struct k7_photo_store *s=calloc(1,sizeof(*s));
  if(!s)return NULL;
  s->data=malloc(3*capacity);
  if(!s->data){free(s);return NULL;}
  if(pthread_mutex_init(&s->lock,NULL)){free(s->data);free(s);return NULL;}
  s->capacity=capacity;return s;
}
void k7_photo_store_destroy(struct k7_photo_store *s)
{
  /* Owner must join all producers/uploaders before destruction. */
  if(!s)return;
  memset(s->data,0,3*s->capacity);
  free(s->data);pthread_mutex_destroy(&s->lock);free(s);
}
int k7_photo_store_begin(struct k7_photo_store *s,uint32_t epoch)
{
  if(!s||!epoch)return -EINVAL;
  pthread_mutex_lock(&s->lock);
  int rc=epoch<=s->epoch?-ESTALE:0;
  if(!rc)
    {
      memset(s->data,0,3*s->capacity);
      memset(s->records,0,sizeof(s->records));
      s->epoch=epoch;s->count=0;
    }
  pthread_mutex_unlock(&s->lock);return rc;
}
int k7_photo_store_put(struct k7_photo_store *s,const struct k7_photo_record *r,const void *data)
{
  if(!s||!r||!data||index_of(r->side)<0||r->bytes<4)return -EINVAL;
  if(r->bytes>s->capacity)return -EMSGSIZE;
  const unsigned char *b=data;
  /* Producer must additionally have decoded/quality-checked this exact JPEG. */
  if(b[0]!=0xff||b[1]!=0xd8||b[r->bytes-2]!=0xff||b[r->bytes-1]!=0xd9)return -EINVAL;
  pthread_mutex_lock(&s->lock);
  int idx=index_of(r->side),rc=0;
  if(!s->epoch||r->epoch!=s->epoch)rc=-ESTALE;
  else if((unsigned)idx<s->count)
    {
      const struct k7_photo_record *old=&s->records[idx];
      if(old->sequence!=r->sequence || old->bytes!=r->bytes ||
         memcmp(s->data+idx*s->capacity,data,r->bytes))rc=-EEXIST;
    }
  else if((unsigned)idx!=s->count)rc=-EINVAL;
  else if(idx && r->sequence<=s->records[idx-1].sequence)rc=-ESTALE;
  else
    {
      memcpy(s->data+idx*s->capacity,data,r->bytes);
      s->records[idx]=*r;s->count++;
    }
  pthread_mutex_unlock(&s->lock);return rc;
}
int k7_photo_store_copy(struct k7_photo_store *s,uint32_t epoch,unsigned side,
                       struct k7_photo_record *r,void *data,size_t capacity)
{
  int idx=index_of(side);
  if(!s||!r||!data||idx<0)return -EINVAL;
  pthread_mutex_lock(&s->lock);
  int rc=0;
  if(!s->epoch||s->epoch!=epoch)rc=-ESTALE;
  else if((unsigned)idx>=s->count)rc=-ENOENT;
  else if(capacity<s->records[idx].bytes)rc=-EMSGSIZE;
  else{memcpy(data,s->data+idx*s->capacity,s->records[idx].bytes);*r=s->records[idx];}
  pthread_mutex_unlock(&s->lock);return rc;
}
int k7_photo_store_discard(struct k7_photo_store *s,uint32_t epoch)
{
  if(!s||!epoch)return -EINVAL;
  pthread_mutex_lock(&s->lock);
  int rc=s->epoch!=epoch?-ESTALE:0;
  if(!rc){memset(s->data,0,3*s->capacity);memset(s->records,0,sizeof(s->records));s->count=0;}
  pthread_mutex_unlock(&s->lock);return rc;
}
int k7_photo_store_complete(struct k7_photo_store *s,uint32_t epoch)
{
  if(!s)return -EINVAL;
  pthread_mutex_lock(&s->lock);
  int rc=!s->epoch||epoch!=s->epoch?-ESTALE:s->count==3?1:0;
  pthread_mutex_unlock(&s->lock);return rc;
}
int k7_photo_store_status(struct k7_photo_store *s,uint32_t *epoch,unsigned *done)
{
  if(!s||!epoch||!done)return -EINVAL;
  pthread_mutex_lock(&s->lock);
  *epoch=s->epoch;*done=(1u<<s->count)-1;
  pthread_mutex_unlock(&s->lock);return 0;
}
