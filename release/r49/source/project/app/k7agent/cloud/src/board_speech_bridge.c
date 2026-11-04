#include "vv_speech_owner.h"
/* Explicit LAN speech test commands. No flash, reboot or Wi-Fi operations. */
#include "capture_stream.h"
#include "stream_ring.h"
#include "device_voice_intent.h"
#include "device_photo_prompts.h"
#include "command_endpoint.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#include <sched.h>
#endif

int k7sound_speak_mono16(const int16_t *, unsigned, unsigned);
int k7sound_speak_mono16_cancel(const int16_t *,unsigned,int (*)(void *),void *);
#include "board_voice_prompts.inc"
#include "board_fixed_pcm.inc"
static pthread_mutex_t tts_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool tts_cancelled;
static int tts_busy,tts_fd=-1,tts_result;
static pthread_mutex_t audio_owner=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool device_busy;
static atomic_bool device_loop_busy;
static atomic_uint device_epoch;
static atomic_bool photo_prompt_pending;
/* Nonzero only for an explicitly admitted assessment. STOP invalidates it
 * without deleting the retained RAM photos or terminating voice listening. */
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
static atomic_uint assessment_serial;
#endif
static atomic_uint assessment_live;
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
int k7_pipeline_voice_mode(unsigned int);
int k7_pipeline_voice_mode_status(unsigned int);
int k7_pipeline_hold_revision(unsigned int *);
int k7_pipeline_resume_tracking(unsigned int);
void k7_pipeline_halt(void);
int k7_photo_native_status(uint32_t *,unsigned *);
#include "../../../k7host/k7_photo_store.h"
int k7_photo_native_copy(uint32_t,unsigned,struct k7_photo_record *,void *,size_t);
#endif
extern uint64_t k7sound_clock_us(void);
static int playback_cancelled(void *arg) {(void)arg;return atomic_load(&tts_cancelled);}
int k7cloud_tts_control(int stop)
{
  pthread_mutex_lock(&tts_lock);
  if(stop)
    {
      atomic_store(&tts_cancelled,1);
      if(tts_fd>=0)shutdown(tts_fd,SHUT_RDWR);
      puts("K7CLOUD tts_stop requested=1");
    }
  else printf("K7CLOUD tts_state busy=%d result=%d cancelled=%d\n",tts_busy,tts_result,tts_result==-ECANCELED);
  pthread_mutex_unlock(&tts_lock);
  return 0;
}

static int transfer(int fd, void *buffer, size_t bytes, int writing)
{
  unsigned char *p=buffer;
  while(bytes)
    {
      ssize_t n=writing?send(fd,p,bytes,0):recv(fd,p,bytes,0);
      if(n<0 && errno==EINTR)continue;
      if(n<0)return -errno;
      if(!n)return -ECONNRESET;
      p+=n;bytes-=(size_t)n;
    }
  return 0;
}

static int send_packet(int fd, unsigned char type, void *data, uint32_t size)
{
  unsigned char packet[649]={type,size>>24,size>>16,size>>8,size};
  if(size>644 || (size && !data))return -EINVAL;
  if(size)memcpy(packet+5,data,size);
  return transfer(fd,packet,size+5,1);
}

static int header(int fd, unsigned char *type, uint32_t *size)
{
  unsigned char h[5];int rc=transfer(fd,h,sizeof(h),0);
  if(rc)return rc;
  *type=h[0];*size=((uint32_t)h[1]<<24)|((uint32_t)h[2]<<16)|((uint32_t)h[3]<<8)|h[4];
  return 0;
}

static int connect_server(const char *ip)
{
  struct sockaddr_in a;struct timeval timeout={15,0};
  memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(8001);
  if(inet_pton(AF_INET,ip,&a.sin_addr)!=1)return -EINVAL;
  int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -errno;
  int flags=fcntl(fd,F_GETFL,0),rc;
  if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);return -EIO;}
  rc=connect(fd,(struct sockaddr *)&a,sizeof(a));
  if(rc<0 && errno==EINPROGRESS)
    {
      struct pollfd p={fd,POLLOUT,0};int error=0;socklen_t n=sizeof(error);
      rc=poll(&p,1,10000);
      if(rc>0)
        {
          if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)<0)rc=-errno;
          else rc=error?-error:0;
        }
      else rc=rc==0?-ETIMEDOUT:-errno;
    }
  else if(rc<0)rc=-errno;
  if(rc){close(fd);return rc;}
  if(fcntl(fd,F_SETFL,flags)<0){rc=-errno;close(fd);return rc;}
  if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0 ||
     setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))<0)
    {close(fd);return -EIO;}
  return fd;
}

/* Diagnostic transport check only: no protocol payload, capture or playback. */
int k7cloud_gateway_probe(const char *ip)
{
  int fd = connect_server(ip);
  int rc = fd < 0 ? fd : 0;
  if (fd >= 0) close(fd);
  printf("K7CLOUD gateway_check=%d tcp_only=1 asr_verified=0 tts_verified=0\n", rc);
  return rc;
}

struct live {
  struct k7_stream_ring ring;
  int fd,done,capture_rc,receive_rc,completed,chat;
  uint64_t completed_us;
  struct k7_voice_result result;
  struct k7_command_endpoint endpoint;
  int capture_reason,stage_priority;
};

