#ifndef VV_REPORT_CHUNKS_H
#define VV_REPORT_CHUNKS_H
#include "vv_tts_segments.h"
#include "vv_https_client.h"
#include <errno.h>
#include <string.h>

/* Report-only delivery. Each callback must finish playback synchronously.
 * Never admit partial network audio or replay a successfully played chunk. */
struct vv_report_chunk_guard {
 const struct vv_https_cancel *outer;
 uint64_t (*now)(void *); void *clock_arg;
 uint64_t began,last; bool expired;
};
static bool vv_report_chunk_cancel(void *arg) {
 struct vv_report_chunk_guard *g=arg;
 if(g->outer&&g->outer->is_cancelled&&g->outer->is_cancelled(g->outer->arg))return true;
 uint64_t now=g->now(g->clock_arg);
 if(now==UINT64_MAX||now<g->last||now-g->began>=120000)g->expired=true;
 g->last=now;return g->expired;
}
static int vv_report_chunks_run(const char *text,
 int (*speak)(void *,const char *,const struct vv_https_cancel *),void *arg,
 uint64_t (*now)(void *),void *clock_arg,const struct vv_https_cancel *cancel,
 size_t *played) {
 if(!text||!speak||!now||!played)return -EINVAL;
 *played=0;
 struct vv_tts_segments cursor;
 if(vv_tts_segments_init(&cursor,text,strlen(text)))return -EPROTO;
 uint64_t start=now(clock_arg);if(start==UINT64_MAX)return -ETIMEDOUT;
 struct vv_report_chunk_guard guard={cancel,now,clock_arg,start,start,false};
 struct vv_https_cancel token={vv_report_chunk_cancel,&guard};
 char chunk[16*4+1];int rc=0;
 for(;;){
  if(vv_report_chunk_cancel(&guard)){rc=guard.expired?-ETIMEDOUT:-ECANCELED;break;}
  size_t bytes=0;
  int next=vv_tts_segments_next_limit(&cursor,chunk,sizeof(chunk),&bytes,16);
  if(next==VV_TTS_SEGMENT_DONE)break;
  if(next!=VV_TTS_SEGMENT_READY){rc=-EPROTO;break;}
  rc=speak(arg,chunk,&token);
  if(vv_report_chunk_cancel(&guard))rc=guard.expired?-ETIMEDOUT:-ECANCELED;
  if(rc)break;
  ++*played;
 }
 vv_https_secure_zero(chunk,sizeof(chunk));return rc;
}
#endif
