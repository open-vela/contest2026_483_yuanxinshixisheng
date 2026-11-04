/* SPDX-License-Identifier: Apache-2.0 */
/* Single producer, latest-only preview. Never hold lock while transmitting. */
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include "k7_preview.h"
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define RGB_BYTES (160 * 120 * 3)
struct k7_preview_queue_s
{
  pthread_mutex_t lock;
  pthread_cond_t ready;
  pthread_t thread;
  bool pending, stopping;
  unsigned int sequence;
  uint8_t buffers[2][RGB_BYTES];
  uint8_t *next, *active;
};
static void *preview_worker(void *arg)
{
  struct k7_preview_queue_s *q=arg;
  for (;;)
    {
      pthread_mutex_lock(&q->lock);
      while (!q->pending && !q->stopping)
        pthread_cond_wait(&q->ready,&q->lock);
      if (q->stopping)
        { pthread_mutex_unlock(&q->lock); break; }
      uint8_t *swap=q->active; q->active=q->next; q->next=swap;
      unsigned int sequence=q->sequence;
      q->pending=false;
      pthread_mutex_unlock(&q->lock);
      /* No pipeline or camera storage is referenced by this thread. */
      k7_preview_emit(q->active,RGB_BYTES,sequence);
    }
  return NULL;
}
int k7_preview_queue_start(struct k7_preview_queue_s **out)
{
  *out=NULL;
  struct k7_preview_queue_s *q=calloc(1,sizeof(*q));
  if (!q) return -ENOMEM;
  q->next=q->buffers[0]; q->active=q->buffers[1];
  int ret=pthread_mutex_init(&q->lock,NULL);
  if (ret) goto release;
  ret=pthread_cond_init(&q->ready,NULL);
  if (ret) goto mutex;
  pthread_attr_t attr;
  ret=pthread_attr_init(&attr);
  if (ret) goto cond;
  ret=pthread_attr_setstacksize(&attr,16384);
#ifdef __NuttX__
  struct sched_param param={.sched_priority=80};
  if (!ret) ret=pthread_attr_setschedpolicy(&attr,SCHED_FIFO);
  if (!ret) ret=pthread_attr_setschedparam(&attr,&param);
  if (!ret) ret=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
#endif
  if (!ret) ret=pthread_create(&q->thread,&attr,preview_worker,q);
  pthread_attr_destroy(&attr);
  if (ret) goto cond;
  *out=q;
  return 0;
cond: pthread_cond_destroy(&q->ready);
mutex: pthread_mutex_destroy(&q->lock);
release: free(q); return -ret;
}
void k7_preview_queue_submit(struct k7_preview_queue_s *q,
                             const uint8_t *rgb,size_t bytes,unsigned int seq)
{
  if (!q || !rgb || bytes!=RGB_BYTES) return;
  pthread_mutex_lock(&q->lock);
  if (!q->stopping)
    {
      /* Overwrite only pending storage; active output remains immutable. */
      memcpy(q->next,rgb,bytes);
      q->sequence=seq; q->pending=true;
      pthread_cond_signal(&q->ready);
    }
  pthread_mutex_unlock(&q->lock);
}
/* Caller must have joined the sole producer before stopping. */
int k7_preview_queue_stop(struct k7_preview_queue_s *q)
{
  if (!q) return 0;
  pthread_mutex_lock(&q->lock);
  q->stopping=true;
  pthread_cond_signal(&q->ready);
  pthread_mutex_unlock(&q->lock);
  int ret=pthread_join(q->thread,NULL);
  if (ret) return -ret; /* Retain live-thread storage on join failure. */
  pthread_cond_destroy(&q->ready);
  pthread_mutex_destroy(&q->lock);
  free(q);
  return 0;
}