static int command_sample(void *arg,uint32_t left,uint32_t right)
{
  struct live *p=arg;
  int rc=k7_stream_ring_sample(&p->ring,left,right);
  if(rc)return rc;
  if(p->chat)return 0;
  k7_endpoint_sample(&p->endpoint,(int16_t)(left>>16));
  if(p->ring.partial_samples)return 0;
  int ended=k7_endpoint_frame(&p->endpoint);
  if(p->stage_priority && atomic_load(&photo_prompt_pending))
    {p->capture_reason=2;return 1;}
  if(ended){p->capture_reason=1;return 1;}
  return 0;
}

static void *record(void *arg)
{
  struct live *p=arg;
  /* Both real commands and chat use the established cue/ADC handoff.
   * The chat flag only selects the wire protocol, never microphone timing. */
  p->capture_rc=k7sound_capture_stream_cued(48000,command_sample,p);
  __atomic_store_n(&p->done,1,__ATOMIC_RELEASE);return NULL;
}

static int json_event(int fd,char *text,size_t cap)
{
  unsigned char kind;uint32_t n;int rc=header(fd,&kind,&n);
  if(rc || kind!='J' || !n || n>=cap)return -EIO;
  rc=transfer(fd,text,n,0);
  if(!rc && memchr(text,0,n))rc=-EINVAL;
  text[n]=0;return rc;
}

static void *receive_events(void *arg)
{
  struct live *p=arg;char text[8193];int rc;
  while(!(rc=json_event(p->fd,text,sizeof(text))))
    {
      rc=k7_voice_event(&p->result,text,strlen(text));
      if(rc)break;
      if(p->result.completed){p->completed_us=k7sound_clock_us();p->completed=1;break;}
    }
  p->receive_rc=rc;
  if(rc){k7_stream_ring_cancel(&p->ring);shutdown(p->fd,SHUT_RDWR);}
  return NULL;
}

static int asr(int fd,enum k7_voice_intent *intent,int chat)
{
  struct live *p=calloc(1,sizeof(*p));pthread_t capture,receiver;
  pthread_attr_t attr;unsigned char audio[644];char ready[256];
  uint32_t seq=0,count=0;int rc,have_rx=0,have_capture=0;
  if(!p)return -ENOMEM;
  p->fd=fd;p->chat=chat;
  p->stage_priority=!chat && atomic_load(&device_loop_busy);
  rc=transfer(fd,(void *)(chat?"K7S1B":"K7S1A"),5,1);
  if(!rc)rc=json_event(fd,ready,sizeof(ready));
  if(!rc && !strstr(ready,"\"type\": \"ready\""))rc=-EIO;
  if(rc){free(p);return rc;}
  rc=pthread_attr_init(&attr);if(rc){free(p);return -rc;}
  rc=pthread_attr_setstacksize(&attr,32768);
  if(!rc){rc=pthread_create(&receiver,&attr,receive_events,p);have_rx=!rc;}
#ifdef CONFIG_SMP
  if(!rc)
    {
      cpu_set_t cpus;
      CPU_ZERO(&cpus);CPU_SET(1,&cpus);
      rc=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);
    }
#endif
  if(!rc){rc=pthread_create(&capture,&attr,record,p);have_capture=!rc;}
  pthread_attr_destroy(&attr);
  if(rc)rc=-rc;
  while(!rc)
    {
      int got=k7_stream_ring_take(&p->ring,audio+4,&seq);
      if(got<0){rc=got;break;}
      if(got)
        {
          if(seq!=count){rc=-EIO;break;}
          audio[0]=seq;audio[1]=seq>>8;audio[2]=seq>>16;audio[3]=seq>>24;
          rc=send_packet(fd,'A',audio,sizeof(audio));count++;
        }
      else if(__atomic_load_n(&p->done,__ATOMIC_ACQUIRE))
        {
          if(__atomic_load_n(&p->ring.produced,__ATOMIC_ACQUIRE)!=
             __atomic_load_n(&p->ring.consumed,__ATOMIC_RELAXED))continue;
          break;
        }
      else usleep(1000);
    }
  if(rc)k7_stream_ring_cancel(&p->ring);
  if(have_capture){int joined=pthread_join(capture,NULL);if(joined){k7_stream_ring_cancel(&p->ring);shutdown(fd,SHUT_RDWR);vv_speech_owner_legacy_poison();return -joined;}}
  if(!rc)rc=p->capture_rc;
  if(!rc && p->capture_reason==2)rc=-EINTR;
  if(!rc && (count>150 || !count || (!p->capture_reason && count!=150)))rc=-EIO;
  if(!rc)rc=send_packet(fd,'E',NULL,0);
  if(rc)shutdown(fd,SHUT_RDWR);
  if(have_rx){int joined=pthread_join(receiver,NULL);if(joined){k7_stream_ring_cancel(&p->ring);shutdown(fd,SHUT_RDWR);vv_speech_owner_legacy_poison();return -joined;}}
  if(!rc)rc=p->receive_rc;
  if(!rc && !p->completed)rc=-EIO;
  printf("K7CLOUD asr_capture result=%d produced=%lu consumed=%lu slots=%u receive=%d\n",
         p->capture_rc,(unsigned long)p->ring.produced,
         (unsigned long)p->ring.consumed,K7_STREAM_SLOTS,p->receive_rc);
  printf("K7CLOUD asr result=%d frames_sent=%lu completed=%d\n",rc,(unsigned long)count,p->completed);
  printf("K7CLOUD timing asr_completed_us=%" PRIu64 " result=%d\n",p->completed_us,rc);
  printf("K7CLOUD endpoint reason=%d captured_ms=%u last_active_ms=%u voiced_ms=%u\n",
         p->capture_reason,count*20,p->endpoint.last_active*20,p->endpoint.voiced*20);
  if(!rc && intent)*intent=k7_voice_result_intent(&p->result);
  free(p);return rc;
}

