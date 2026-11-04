/* SPDX-License-Identifier: Apache-2.0 */
#include "k7_video_queue.h"
#include "k7_video_frame.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>
#define BLOCK 16384u
#define CAPACITY (K7_VIDEO_MAX_BYTES+BLOCK)
enum state { FREE, WRITING, PENDING, SENDING };
static uint8_t storage[2][CAPACITY] __attribute__((aligned(64)));
static struct { enum state state; size_t bytes; uint32_t seq; uint64_t stamp; } slots[2];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool accepting;
void k7_video_queue_open(void){atomic_store(&accepting,true);}
void k7_video_queue_close(void){atomic_store(&accepting,false);}
void k7_video_publish(const uint8_t *jpeg,size_t bytes,uint32_t seq,uint64_t stamp)
{
 if(!atomic_load(&accepting) || !jpeg || bytes<4 || bytes>K7_VIDEO_MAX_BYTES)return;
 if(pthread_mutex_trylock(&lock))return;
 int chosen=-1;
 for(int i=0;i<2;i++)if(slots[i].state==FREE){chosen=i;break;}
 if(chosen<0)
  for(int i=0;i<2;i++)if(slots[i].state==PENDING &&
   (chosen<0 || slots[i].seq<slots[chosen].seq))chosen=i;
 if(chosen<0){pthread_mutex_unlock(&lock);return;}
 slots[chosen].state=WRITING;
 pthread_mutex_unlock(&lock);
 memcpy(storage[chosen]+K7_VIDEO_HEADER_BYTES,jpeg,bytes);
 pthread_mutex_lock(&lock);
 slots[chosen].bytes=bytes;slots[chosen].seq=seq;slots[chosen].stamp=stamp;
 slots[chosen].state=atomic_load(&accepting)?PENDING:FREE;
 pthread_mutex_unlock(&lock);
}
int k7_video_queue_take(const uint8_t **wire,size_t *bytes)
{
 if(!wire || !bytes || !atomic_load(&accepting))return -1;
 pthread_mutex_lock(&lock);
 int chosen=-1;
 for(int i=0;i<2;i++)if(slots[i].state==PENDING &&
  (chosen<0 || slots[i].seq>slots[chosen].seq))chosen=i;
 if(chosen<0){pthread_mutex_unlock(&lock);return -1;}
 for(int i=0;i<2;i++)if(i!=chosen && slots[i].state==PENDING)slots[i].state=FREE;
 slots[chosen].state=SENDING;
 pthread_mutex_unlock(&lock);
 uint8_t *data=storage[chosen];
 size_t length=slots[chosen].bytes;
 if(k7_video_header(data,slots[chosen].seq,slots[chosen].stamp,640,480,
                    data+K7_VIDEO_HEADER_BYTES,length))
  {k7_video_queue_release(chosen);return -1;}
 length+=K7_VIDEO_HEADER_BYTES;
 size_t padded=(length+BLOCK-1)/BLOCK*BLOCK;
 memset(data+length,0,padded-length);
 *wire=data;*bytes=padded;
 return chosen;
}
void k7_video_queue_release(int slot)
{
 if(slot<0 || slot>1)return;
 pthread_mutex_lock(&lock);
 if(slots[slot].state==SENDING)slots[slot].state=FREE;
 pthread_mutex_unlock(&lock);
}
