#include "vv_real_upload_private.h"
#include "vv_real_shuiguang.h"
#include "vv_native_assessment.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct guard {
  struct vv_native_assessment *owner;const struct vv_https_cancel *outer;
  uint64_t deadline,last;bool timed_out,backwards,user_cancelled,wait_route;atomic_bool reported;
};
static bool cancelled(void *arg) {
  struct guard *g=arg;
  if(g->outer && g->outer->is_cancelled && g->outer->is_cancelled(g->outer->arg))g->user_cancelled=true;
  uint64_t previous=g->last;
  uint64_t now=g->owner->now_ms(g->owner->clock_arg);
  if(now<g->last)g->backwards=true;
  if(now>=g->deadline)g->timed_out=true;
  g->last=now;
  bool stop=g->user_cancelled || g->timed_out || g->backwards;
  if(stop&&!atomic_exchange(&g->reported,true)){
    printf("ASSESS_CANCEL user=%u backwards=%u timeout=%u\n",(unsigned)g->user_cancelled,(unsigned)g->backwards,(unsigned)g->timed_out);
    printf("ASSESS_CANCEL now=%llu last=%llu deadline=%llu\n",(unsigned long long)now,(unsigned long long)previous,(unsigned long long)g->deadline);fflush(stdout);
  }
  return stop;
}
static enum vv_assessment_outcome gate(struct guard *g) {
  if(!cancelled(g))return VV_ASSESSMENT_READY;
  /* A link-triggered outer cancellation must not conceal a bad monotonic clock. */
  if((K7_REAL_SHUIGUANG_PROFILE||g->wait_route)&&g->backwards)return VV_ASSESSMENT_PROTOCOL;
  if(g->user_cancelled)return VV_ASSESSMENT_CANCELLED;
  return g->backwards?VV_ASSESSMENT_PROTOCOL:VV_ASSESSMENT_TIMEOUT;
}
static enum vv_assessment_outcome checked(struct guard *g,int rc,struct vv_assessment_result *r) {
  r->backend_error=rc;
  enum vv_assessment_outcome state=gate(g);if(state)return state;
  if(rc==VV_BACKEND_CLIENT_REPLACED)return VV_ASSESSMENT_REPLACED;
  return rc?VV_ASSESSMENT_BACKEND_ERROR:VV_ASSESSMENT_READY;
}
static bool uuid(const char *s) {
  if(strlen(s)!=36)return false;
  for(unsigned i=0;i<36;++i) {
    if(i==8 || i==13 || i==18 || i==23) {if(s[i]!='-')return false;}
    else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F')))return false;
  }
  return true;
}
static enum vv_assessment_outcome task_status(struct vv_backend_task *t,struct vv_assessment_result *r) {
  if(strcmp(t->task_id,r->task_id))return VV_ASSESSMENT_PROTOCOL;
  if(t->status==VV_TASK_NEEDS_RETAKE) {
    if(!t->required_views || (t->required_views&~7u))return VV_ASSESSMENT_PROTOCOL;
    r->required_views=t->required_views;return VV_ASSESSMENT_NEEDS_RETAKE;
  }
  if(t->status==VV_TASK_FAILED) {
    memcpy(r->failure_code,t->failure_code,sizeof(r->failure_code));return VV_ASSESSMENT_FAILED;
  }
  if(t->status!=VV_TASK_QUEUED && t->status!=VV_TASK_ANALYZING && t->status!=VV_TASK_REPORT_READY)
    return VV_ASSESSMENT_PROTOCOL;
  return VV_ASSESSMENT_READY;
}
void vv_native_assessment_init(struct vv_native_assessment *o) {
  if(o){memset(o,0,sizeof(*o));atomic_init(&o->busy,false);}
}
static enum vv_assessment_outcome upload_checked(struct guard *g,
 const struct vv_assessment_request *q,const struct vv_https_cancel *cancel,
 struct vv_assessment_result *r,vv_assessment_upload_fn upload_override,void *upload_arg){
 enum vv_assessment_outcome state=gate(g);if(state)return state;
 r->upload_attempted=true;
 int rc=upload_override?upload_override(upload_arg,q,r->task_id,cancel):
  g->owner->ops->upload(g->owner->ops_arg,q,r->task_id,cancel);
 state=checked(g,rc,r);
 if(state)return state;
 return !memchr(r->task_id,0,sizeof(r->task_id))||!uuid(r->task_id)?VV_ASSESSMENT_PROTOCOL:VV_ASSESSMENT_READY;
}
enum vv_assessment_outcome vv_native_assessment_upload_only(struct vv_native_assessment *o,
 const struct vv_assessment_request *q,const struct vv_https_cancel *outer,
 struct vv_assessment_result *r){
 if(!o||!q||!r||!o->ops||!o->ops->upload||!o->now_ms||
    !q->capture_session||!q->consent_ref||!q->boundary_hex||!q->idempotency_key||
    !q->total_timeout_ms||q->total_timeout_ms>3600000)return VV_ASSESSMENT_INVALID;
 if(atomic_exchange(&o->busy,true))return VV_ASSESSMENT_BUSY;
 memset(r,0,sizeof(*r));
 struct guard g={.owner=o,.outer=outer,.last=o->now_ms(o->clock_arg)};
 atomic_init(&g.reported,false);g.deadline=g.last+q->total_timeout_ms;
 enum vv_assessment_outcome state=VV_ASSESSMENT_INVALID;
 if(g.deadline>=g.last){
  struct vv_https_cancel cancel={cancelled,&g};
  state=upload_checked(&g,q,&cancel,r,NULL,NULL);
  if(state==VV_ASSESSMENT_READY)state=VV_ASSESSMENT_UPLOADED;
 }
 r->outcome=state;atomic_store(&o->busy,false);return state;
}
static enum vv_assessment_outcome run_internal(struct vv_native_assessment *o,
  const struct vv_assessment_request *q,unsigned char *pcm,size_t cap,
  const struct vv_https_cancel *outer,struct vv_assessment_result *r,
  char *report_text,size_t report_capacity,size_t *report_bytes,
  int (*uploaded)(void *,const struct vv_https_cancel *),void *progress_arg,
  vv_assessment_wait_fn wait_task,void *wait_arg,vv_assessment_upload_fn upload_override,void *upload_arg) {
  bool text_only=report_text!=NULL;
  size_t text_capacity=text_only?4612:4097;
  if(!r)return VV_ASSESSMENT_INVALID;
  if(!o || !q || (!text_only && (!pcm || !cap || cap>960000)) ||
     (text_only && (!report_capacity || !report_bytes)) || !o->ops || !o->ops->upload ||
     (!wait_task&&(!o->ops->task || !o->ops->brief)) || (!text_only && !o->ops->tts) || !o->now_ms || !o->wait_ms ||
     !q->capture_session || !q->consent_ref || !q->boundary_hex || !q->idempotency_key ||
     (!wait_task&&(!q->poll_interval_ms || q->poll_interval_ms>60000)) || !q->max_polls || q->max_polls>1000 ||
     !q->total_timeout_ms || q->total_timeout_ms>3600000)return VV_ASSESSMENT_INVALID;
  /* Do not touch an active owner's PCM, including a reentrant call using the
   * same scratch pointer. BUSY leaves caller memory untouched. */
  if(atomic_exchange(&o->busy,true))return VV_ASSESSMENT_BUSY;
  memset(r,0,sizeof(*r));r->outcome=VV_ASSESSMENT_INVALID;
  if(text_only){report_text[0]=0;*report_bytes=0;}
  else vv_https_secure_zero(pcm,cap);
  struct guard g={.owner=o,.outer=outer,.last=o->now_ms(o->clock_arg)};
  atomic_init(&g.reported,false);
  g.deadline=g.last+q->total_timeout_ms;
  enum vv_assessment_outcome state=VV_ASSESSMENT_INVALID;
  struct vv_backend_brief *brief=NULL;char *text=NULL;
  const uint64_t began=g.last;unsigned retries=0;
  g.wait_route=wait_task!=NULL;
  if(g.deadline<g.last)goto done;
  struct vv_https_cancel cancel={cancelled,&g};
  state=upload_checked(&g,q,&cancel,r,upload_override,upload_arg);if(state)goto done;
  if(wait_task)printf("ASSESS_WAIT_EVENT phase=upload_accepted elapsed_ms=%llu task=%.36s\n",(unsigned long long)(g.last-began),r->task_id);
  if(uploaded){state=checked(&g,uploaded(progress_arg,&cancel),r);if(state)goto done;}
  if(wait_task){brief=calloc(1,sizeof(*brief));if(!brief){state=VV_ASSESSMENT_MEMORY;goto done;}}
  struct vv_backend_task task;
  for(unsigned i=0;i<q->max_polls;++i) {
    state=gate(&g);if(state)goto done;
    memset(&task,0,sizeof(task));++r->polls;++r->task_checks;
    if(wait_task){
      memset(brief,0,sizeof(*brief));
      printf("ASSESS_WAIT_EVENT phase=wait_begin elapsed_ms=%llu check=%u\n",(unsigned long long)(g.last-began),r->task_checks);
      int rc=wait_task(wait_arg,r->task_id,&task,brief,&cancel);
      state=checked(&g,rc,r);
      if(state){
        memset(brief,0,sizeof(*brief));
        if(state==VV_ASSESSMENT_BACKEND_ERROR&&(rc==VV_HTTPS_EIO||rc==VV_HTTPS_ETIMEOUT)&&retries<3&&i+1<q->max_polls){
          const unsigned delays[]={250,500,1000};unsigned delay=delays[retries++];
          uint64_t until=g.last+delay;
          if(until<g.last){state=VV_ASSESSMENT_PROTOCOL;goto done;}
          printf("ASSESS_WAIT_EVENT phase=retry elapsed_ms=%llu retry=%u rc=%d\n",(unsigned long long)(g.last-began),retries,rc);
          while(g.last<until){
            state=gate(&g);if(state)goto done;
            if(g.last>=until)break;
            uint64_t remain=until-g.last;if(remain>50)remain=50;
            uint64_t before=g.last;o->wait_ms(o->wait_arg,(unsigned)remain,&cancel);
            state=gate(&g);if(state)goto done;
            if(g.last==before){state=VV_ASSESSMENT_PROTOCOL;goto done;}
          }
          continue;
        }
        goto done;
      }
    }else{state=checked(&g,o->ops->task(o->ops_arg,r->task_id,&task,&cancel),r);if(state)goto done;}
    if(!memchr(task.task_id,0,sizeof(task.task_id)) || !memchr(task.report_id,0,sizeof(task.report_id)) ||
       !memchr(task.failure_code,0,sizeof(task.failure_code))){state=VV_ASSESSMENT_PROTOCOL;goto done;}
    state=task_status(&task,r);if(state)goto done;
    if(task.status==VV_TASK_REPORT_READY)break;
    if(i+1==q->max_polls){state=VV_ASSESSMENT_POLL_LIMIT;goto done;}
    if(wait_task)continue; /* Server wait already bounded; no normal pending sleep. */
    uint64_t until=g.last+q->poll_interval_ms;
    if(until<g.last){state=VV_ASSESSMENT_PROTOCOL;goto done;}
    while(g.last<until) {
      state=gate(&g);if(state)goto done;
      if(g.last>=until)break;
      uint64_t remain=until-g.last;if(remain>50)remain=50;
      uint64_t before=g.last;
      o->wait_ms(o->wait_arg,(unsigned)remain,&cancel);
      state=gate(&g);if(state)goto done;
      if(g.last==before){state=VV_ASSESSMENT_PROTOCOL;goto done;}
    }
  }
  if(!uuid(task.report_id)){state=VV_ASSESSMENT_PROTOCOL;goto done;}
  strcpy(r->report_id,task.report_id);
  if(!brief)brief=calloc(1,sizeof(*brief));
  text=calloc(1,text_capacity);
  if(!brief || !text){state=VV_ASSESSMENT_MEMORY;goto done;}
  if(!wait_task){state=checked(&g,o->ops->brief(o->ops_arg,r->report_id,brief,&cancel),r);if(state)goto done;}
  if(!memchr(brief->report_id,0,sizeof(brief->report_id)) || strcmp(brief->report_id,r->report_id) ||
     !memchr(brief->conclusion,0,sizeof(brief->conclusion)) || !memchr(brief->description,0,sizeof(brief->description)))
    {state=VV_ASSESSMENT_PROTOCOL;goto done;}
  if(K7_REAL_SHUIGUANG_PROFILE||wait_task){
    if(!vv_real_report_text(brief->conclusion,sizeof(brief->conclusion),false)||!vv_real_report_text(brief->description,sizeof(brief->description),false)){state=VV_ASSESSMENT_PROTOCOL;goto done;}
    if(!brief->conclusion[0]){r->backend_error=VV_BACKEND_CLIENT_REPORT_MISSING;state=VV_ASSESSMENT_BACKEND_ERROR;goto done;}
  }
  size_t a=strlen(brief->conclusion),b=(K7_REAL_SHUIGUANG_PROFILE||wait_task)?0:strlen(brief->description);
  size_t separator=a&&b?3:0; /* Chinese full stop, no generated medical claims. */
  if(!a&&!b){state=VV_ASSESSMENT_PROTOCOL;goto done;}
  if(a+b+separator>=text_capacity || (text_only && a+b+separator>=report_capacity)){state=VV_ASSESSMENT_PROTOCOL;goto done;}
  memcpy(text,brief->conclusion,a);
  if(separator)memcpy(text+a,"。",3);
  memcpy(text+a+separator,brief->description,b);
  if(!text_only) {
    state=checked(&g,o->ops->tts(o->ops_arg,text,pcm,cap,&r->pcm_size,&cancel),r);if(state)goto done;
    if(!r->pcm_size || r->pcm_size>cap || r->pcm_size%2){state=VV_ASSESSMENT_PROTOCOL;goto done;}
  }
  if(wait_task){
    state=gate(&g);if(state)goto done;
    printf("ASSESS_WAIT_EVENT phase=ready elapsed_ms=%llu task=%.36s report=%.36s checks=%u\n",(unsigned long long)(g.last-began),r->task_id,r->report_id,r->task_checks);
    goto done;
  }
  /* One additional GET after brief (or synthesis) rejects an already replaced
   * report. Future playback still needs its own fresh identity check. */
  memset(&task,0,sizeof(task));++r->task_checks;
  state=checked(&g,o->ops->task(o->ops_arg,r->task_id,&task,&cancel),r);if(state)goto done;
  if(!memchr(task.task_id,0,sizeof(task.task_id)) || !memchr(task.report_id,0,sizeof(task.report_id)) ||
     !memchr(task.failure_code,0,sizeof(task.failure_code))){state=VV_ASSESSMENT_PROTOCOL;goto done;}
  state=task_status(&task,r);if(state)goto done;
  if(task.status!=VV_TASK_REPORT_READY || strcmp(task.report_id,r->report_id))state=VV_ASSESSMENT_REPLACED;
done:
  if(!state && text_only){*report_bytes=strlen(text);memcpy(report_text,text,*report_bytes+1);}
  if(brief){vv_https_secure_zero(brief,sizeof(*brief));free(brief);}
  if(text){vv_https_secure_zero(text,text_capacity);free(text);}
  if(state){if(!text_only)vv_https_secure_zero(pcm,cap);r->pcm_size=0;}
  r->outcome=state;atomic_store(&o->busy,false);return state;
}