static int tts(int fd, const char *phrase, int prompt)
{
  unsigned char kind;uint32_t size=0;
  int rc=transfer(fd,(void *)(prompt==2?"K7S1C":prompt?"K7S1P":"K7S1T"),5,1);void *pcm=NULL;
  if(!rc)rc=send_packet(fd,'T',(void *)phrase,strlen(phrase));
  if(!rc)rc=header(fd,&kind,&size);
  unsigned waits=0;
  while(!rc && prompt==2 && kind=='W' && !size && waits++<20)
    rc=header(fd,&kind,&size);
  if(!rc && prompt==2 && kind=='N' && !size)return 0;
  if(!rc && (kind!='D'||!size||size>960000||size%2))rc=-EINVAL;
  if(!rc){pcm=malloc(size);if(!pcm)rc=-ENOMEM;}
  if(!rc)rc=transfer(fd,pcm,size,0);
  if(atomic_load(&tts_cancelled))rc=-ECANCELED;
  if(!rc)rc=k7sound_speak_mono16_cancel(pcm,size/2,playback_cancelled,NULL);
  printf("K7CLOUD tts result=%d pcm_bytes=%lu\n",rc,(unsigned long)size);
  free(pcm);return rc;
}

static int board_speech(const char *mode,const char *ip,const unsigned *epoch)
{
  const char *phrase = NULL;
  if (!strcmp(mode,"tts-test")) phrase="联网成功";
  else if (!strcmp(mode,"chat-reply")) phrase="reply";
  else if (strcmp(mode,"asr-live"))
    {
      for (unsigned int i=0;i<sizeof(k7_voice_prompts)/sizeof(k7_voice_prompts[0]);i++)
        if (!strcmp(mode,k7_voice_prompts[i].key)) { phrase=k7_voice_prompts[i].text; break; }
      if (!phrase) { puts("K7CLOUD prompt rejected=unknown_key"); return -EINVAL; }
    }
  if(!strcmp(mode,"asr-live"))
    {
      if(atomic_load(&device_busy))return -EBUSY;
      if(pthread_mutex_trylock(&audio_owner))return -EBUSY;
      int fd=connect_server(ip),rc=fd;
      if(fd>=0){rc=asr(fd,NULL,0);close(fd);}
      pthread_mutex_unlock(&audio_owner);return rc;
    }
  if(pthread_mutex_trylock(&audio_owner))return -EBUSY;
  pthread_mutex_lock(&tts_lock);
  if(epoch && *epoch!=atomic_load(&device_epoch))
    {pthread_mutex_unlock(&tts_lock);pthread_mutex_unlock(&audio_owner);return -ECANCELED;}
  if(tts_busy){pthread_mutex_unlock(&tts_lock);pthread_mutex_unlock(&audio_owner);puts("K7CLOUD prompt_done result=-16");return -EBUSY;}
  tts_busy=1;tts_result=0;atomic_store(&tts_cancelled,0);
  printf("K7CLOUD prompt_started busy=1 started_us=%" PRIu64 "\n",k7sound_clock_us());
  pthread_mutex_unlock(&tts_lock);
  const struct k7_fixed_prompt *fixed=NULL;
  for(unsigned i=0;i<sizeof(k7_fixed_prompts)/sizeof(k7_fixed_prompts[0]);i++)
    if(!strcmp(mode,k7_fixed_prompts[i].key)){fixed=&k7_fixed_prompts[i];break;}
  int fd=-1,rc=0;
  if(fixed)
    {
      uint64_t started=k7sound_clock_us();
      rc=atomic_load(&tts_cancelled)?-ECANCELED:
        k7sound_speak_mono16_cancel((const int16_t *)(k7_fixed_pcm+fixed->offset),
                                  fixed->bytes/2,playback_cancelled,NULL);
      printf("K7CLOUD local_prompt key=%s bytes=%u result=%d elapsed_us=%" PRIu64 "\n",
             fixed->key,fixed->bytes,rc,k7sound_clock_us()-started);
    }
  else if((fd=connect_server(ip))>=0)
    {
      pthread_mutex_lock(&tts_lock);tts_fd=fd;
      int cancelled=atomic_load(&tts_cancelled);
      pthread_mutex_unlock(&tts_lock);
      rc=cancelled?-ECANCELED:tts(fd,phrase,!strcmp(mode,"chat-reply")?2:strcmp(mode,"tts-test")!=0);
    }
  else rc=atomic_load(&tts_cancelled)?-ECANCELED:fd;
  pthread_mutex_lock(&tts_lock);
  tts_fd=-1;if(fd>=0)close(fd);
  tts_busy=0;tts_result=rc;
  printf("K7CLOUD prompt_done result=%d\n",rc);
  pthread_mutex_unlock(&tts_lock);
  pthread_mutex_unlock(&audio_owner);
  return rc;
}
static int legacy_board_speech(const char *mode,const char *ip)
{return board_speech(mode,ip,NULL);}

