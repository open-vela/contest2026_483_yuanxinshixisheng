#include "vv_speech_owner.h"
/* Explicit three-second diagnostic; consumes live PCM without sending it. */
#include "capture_stream.h"
#include "stream_ring.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct probe {
  struct k7_stream_ring ring;
  int done;
  int result;
};

static void *capture_worker(void *arg)
{
  struct probe *p=arg;
  p->result=k7sound_capture_stream(48000,k7_stream_ring_sample,&p->ring);
  __atomic_store_n(&p->done,1,__ATOMIC_RELEASE);
  return NULL;
}

static int capture_stream_probe(void)
{
  struct probe *p=calloc(1,sizeof(*p));
  pthread_attr_t attr;
  pthread_t worker;
  uint8_t pcm[K7_STREAM_BYTES];
  uint32_t seq=0,received=0,nonzero=0;
  int rc;
  if(!p)return -ENOMEM;
  rc=pthread_attr_init(&attr);
  if(rc){free(p);return -rc;}
  rc=pthread_attr_setstacksize(&attr,32768);
  if(!rc)rc=pthread_create(&worker,&attr,capture_worker,p);
  pthread_attr_destroy(&attr);
  if(rc){free(p);return -rc;}
  for(;;)
    {
      rc=k7_stream_ring_take(&p->ring,pcm,&seq);
      if(rc<0)break;
      if(rc==1)
        {
          unsigned i;
          if(seq!=received){rc=-EIO;break;}
          received++;
          for(i=0;i<K7_STREAM_BYTES;i++)if(pcm[i])nonzero++;
        }
      else if(__atomic_load_n(&p->done,__ATOMIC_ACQUIRE))
        {
          /* Recheck after observing completion: last frame precedes done. */
          if(__atomic_load_n(&p->ring.produced,__ATOMIC_ACQUIRE)!=
             __atomic_load_n(&p->ring.consumed,__ATOMIC_RELAXED))continue;
          break;
        }
      else usleep(1000);
    }
  if(rc<0)k7_stream_ring_cancel(&p->ring);
  int joined=pthread_join(worker,NULL);
  if(joined){vv_speech_owner_legacy_poison();return -joined;}
  if(!rc)rc=p->result;
  if(!rc && received!=150)rc=-EIO;
  printf("K7CLOUD capture_stream result=%d pcm_frames=%lu nonzero_bytes=%lu network_sent=0\n",
         rc,(unsigned long)received,(unsigned long)nonzero);
  free(p);
  return rc;
}

int k7cloud_capture_stream_probe(void){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=capture_stream_probe();vv_speech_owner_legacy_leave();return rc;
}
