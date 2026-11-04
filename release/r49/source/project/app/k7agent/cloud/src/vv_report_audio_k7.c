#include "vv_report_audio_k7.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
#include "vv_session_pcm.h"
#endif
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) &&     defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
#include "vv_paraformer_k7.h"
#endif
#include "k7sound_api.h"
#include <errno.h>
#include <string.h>
static bool ready(struct vv_report_audio_k7 *a) {
  return a && a->client && a->ready && a->ready(a->ready_arg);
}
static int tts(void *arg,const char *text,unsigned char *pcm,size_t cap,size_t *size,
  const struct vv_https_cancel *cancel) {
  struct vv_report_audio_k7 *a=arg;
  if (!ready(a)) {if(size)*size=0;return -ENETDOWN;}
  return vv_native_client_tts(a->client,text,pcm,cap,size,cancel);
}
static int fresh(void *arg,const char *task,const char *report,
  const struct vv_https_cancel *cancel) {
  struct vv_report_audio_k7 *a=arg;
  if (!ready(a)) return -ENETDOWN;
  struct vv_backend_task result={0};
  int rc=vv_backend_client_task(vv_native_client_backend(a->client),task,&result,cancel);
  if (!rc && (result.status!=VV_TASK_REPORT_READY || strcmp(result.task_id,task) ||
      strcmp(result.report_id,report))) rc=VV_BACKEND_CLIENT_REPLACED;
  vv_https_secure_zero(&result,sizeof(result));return rc;
}
struct playback {struct vv_report_audio_k7 *adapter;const struct vv_https_cancel *cancel;};
static int cancelled(void *arg) {
  struct playback *p=arg;
  return (p->cancel && p->cancel->is_cancelled && p->cancel->is_cancelled(p->cancel->arg)) ||
    !ready(p->adapter);
}
#if defined(CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM) || defined(CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION)
static bool stream_cancelled(void *arg){return cancelled(arg)!=0;}
#endif
static int play(void *arg,const unsigned char *pcm,size_t bytes,
  const struct vv_https_cancel *cancel) {
  struct playback p={arg,cancel};
  if (!pcm || !bytes || bytes>960000 || bytes%2 || (uintptr_t)pcm%_Alignof(int16_t))
    return -EINVAL;
  if (cancelled(&p)) return -ECANCELED;
#ifdef CONFIG_EXAMPLES_K7CLOUD_VOICE_SESSION
  struct vv_https_cancel token={stream_cancelled,&p};
  return vv_session_pcm_play(pcm,bytes,&token);
#else
#if defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_ADC_OVERLAP) &&     defined(CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW)
 int cleanup=vv_paraformer_k7_close();if(cleanup)return cleanup;
#endif
  return k7sound_speak_mono16_cancel((const int16_t *)pcm,(unsigned)(bytes/2),cancelled,&p);
#endif
}
#ifdef CONFIG_EXAMPLES_K7CLOUD_TTS_STREAM
static int speak(void *arg,const char *text,const struct vv_https_cancel *cancel){
 struct vv_report_audio_k7 *a=arg;
 if(!ready(a))return -ENETDOWN;
 struct playback p={arg,cancel};struct vv_https_cancel token={stream_cancelled,&p};
 /* Keep the original external mode/report cancellation and network readiness. */
 return vv_native_client_speak(a->client,text,&token);
}
const struct vv_report_audio_ops vv_report_audio_k7_ops={tts,fresh,play,speak};
#else
const struct vv_report_audio_ops vv_report_audio_k7_ops={tts,fresh,play,NULL};
#endif