int k7cloud_device_stop(void)
{
  /* Shared with mode dispatch and prompt admission: a late final or prompt
   * cannot undo a stop. An in-flight capture exits before releasing its buffers. */
  pthread_mutex_lock(&tts_lock);
  atomic_fetch_add(&device_epoch,1);
  atomic_store(&assessment_live,0);
  atomic_store(&tts_cancelled,true);
  if(tts_fd>=0)shutdown(tts_fd,SHUT_RDWR);
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
  k7_pipeline_halt();
#endif
  pthread_mutex_unlock(&tts_lock);
  puts("K7CLOUD device stop_requested=1");return 0;
}

#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
/* 0 accepted, 1 policy-rejected (keep listening), negative real failure.
 * Serialize command admission with explicit service stop and report resume. */
static int device_mode_request(enum k7_voice_intent intent,unsigned epoch,unsigned *mode)
{
  int rc=0;
  pthread_mutex_lock(&tts_lock);
  if(epoch!=atomic_load(&device_epoch)){rc=-ECANCELED;goto out;}
  if(intent==K7_VOICE_STOP)
    {
      atomic_store(&assessment_live,0);
      atomic_store(&tts_cancelled,true);
      if(tts_fd>=0)shutdown(tts_fd,SHUT_RDWR);
      k7_pipeline_halt();
      *mode=0;
      goto out;
    }
  if(intent!=K7_VOICE_TRACK && intent!=K7_VOICE_PHOTO){rc=1;goto out;}
  if(atomic_load(&assessment_live))
    {puts("K7CLOUD command ignored=assessment_active");rc=1;goto out;}
  if(intent==K7_VOICE_PHOTO)
    {
      rc=k7_pipeline_voice_mode_status(1);
      if(rc==-ECANCELED || rc==-EAGAIN)
        {puts("K7CLOUD command ignored=wake_tracking_first");rc=1;}
      if(rc)goto out;
    }
  *mode=intent==K7_VOICE_TRACK?1:2;
  rc=k7_pipeline_voice_mode(*mode);
  if(!rc && *mode==2)
    {
      unsigned serial=atomic_fetch_add(&assessment_serial,1)+1;
      if(!serial)serial=atomic_fetch_add(&assessment_serial,1)+1;
      atomic_store(&assessment_live,serial);
    }
out:
  pthread_mutex_unlock(&tts_lock);
  return rc;
}
#endif

/* One explicit recording round. Camera must already be in the held voicecam
 * session; this entry never opens UVC or assumes a calibrated MCU position. */
