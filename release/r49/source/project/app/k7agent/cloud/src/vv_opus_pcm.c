#include "vv_opus_pcm.h"
#include "vv_ogg_opus.h"
#include <opus.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PCM_LIMIT 2880000u
#define WIRE_LIMIT 2880000u
#define FRAME_SAMPLES 1920u /* Opus maximum120ms, mono16kHz. */
struct vv_opus_pcm {
 struct vv_ogg_opus ogg;
 OpusDecoder *decoder;
 vv_stream_pcm_sink sink;void *sink_arg;
 const struct vv_https_cancel *cancel;
 struct vv_opus_pcm_stats stats;
 opus_int16 decoded[FRAME_SAMPLES];
 unsigned char little[FRAME_SAMPLES*2];
 bool sealed;
 int error;
};
static void wipe(void *data,size_t n){volatile unsigned char*p=data;while(n--)*p++=0;}
static int reject(struct vv_opus_pcm*s,int rc){
 if(!s->error)s->error=rc<0?rc:-EIO;
 return s->error;
}
static int check(struct vv_opus_pcm*s){
 if(s->error)return s->error;
 if(s->cancel&&s->cancel->is_cancelled&&s->cancel->is_cancelled(s->cancel->arg))
  return reject(s,-ECANCELED);
 return 0;
}
static int head(void*arg,const struct vv_ogg_opus_head*h){
 struct vv_opus_pcm*s=arg;int rc=check(s);if(rc)return rc;
 if(s->decoder||h->channels!=1||h->mapping)return reject(s,-EPROTO);
 /* Granules and pre-skip use48kHz even when decoding at16kHz. This first
  * native route requires exact alignment; it never rounds per packet. */
 if(h->pre_skip_48k%3)return reject(s,-ENOTSUP);
 int error=OPUS_OK;s->decoder=opus_decoder_create(16000,1,&error);
 if(!s->decoder||error!=OPUS_OK)return reject(s,error==OPUS_ALLOC_FAIL?-ENOMEM:-EPROTO);
 if(opus_decoder_ctl(s->decoder,OPUS_SET_GAIN((int)h->gain_q8))!=OPUS_OK)
  return reject(s,-EPROTO);
 s->stats.pre_skip_48k=h->pre_skip_48k;
 return check(s);
}
static int packet(void*arg,const uint8_t*data,size_t bytes,const struct vv_ogg_opus_packet*p){
 struct vv_opus_pcm*s=arg;int rc=check(s);if(rc)return rc;
 if(!s->decoder||!data||!bytes||bytes>INT_MAX||!p->duration_48k||
    p->duration_48k>5760||p->skip_48k>p->duration_48k||
    p->keep_48k>p->duration_48k-p->skip_48k)return reject(s,-EPROTO);
 if(p->decoded_start_48k%3||p->duration_48k%3||p->skip_48k%3||p->keep_48k%3)
  return reject(s,-ENOTSUP);
 int expected=(int)(p->duration_48k/3);
 if(opus_packet_get_nb_samples(data,(opus_int32)bytes,16000)!=expected)
  return reject(s,-EPROTO);
 /* Even wholly skipped packets must update the predictive decoder state.
  * Never request PLC/FEC for missing or invalid compressed bytes. */
 int got=opus_decode(s->decoder,data,(opus_int32)bytes,s->decoded,FRAME_SAMPLES,0);
 if(got!=expected)return reject(s,-EPROTO);
 if((rc=check(s)))return rc;
 unsigned skip=p->skip_48k/3,keep=p->keep_48k/3;
 size_t count=(size_t)keep*2;
 if(count>PCM_LIMIT-s->stats.pcm_bytes)return reject(s,-EOVERFLOW);
 for(unsigned i=0;i<keep;i++){
  uint16_t v=(uint16_t)s->decoded[skip+i];
  s->little[i*2]=(unsigned char)v;s->little[i*2+1]=(unsigned char)(v>>8);
 }
 if(count){rc=s->sink(s->sink_arg,s->little,count);if(rc)return reject(s,rc);}
 s->stats.pcm_bytes+=count;++s->stats.packets;s->stats.eos=p->page_eos;
 wipe(s->decoded,sizeof(s->decoded));wipe(s->little,sizeof(s->little));
 return check(s);
}
static int create(vv_stream_pcm_sink sink,void*arg,const struct vv_https_cancel*cancel,
 struct vv_opus_pcm**out,bool qwen){
 if(out)*out=NULL;
 if(!out||!sink)return -EINVAL;
 if(cancel&&cancel->is_cancelled&&cancel->is_cancelled(cancel->arg))return -ECANCELED;
 struct vv_opus_pcm*s=calloc(1,sizeof(*s));if(!s)return -ENOMEM;
 s->sink=sink;s->sink_arg=arg;s->cancel=cancel;
 const struct vv_ogg_opus_callbacks callbacks={head,packet};
 int rc=qwen?vv_ogg_opus_init_qwen_live(&s->ogg,&callbacks,s):vv_ogg_opus_init(&s->ogg,&callbacks,s);
 if(rc){vv_opus_pcm_destroy(s);return rc;}
 *out=s;return 0;
}
int vv_opus_pcm_create(vv_stream_pcm_sink sink,void*arg,const struct vv_https_cancel*cancel,
 struct vv_opus_pcm**out){return create(sink,arg,cancel,out,false);}
int vv_opus_pcm_create_qwen_live(vv_stream_pcm_sink sink,void*arg,const struct vv_https_cancel*cancel,
 struct vv_opus_pcm**out){return create(sink,arg,cancel,out,true);}
int vv_opus_pcm_feed(struct vv_opus_pcm*s,const void*data,size_t bytes){
 if(!s)return -EINVAL;
 int rc=check(s);if(rc)return rc;
 if(s->sealed)return reject(s,-EPROTO);
 if((!data&&bytes)||bytes>WIRE_LIMIT-s->stats.wire_bytes)return reject(s,!data?-EINVAL:-EOVERFLOW);
 rc=vv_ogg_opus_feed(&s->ogg,data,bytes);if(rc)return reject(s,rc);
 s->stats.wire_bytes+=bytes;return check(s);
}
int vv_opus_pcm_finish(struct vv_opus_pcm*s,bool live_task){
 if(!s)return -EINVAL;
 int rc=check(s);if(rc)return rc;
 if(s->sealed)return reject(s,-EPROTO);
 rc=live_task?vv_ogg_opus_finish_live_task(&s->ogg):vv_ogg_opus_finish(&s->ogg);
 if(rc)return reject(s,rc);
 if(!s->stats.pcm_bytes||!s->decoder)return reject(s,-EPROTO);
 s->sealed=true;return check(s);
}
void vv_opus_pcm_get_stats(const struct vv_opus_pcm*s,struct vv_opus_pcm_stats*out){
 if(out){if(s){*out=s->stats;out->granule_mismatches=s->ogg.granule_mismatches;}
  else memset(out,0,sizeof(*out));}
}
void vv_opus_pcm_destroy(struct vv_opus_pcm*s){
 if(!s)return;
 if(s->decoder)opus_decoder_destroy(s->decoder);
 wipe(s,sizeof(*s));free(s);
}