enum vv_assessment_outcome vv_native_assessment_run(struct vv_native_assessment *o,
  const struct vv_assessment_request *q,unsigned char *pcm,size_t cap,
  const struct vv_https_cancel *cancel,struct vv_assessment_result *result) {
  return run_internal(o,q,pcm,cap,cancel,result,NULL,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
}
enum vv_assessment_outcome vv_native_assessment_prepare(struct vv_native_assessment *o,
  const struct vv_assessment_request *q,char *text,size_t capacity,size_t *text_bytes,
  const struct vv_https_cancel *cancel,struct vv_assessment_result *result) {
  if(!text || !text_bytes || !capacity)return VV_ASSESSMENT_INVALID;
  return run_internal(o,q,NULL,0,cancel,result,text,capacity,text_bytes,NULL,NULL,NULL,NULL,NULL,NULL);
}
enum vv_assessment_outcome vv_native_assessment_prepare_progress(struct vv_native_assessment *o,
 const struct vv_assessment_request *q,char *text,size_t capacity,size_t *text_bytes,
 const struct vv_https_cancel *cancel,struct vv_assessment_result *result,
 int (*uploaded)(void *,const struct vv_https_cancel *),void *arg){
 return run_internal(o,q,NULL,0,cancel,result,text,capacity,text_bytes,uploaded,arg,NULL,NULL,NULL,NULL);
}

enum vv_assessment_outcome vv_native_assessment_prepare_wait_progress(struct vv_native_assessment *o,
 const struct vv_assessment_request *q,char *text,size_t capacity,size_t *text_bytes,
 const struct vv_https_cancel *cancel,struct vv_assessment_result *result,
 int (*uploaded)(void *,const struct vv_https_cancel *),void *arg,
 vv_assessment_wait_fn wait_task,void *wait_arg){
 if(!text||!text_bytes||!capacity||!wait_task)return VV_ASSESSMENT_INVALID;
 return run_internal(o,q,NULL,0,cancel,result,text,capacity,text_bytes,uploaded,arg,wait_task,wait_arg,NULL,NULL);
}

enum vv_assessment_outcome vv_native_assessment_prepare_wait_upload_progress(
 struct vv_native_assessment *o,const struct vv_assessment_request *q,
 char *text,size_t capacity,size_t *text_bytes,const struct vv_https_cancel *cancel,
 struct vv_assessment_result *result,int (*uploaded)(void *,const struct vv_https_cancel *),
 void *arg,vv_assessment_wait_fn wait_task,void *wait_arg,
 vv_assessment_upload_fn upload_override,void *upload_arg){
 if(!text||!text_bytes||!capacity||!wait_task||!upload_override)return VV_ASSESSMENT_INVALID;
 return run_internal(o,q,NULL,0,cancel,result,text,capacity,text_bytes,uploaded,arg,
  wait_task,wait_arg,upload_override,upload_arg);
}