static int device_round(const char *ip,unsigned epoch)
{
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
  bool expected=false;
  if(!atomic_compare_exchange_strong(&device_busy,&expected,true))return -EBUSY;
  enum k7_voice_intent intent=K7_VOICE_NONE;
  int rc=-EBUSY;
  if(epoch!=atomic_load(&device_epoch)){rc=-ECANCELED;goto done;}
  if(pthread_mutex_trylock(&audio_owner))goto done;
  int fd=connect_server(ip);rc=fd;
  if(fd>=0){rc=asr(fd,&intent,0);close(fd);}
  pthread_mutex_unlock(&audio_owner);
  if(rc==-EINTR && atomic_load(&photo_prompt_pending)){rc=0;goto done;}
  if(rc)goto done;
  if(epoch!=atomic_load(&device_epoch)){rc=-ECANCELED;goto done;}
  printf("K7CLOUD device intent=%u\n",(unsigned)intent);
  if(intent==K7_VOICE_NONE){rc=0;goto done;}
  if(intent==K7_VOICE_QUIET){rc=k7cloud_tts_control(1);goto done;}
  unsigned mode=0;
  rc=device_mode_request(intent,epoch,&mode);
  if(rc==1){rc=0;goto done;}
  if(!rc && mode==0)
    {rc=board_speech("tracking_stopped",ip,&epoch);goto done;}
  if(!rc)
    {
      for(unsigned i=0;i<300;i++)
        {
          rc=epoch!=atomic_load(&device_epoch)?-ECANCELED:k7_pipeline_voice_mode_status(mode);
          if(rc!=-EAGAIN)break;
          usleep(10000);
        }
      if(rc==-EAGAIN)rc=-ETIMEDOUT;
    }
  if(rc)
    {
      atomic_store(&assessment_live,0);
      k7_pipeline_halt();
      board_speech("tracking_failed",ip,&epoch);
      goto done;
    }
  rc=board_speech(mode==1?"tracking_ready":"assessment_starting",ip,&epoch);
  if(rc){atomic_store(&assessment_live,0);k7_pipeline_halt();}
done:
  atomic_store(&device_busy,false);
  printf("K7CLOUD device result=%d\n",rc);return rc;
#else
  (void)ip;(void)epoch;return -ENOSYS;
#endif
}
static int legacy_device_once(const char *ip)
{
  if(atomic_load(&device_loop_busy))return -EBUSY;
  return device_round(ip,atomic_load(&device_epoch));
}
static char latest_report_task[37];
static int report_play(const char *ip,const char *task,unsigned epoch)
{
  if(!task || strlen(task)!=36)return -EINVAL;
  for(unsigned i=0;i<36;i++)
    if((i==8||i==13||i==18||i==23)?task[i]!='-':
       !((task[i]>='0'&&task[i]<='9')||(task[i]>='a'&&task[i]<='f')))return -EINVAL;
  if(pthread_mutex_trylock(&audio_owner))return -EBUSY;
  pthread_mutex_lock(&tts_lock);
  if(tts_busy || epoch!=atomic_load(&device_epoch))
    {pthread_mutex_unlock(&tts_lock);pthread_mutex_unlock(&audio_owner);return -ECANCELED;}
  tts_busy=1;atomic_store(&tts_cancelled,false);
  pthread_mutex_unlock(&tts_lock);
  int fd=connect_server(ip),rc=fd;
  pthread_mutex_lock(&tts_lock);tts_fd=fd;pthread_mutex_unlock(&tts_lock);
  if(fd>=0)rc=transfer(fd,(void *)"K7S1R",5,1);
  if(!rc)rc=send_packet(fd,'R',(void *)task,36);
  unsigned segments=0,packets=0;bool finished=false;
  printf("K7CLOUD report_started task=%s\n",task);
  while(!rc && !finished && packets++<400)
    {
      unsigned char kind=0;uint32_t size=0;
      if(epoch!=atomic_load(&device_epoch)||atomic_load(&tts_cancelled)){rc=-ECANCELED;break;}
      rc=header(fd,&kind,&size);if(rc)break;
      if(kind=='W'&&!size)continue;
      if(kind=='E'&&!size){finished=true;break;}
      if(kind=='J' && size>0 && size<512)
        {
          char status[512];rc=transfer(fd,status,size,0);status[size]=0;
          if(!rc)printf("K7CLOUD report_status %s\n",status);
          continue;
        }
      if(kind!='D'||!size||size>960000||size%2||segments>=40){rc=-EPROTO;break;}
      void *pcm=malloc(size);if(!pcm){rc=-ENOMEM;break;}
      rc=transfer(fd,pcm,size,0);
      if(!rc && (epoch!=atomic_load(&device_epoch)||atomic_load(&tts_cancelled)))rc=-ECANCELED;
      if(!rc)rc=k7sound_speak_mono16_cancel(pcm,size/2,playback_cancelled,NULL);
      free(pcm);
      if(!rc)rc=send_packet(fd,'A',NULL,0);
      if(!rc)printf("K7CLOUD report_segment played=%u bytes=%" PRIu32 "\n",++segments,size);
    }
  if(!rc&&!finished)rc=-ETIMEDOUT;
  pthread_mutex_lock(&tts_lock);
  tts_fd=-1;if(fd>=0)close(fd);tts_busy=0;tts_result=rc;
  pthread_mutex_unlock(&tts_lock);pthread_mutex_unlock(&audio_owner);
  printf("K7CLOUD report_done result=%d segments=%u task=%s\n",rc,segments,task);
  return rc;
}
static int legacy_report_play(const char *ip,const char *task)
{
  if(atomic_load(&device_loop_busy)||atomic_load(&device_busy))return -EBUSY;
  return report_play(ip,task,atomic_load(&device_epoch));
}
/* Immutable snapshot first; no submission until all views and held mode agree. */
static int photo_upload(const char *ip,unsigned command_epoch,uint32_t photo_epoch,
                        unsigned admission)
{
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
  const size_t cap=1024*1024;unsigned char *images=malloc(3*cap);
  struct k7_photo_record records[3];int rc=images?0:-ENOMEM,fd=-1;
  const char *stage="copy";
  static uint64_t boot_id;
  if(!boot_id)boot_id=k7sound_clock_us();
  for(unsigned i=0;!rc && i<3;i++)
    rc=k7_photo_native_copy(photo_epoch,1u<<i,&records[i],images+i*cap,cap);
  uint32_t current=0;unsigned mask=0;
  if(!rc)rc=k7_photo_native_status(&current,&mask);
  if(!rc && (current!=photo_epoch || mask!=7 || (command_epoch!=atomic_load(&device_epoch) ||
      (admission && admission!=atomic_load(&assessment_live)))))rc=-ECANCELED;
  if(!rc)rc=k7_pipeline_voice_mode_status(0);
  if(!rc){stage="connect";fd=connect_server(ip);if(fd<0)rc=fd;}
  if(!rc)
    {
      pthread_mutex_lock(&tts_lock);
      if(admission && admission!=atomic_load(&assessment_live))rc=-ECANCELED;
      else if(tts_busy)rc=-EBUSY;
      else{tts_busy=1;tts_fd=fd;atomic_store(&tts_cancelled,0);}
      pthread_mutex_unlock(&tts_lock);
      if(rc){close(fd);fd=-1;}
    }
  if(!rc){stage="protocol";rc=transfer(fd,(void *)"K7S1U",5,1);}
  char capture_id[80];
  snprintf(capture_id,sizeof(capture_id),"k7-%016" PRIx64 "-%" PRIu32,boot_id,photo_epoch);
  if(!rc){stage="metadata";rc=send_packet(fd,'M',capture_id,strlen(capture_id));}
  const unsigned char markers[3]={'F','L','R'};
  for(unsigned i=0;!rc && i<3;i++)
    {
      uint32_t n=(uint32_t)records[i].bytes;
      unsigned char h[5]={markers[i],n>>24,n>>16,n>>8,n};
      if((command_epoch!=atomic_load(&device_epoch) ||
      (admission && admission!=atomic_load(&assessment_live)))){rc=-ECANCELED;break;}
      stage=i==0?"front":i==1?"left":"right";
      rc=transfer(fd,h,5,1);
      /* Bounded writes keep one JPEG packet while limiting socket enqueue bursts. */
      for(uint32_t off=0;!rc && off<n;)
        {
          uint32_t chunk=n-off>1024?1024:n-off;
          if((command_epoch!=atomic_load(&device_epoch) ||
      (admission && admission!=atomic_load(&assessment_live)))){rc=-ECANCELED;break;}
          rc=transfer(fd,images+i*cap+off,chunk,1);off+=chunk;
        }
    }
  if(!rc)rc=k7_photo_native_status(&current,&mask);
  if(!rc && (current!=photo_epoch || mask!=7 || (command_epoch!=atomic_load(&device_epoch) ||
      (admission && admission!=atomic_load(&assessment_live)))))rc=-ECANCELED;
  if(!rc)rc=k7_pipeline_voice_mode_status(0);
  if(!rc){stage="commit";rc=send_packet(fd,'E',NULL,0);}
  unsigned char kind=0;uint32_t n=0;unsigned waits=0;char task[37]={0};
  if(!rc){stage="backend_ack";rc=header(fd,&kind,&n);}
  while(!rc && kind=='W' && n==0 && waits++<18)rc=header(fd,&kind,&n);
  if(!rc && (kind!='K' || n!=36))rc=-EIO;
  if(!rc)rc=transfer(fd,task,36,0);
  if(!rc)memcpy(latest_report_task,task,sizeof(latest_report_task));
  if(fd>=0)
    {
      pthread_mutex_lock(&tts_lock);tts_fd=-1;tts_busy=0;pthread_mutex_unlock(&tts_lock);
      close(fd);
    }
  free(images);
  printf("K7CLOUD photo_transport stage=%s result=%d\n",stage,rc);
  printf("K7CLOUD photo_upload epoch=%" PRIu32 " result=%d accepted=%d task=%s\n",photo_epoch,rc,!rc,rc?"":task);
  return rc;
#else
  (void)ip;(void)command_epoch;(void)photo_epoch;(void)admission;return -ENOSYS;
#endif
}

