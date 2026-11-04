#include "vv_report_audio.h"
#include "vv_tts_segments.h"
#include <string.h>
static bool stop(const struct vv_https_cancel *c){return c && c->is_cancelled && c->is_cancelled(c->arg);}
static bool identity(const char *s){
  if(!s || strlen(s)!=36)return false;
  for(unsigned i=0;i<36;++i){
    if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}
    else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F')))return false;
  }
  return true;
}
void vv_report_audio_init(struct vv_report_audio *o){if(o){memset(o,0,sizeof(*o));atomic_init(&o->busy,false);}}
int vv_report_audio_run(struct vv_report_audio *o,const char *text,size_t bytes,
  const char *task,const char *report,unsigned char *pcm,size_t cap,
  const struct vv_https_cancel *cancel,struct vv_report_audio_result *result){
  if(!o||!o->ops||!o->ops->tts||!o->ops->fresh||!o->ops->play||!text||
     !bytes||bytes>16384||!identity(task)||!identity(report)||!pcm||!cap||cap>960000||!result)return VV_REPORT_AUDIO_INVALID;
  if(atomic_exchange(&o->busy,true))return VV_REPORT_AUDIO_BUSY;
  memset(result,0,sizeof(*result));vv_https_secure_zero(pcm,cap);
  int rc=VV_REPORT_AUDIO_OK;struct vv_tts_segments cursor;
  char segment[VV_TTS_SEGMENT_BYTES+1];memset(segment,0,sizeof(segment));
  if(stop(cancel)){rc=VV_REPORT_AUDIO_CANCELLED;goto done;}
  if(vv_tts_segments_init(&cursor,text,bytes)){rc=VV_REPORT_AUDIO_TEXT;goto done;}
  for(;;){
    if(stop(cancel)){rc=VV_REPORT_AUDIO_CANCELLED;break;}
    size_t written=0,audio=0;
    /* Smaller stream segments reduce first-audio delay and exposure to the
     * per-stream 30s cap; duration still depends on synthesis and is bounded. */
    int next=o->ops->speak?vv_tts_segments_next_limit(&cursor,segment,sizeof(segment),&written,80):
      vv_tts_segments_next(&cursor,segment,sizeof(segment),&written);
    if(next==VV_TTS_SEGMENT_DONE)break;
    if(next!=VV_TTS_SEGMENT_READY){rc=VV_REPORT_AUDIO_TEXT;break;}
    vv_https_secure_zero(pcm,cap);
    if(o->ops->speak){
      int call=o->ops->fresh(o->arg,task,report,cancel);result->callback_error=call;
      if(stop(cancel)){rc=VV_REPORT_AUDIO_CANCELLED;break;}
      if(call){rc=VV_REPORT_AUDIO_STALE;break;}
      call=o->ops->speak(o->arg,segment,cancel);result->callback_error=call;
      if(stop(cancel)){rc=VV_REPORT_AUDIO_CANCELLED;break;}
      if(call){rc=VV_REPORT_AUDIO_PLAY;break;}
      ++result->segments_synthesized;++result->segments_played;continue;
    }
    int call=o->ops->tts(o->arg,segment,pcm,cap,&audio,cancel);result->callback_error=call;
    if(stop(cancel)){rc=VV_REPORT_AUDIO_CANCELLED;break;}
    if(call){rc=VV_REPORT_AUDIO_TTS;break;}
    if(!audio||audio>cap||audio%2){rc=VV_REPORT_AUDIO_PCM;break;}
    ++result->segments_synthesized;
    call=o->ops->fresh(o->arg,task,report,cancel);result->callback_error=call;
    if(stop(cancel)){rc=VV_REPORT_AUDIO_CANCELLED;break;}
    if(call){rc=VV_REPORT_AUDIO_STALE;break;}
    call=o->ops->play(o->arg,pcm,audio,cancel);result->callback_error=call;
    if(stop(cancel)){rc=VV_REPORT_AUDIO_CANCELLED;break;}
    if(call){rc=VV_REPORT_AUDIO_PLAY;break;}
    ++result->segments_played;
  }
done:
  vv_https_secure_zero(segment,sizeof(segment));vv_https_secure_zero(pcm,cap);
  atomic_store(&o->busy,false);return rc;
}