static int legacy_photo_retry(const char *ip)
{
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
  bool expected=false;
  if(atomic_load(&device_loop_busy) ||
     !atomic_compare_exchange_strong(&device_busy,&expected,true))return -EBUSY;
  int rc=pthread_mutex_trylock(&audio_owner);
  if(rc){atomic_store(&device_busy,false);return -EBUSY;}
  uint32_t epoch=0;unsigned done=0;
  rc=k7_photo_native_status(&epoch,&done);
  if(!rc && done!=7)rc=-ENODATA;
  if(!rc)rc=k7_pipeline_voice_mode_status(0);
  if(!rc)rc=photo_upload(ip,atomic_load(&device_epoch),epoch,0);
  pthread_mutex_unlock(&audio_owner);atomic_store(&device_busy,false);
  printf("K7CLOUD photo_retry result=%d retained_views=%u\n",rc,done);
  return rc;
#else
  (void)ip;return -ENOSYS;
#endif
}

static int photo_prompts(const char *ip,unsigned epoch,struct k7_photo_prompt_state *state)
{
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
  unsigned admission=atomic_load(&assessment_live);
  if(!admission)return 0;
  uint32_t photo_epoch;unsigned done;
  int rc=k7_photo_native_status(&photo_epoch,&done);
  if(rc)return rc;
  if(k7_pipeline_voice_mode_status(2) && !(done==7 && !k7_pipeline_voice_mode_status(0)))return 0;
  unsigned held_revision=0;
  if(done==7)
    {
      rc=k7_pipeline_hold_revision(&held_revision);
      /* The last saved image precedes the decoder's applied hold receipt.
       * Keep this stage pending until that normal transition completes. */
      if(rc==-EAGAIN)return -EBUSY;
      if(rc)return rc;
    }
  const char *keys[4];int n=k7_photo_prompt_plan(state,photo_epoch,done,keys);
  if(n<0)return n;
  if(!n)return 0;
  for(int i=0;i<n;i++)
    {
      uint32_t current;unsigned mask;
      rc=k7_photo_native_status(&current,&mask);
      if(rc)return rc;
      if(current!=photo_epoch)return 0; /* Rescan next round; do not announce old set. */
      if(mask!=done)return 0; /* Do not announce alignment for a view already captured. */
      if(k7_pipeline_voice_mode_status(2) && !(done==7 && !k7_pipeline_voice_mode_status(0)))return 0;
      if(admission!=atomic_load(&assessment_live))return -ECANCELED;
      rc=board_speech(keys[i],ip,&epoch);if(rc)return rc;
    }
  if(admission!=atomic_load(&assessment_live))return -ECANCELED;
  *state=(struct k7_photo_prompt_state){photo_epoch,done};
  if(done==7)
    {
      rc=photo_upload(ip,epoch,photo_epoch,admission);
      if(rc){board_speech("upload_failed",ip,&epoch);return rc;}
      if(admission!=atomic_load(&assessment_live))return -ECANCELED;
      rc=board_speech("upload_accepted",ip,&epoch);if(rc)return rc;
      rc=report_play(ip,latest_report_task,epoch);if(rc)return rc;
      uint32_t final_photo_epoch;unsigned final_done;
      rc=k7_photo_native_status(&final_photo_epoch,&final_done);if(rc)return rc;
      if(final_photo_epoch!=photo_epoch || final_done!=7)return -ESTALE;
      pthread_mutex_lock(&tts_lock);
      rc=epoch!=atomic_load(&device_epoch) || admission!=atomic_load(&assessment_live) || atomic_load(&tts_cancelled) ?
        -ECANCELED : k7_pipeline_resume_tracking(held_revision);
      pthread_mutex_unlock(&tts_lock);
      if(!rc) {
        for(unsigned i=0;i<300;i++) {
          if(epoch!=atomic_load(&device_epoch)){rc=-ECANCELED;break;}
          rc=k7_pipeline_voice_mode_status(1);
          if(rc!=-EAGAIN)break;
          usleep(10000);
        }
        if(rc==-EAGAIN)rc=-ETIMEDOUT;
      }
      printf("K7CLOUD report_resume_tracking result=%d photo_epoch=%" PRIu32 "\n",rc,photo_epoch);
      if(rc)return rc;
      atomic_compare_exchange_strong(&assessment_live,&admission,0);
    }
#else
  (void)ip;(void)epoch;(void)state;
#endif
  return 0;
}
/* Stage checks continue independently of cloud ASR completion. The existing
 * audio owner serializes ADC/DAC; pending prompts take priority between rounds.
 * A worker never outlives its loop context, including on stop or failure. */
struct photo_worker {
  const char *ip;
  unsigned epoch;
  atomic_bool stop,pending;
  atomic_int result;
  struct k7_photo_prompt_state state;
};
static void *photo_watch(void *arg)
{
  struct photo_worker *w=arg;
  while(!atomic_load(&w->stop) && w->epoch==atomic_load(&device_epoch))
    {
      int rc=0;bool pending=false;
      unsigned admission=atomic_load(&assessment_live);
#if defined(CONFIG_EXAMPLES_K7HOST_TRACK)
      uint32_t epoch;unsigned done;
      rc=k7_photo_native_status(&epoch,&done);
      if(!rc && admission && (!k7_pipeline_voice_mode_status(2) ||
                 (done==7 && !k7_pipeline_voice_mode_status(0))))
        pending=epoch!=w->state.epoch || done!=w->state.done;
      /* No photo session yet is normal while listening for the first command. */
      if(rc==-ENODEV && !admission)rc=0;
      if(!rc && admission && done!=7 && !k7_pipeline_voice_mode_status(0))
        atomic_compare_exchange_strong(&assessment_live,&admission,0);
#endif
      atomic_store(&w->pending,pending);
      atomic_store(&photo_prompt_pending,pending);
      /* Do not steal audio between a command's applied-mode ACK and reply. */
      if(!rc && pending)
        rc=atomic_load(&device_busy)?-EBUSY:photo_prompts(w->ip,w->epoch,&w->state);
      if(rc==-ECANCELED && w->epoch==atomic_load(&device_epoch) &&
         admission!=atomic_load(&assessment_live))rc=0;
      if(rc && rc!=-EBUSY)
        {
          atomic_store(&w->result,rc);
          k7cloud_device_stop();
          break;
        }
      if(!rc){atomic_store(&w->pending,false);atomic_store(&photo_prompt_pending,false);}
      usleep(50000);
    }
  atomic_store(&w->pending,false);
  atomic_store(&photo_prompt_pending,false);
  return NULL;
}
static int device_listen(const char *ip,unsigned rounds,bool continuous,
                         int (*run_round)(const char *,unsigned))
{
  bool expected=false;
  if(!atomic_compare_exchange_strong(&device_loop_busy,&expected,true))return -EBUSY;
  unsigned epoch=atomic_load(&device_epoch);int rc=0;
  struct photo_worker *worker=calloc(1,sizeof(*worker));pthread_t watcher;
  if(!worker){atomic_store(&device_loop_busy,false);return -ENOMEM;}
  worker->ip=strdup(ip);worker->epoch=epoch;
  if(!worker->ip){free(worker);atomic_store(&device_loop_busy,false);return -ENOMEM;}
  atomic_init(&worker->stop,false);atomic_init(&worker->pending,false);
  atomic_init(&worker->result,0);
  pthread_attr_t attr;
  rc=pthread_attr_init(&attr);
  if(!rc)
    {
      rc=pthread_attr_setstacksize(&attr,32768);
      if(!rc)rc=pthread_create(&watcher,&attr,photo_watch,worker);
      pthread_attr_destroy(&attr);
    }
  if(rc){free((void*)worker->ip);free(worker);atomic_store(&device_loop_busy,false);return -rc;}
  if(continuous)puts("K7CLOUD device service_started=1");
  for(unsigned round=0;continuous || round<rounds;)
    {
      if(epoch!=atomic_load(&device_epoch)){rc=-ECANCELED;break;}
      if(atomic_load(&worker->pending)){usleep(50000);continue;}
      printf("K7CLOUD device round=%u\n",round+1);
      rc=run_round(ip,epoch);
      if(rc==-EBUSY){usleep(50000);continue;}
      if(rc)break;
      round++;
      usleep(250000);
    }
  /* A finite diagnostic batch ending is not a STOP command. Let its admitted
   * photos/report finish first; explicit STOP invalidates admission and wakes
   * this drain. Drain is bounded to 180 seconds; it does not start new ASR.
   * Finite rounds are not a total-time limit. Real errors still halt motion
   * and are never auto-retried. */
  uint64_t drain_started=k7sound_clock_us();
  while(!rc && atomic_load(&assessment_live) &&
        epoch==atomic_load(&device_epoch) && !atomic_load(&worker->result))
    {
      if(k7sound_clock_us()-drain_started>=UINT64_C(180000000))
        {puts("K7CLOUD device drain_timeout_ms=180000");rc=-ETIMEDOUT;break;}
      usleep(50000);
    }
  if(!rc && epoch!=atomic_load(&device_epoch))rc=-ECANCELED;
  atomic_store(&worker->stop,true);
  if(rc)k7cloud_device_stop();
  int joined=pthread_join(watcher,NULL);
  /* Failed join retains heap context and busy ownership; no dangling stack. */
  if(joined){vv_speech_owner_legacy_poison();k7cloud_device_stop();return -joined;}
  if(atomic_load(&worker->result))rc=atomic_load(&worker->result);
  free((void*)worker->ip);
  free(worker);
  atomic_store(&device_loop_busy,false);
  printf("K7CLOUD device loop_done=%d\n",rc);return rc;
}

static int legacy_device_loop(const char *ip,unsigned rounds)
{
  if(!rounds || rounds>300)return -EINVAL;
  return device_listen(ip,rounds,false,device_round);
}

/* Explicit continuous listener: normal no-command rounds repeat; actual
 * transport/ASR/upload errors return to the caller without reconnect loops. */
static int legacy_device_service(const char *ip)
{
  return device_listen(ip,0,true,device_round);
}

/* Native ownership of capture/reply/next round. No backend text is dispatched
 * as a motion command. Stop invalidates the epoch and shuts down waiting TTS. */
static int legacy_chat_loop(const char *ip,unsigned rounds)
{
  if(!rounds || rounds>300)return -EINVAL;
  bool expected=false;
  if(!atomic_compare_exchange_strong(&device_loop_busy,&expected,true))return -EBUSY;
  expected=false;
  if(!atomic_compare_exchange_strong(&device_busy,&expected,true))
    {atomic_store(&device_loop_busy,false);return -EBUSY;}
  unsigned epoch=atomic_load(&device_epoch);int rc=0;
  for(unsigned round=0;round<rounds;round++)
    {
      if(epoch!=atomic_load(&device_epoch)){rc=-ECANCELED;break;}
      if(pthread_mutex_trylock(&audio_owner)){rc=-EBUSY;break;}
      printf("K7CLOUD chat listening round=%u\n",round+1);
      int fd=connect_server(ip);rc=fd;
      if(fd>=0){rc=asr(fd,NULL,1);close(fd);}
      pthread_mutex_unlock(&audio_owner);
      if(rc)break;
      if(epoch!=atomic_load(&device_epoch)){rc=-ECANCELED;break;}
      puts("K7CLOUD chat answering");
      rc=board_speech("chat-reply",ip,&epoch);
      if(rc)break;
      usleep(250000);
    }
  atomic_store(&device_busy,false);
  atomic_store(&device_loop_busy,false);
  printf("K7CLOUD chat loop_done=%d\n",rc);return rc;
}

int k7cloud_board_speech(const char *mode,const char *ip){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=legacy_board_speech(mode,ip);
 vv_speech_owner_legacy_leave();return rc;
}

int k7cloud_device_once(const char *ip){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=legacy_device_once(ip);
 vv_speech_owner_legacy_leave();return rc;
}

int k7cloud_report_play(const char *ip,const char *task){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=legacy_report_play(ip,task);
 vv_speech_owner_legacy_leave();return rc;
}

int k7cloud_photo_retry(const char *ip){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=legacy_photo_retry(ip);
 vv_speech_owner_legacy_leave();return rc;
}

int k7cloud_device_loop(const char *ip,unsigned rounds){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=legacy_device_loop(ip,rounds);
 vv_speech_owner_legacy_leave();return rc;
}

int k7cloud_device_service(const char *ip){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=legacy_device_service(ip);
 vv_speech_owner_legacy_leave();return rc;
}

int k7cloud_chat_loop(const char *ip,unsigned rounds){
 int rc=vv_speech_owner_legacy_enter();if(rc)return rc;
 rc=legacy_chat_loop(ip,rounds);
 vv_speech_owner_legacy_leave();return rc;
}
