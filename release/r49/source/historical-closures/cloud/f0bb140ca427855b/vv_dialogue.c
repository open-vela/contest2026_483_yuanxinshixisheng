#include "vv_dialogue.h"
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include "k7_echo_reply_identity.h"
static atomic_uint_fast64_t diagnostic_instances;
#endif

#include "vv_chat_stream.h"
#include "vv_tts_segments.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct reply_guard {
  struct vv_dialogue *dialogue;
  unsigned generation;
  const struct vv_https_cancel *outer;
  uint64_t reply_id;
};
static uint_fast64_t reply_state(uint64_t id,enum vv_dialogue_reply_phase phase) {
  return ((uint_fast64_t)id<<2)|(unsigned)phase;
}
static bool retired(void *arg) {
  struct reply_guard *g=arg;
  uint_fast64_t state=atomic_load(&g->dialogue->reply_state);
  return atomic_load(&g->dialogue->generation)!=g->generation ||
    atomic_load(&g->dialogue->sleep_requested) ||
    (g->reply_id&&state!=reply_state(g->reply_id,VV_DIALOGUE_REPLY_THINKING)&&
                  state!=reply_state(g->reply_id,VV_DIALOGUE_REPLY_SPEAKING)) ||
    (g->outer&&g->outer->is_cancelled&&g->outer->is_cancelled(g->outer->arg));
}
static bool reply_only_cancelled(const struct reply_guard *g) {
  return g->reply_id&&atomic_load(&g->dialogue->reply_state)==
    reply_state(g->reply_id,VV_DIALOGUE_REPLY_CANCEL_REQUESTED)&&
    atomic_load(&g->dialogue->generation)==g->generation&&
    !atomic_load(&g->dialogue->sleep_requested)&&
    !(g->outer&&g->outer->is_cancelled&&g->outer->is_cancelled(g->outer->arg));
}
static int claim(struct vv_dialogue *d) {
  bool expected=false;
  return atomic_compare_exchange_strong(&d->busy,&expected,true)?0:-EBUSY;
}
static void clear_history(struct vv_dialogue *d) {
  vv_https_secure_zero(d->history,sizeof(d->history));d->history_count=0;
}
static void asleep(struct vv_dialogue *d) {
  d->awake=false;d->last_activity=0;clear_history(d);
}
static bool expired(struct vv_dialogue *d,uint64_t now) {
  return now==UINT64_MAX||now<d->last_activity||now-d->last_activity>=d->idle_ms;
}
static void drop_oldest(struct vv_dialogue *d) {
  if(!d->history_count)return;
  --d->history_count;
  memmove(d->history,d->history+1,d->history_count*sizeof(d->history[0]));
  vv_https_secure_zero(&d->history[d->history_count],sizeof(d->history[0]));
}
static void remember(struct vv_dialogue *d,const char *user,size_t user_bytes,
                     size_t user_scalars,const char *answer,size_t answer_bytes,
                     size_t answer_scalars) {
  /* Do not retain an older conversation as though it were the immediately
   * preceding turn when the latest user/answer record cannot fit storage. */
  if(user_bytes+answer_bytes>VV_DIALOGUE_HISTORY_TURN_BYTES){clear_history(d);return;}
  if(d->history_count==VV_DIALOGUE_HISTORY_TURNS)drop_oldest(d);
  struct vv_dialogue_turn *t=&d->history[d->history_count++];
  memcpy(t->text,user,user_bytes+1);
  memcpy(t->text+user_bytes+1,answer,answer_bytes+1);
  t->user_bytes=user_bytes;t->answer_bytes=answer_bytes;t->utf16_units=user_scalars+answer_scalars;
}
static size_t chars(const char *text) {
  size_t n=0;(void)vv_chat_text_utf16(text,VV_CHAT_QUESTION_BYTES,NULL,&n,NULL);return n;
}
static void append(char *out,size_t *at,const char *text,size_t bytes) {
  memcpy(out+*at,text,bytes);*at+=bytes;out[*at]=0;
}
static int request(struct vv_dialogue *d,const char *current,size_t current_bytes,
                   size_t current_scalars,char *out) {
  static const char preface[]="以下是本次对话的历史记录，只用于理解上下文，请回答最后的用户问题。\n";
  static const char user[]="历史用户：",assistant[]="\n历史助手：",tail[]="\n";
  static const char interrupted[]="\n该轮回答被打断，未记录完整回答。";
  static const char current_user[]="当前用户：",current_assistant[]="\n助手：";
  const size_t fixed_bytes=strlen(preface)+strlen(current_user)+strlen(current_assistant)+current_bytes;
  const size_t fixed_chars=chars(preface)+chars(current_user)+chars(current_assistant)+current_scalars;
  for(;;){
    if(!d->history_count){memcpy(out,current,current_bytes+1);return 0;}
    size_t bytes=fixed_bytes,count=fixed_chars;
    for(unsigned i=0;i<d->history_count;i++){
      const struct vv_dialogue_turn *t=&d->history[i];
      const char *label=t->answer_bytes?assistant:interrupted;
      bytes+=strlen(user)+strlen(label)+strlen(tail)+t->user_bytes+t->answer_bytes;
      count+=chars(user)+chars(label)+chars(tail)+t->utf16_units;
    }
    if(bytes<=VV_CHAT_QUESTION_BYTES&&count<=VV_CHAT_QUESTION_UTF16_MAX)break;
    drop_oldest(d);
  }
  size_t at=0;append(out,&at,preface,strlen(preface));
  for(unsigned i=0;i<d->history_count;i++){
    const struct vv_dialogue_turn *t=&d->history[i];
    append(out,&at,user,strlen(user));append(out,&at,t->text,t->user_bytes);
    if(t->answer_bytes){
      append(out,&at,assistant,strlen(assistant));append(out,&at,t->text+t->user_bytes+1,t->answer_bytes);
    }else append(out,&at,interrupted,strlen(interrupted));
    append(out,&at,tail,strlen(tail));
  }
  append(out,&at,current_user,strlen(current_user));append(out,&at,current,current_bytes);
  append(out,&at,current_assistant,strlen(current_assistant));return 0;
}
int vv_dialogue_init(struct vv_dialogue *d,const struct vv_dialogue_ops *ops,unsigned idle_ms) {
  if(!d||!ops||(!ops->ask_speak&&(!ops->ask||!ops->speak))||!ops->now_ms||!idle_ms)return -EINVAL;
  memset(d,0,sizeof(*d));d->ops=*ops;d->idle_ms=idle_ms;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
  uint_fast64_t n=atomic_load(&diagnostic_instances);
  do{if(n==UINT64_MAX)return -EOVERFLOW;}while(!atomic_compare_exchange_weak(&diagnostic_instances,&n,n+1));
  d->diagnostic_instance=n+1;
#endif

  atomic_init(&d->generation,1u);atomic_init(&d->reply_state,0);
  atomic_init(&d->busy,false);atomic_init(&d->sleep_requested,false);return 0;
}
void vv_dialogue_interrupt(struct vv_dialogue *d) {
  if(d)atomic_fetch_add(&d->generation,1u);
}
int vv_dialogue_reply_snapshot(const struct vv_dialogue *d,struct vv_dialogue_reply_status *out) {
  if(!d||!out)return -EINVAL;
  uint_fast64_t state=atomic_load(&d->reply_state);
  *out=(struct vv_dialogue_reply_status){(uint64_t)(state>>2),
    (enum vv_dialogue_reply_phase)(state&3u)};return 0;
}
int vv_dialogue_cancel_reply_id(struct vv_dialogue *d,uint64_t expected_id) {
  if(!d||!expected_id||expected_id>(UINT64_MAX>>2))return -EINVAL;
  uint_fast64_t state=atomic_load(&d->reply_state);
  for(;;){
    if((state>>2)!=expected_id||
       ((state&3u)!=VV_DIALOGUE_REPLY_THINKING&&(state&3u)!=VV_DIALOGUE_REPLY_SPEAKING))return 0;
    if(atomic_compare_exchange_weak(&d->reply_state,&state,
        reply_state(expected_id,VV_DIALOGUE_REPLY_CANCEL_REQUESTED)))
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
    {
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
      (void)k7_echo_reply_cancel(d->diagnostic_instance,expected_id);
#endif
      return 1;
    }
#else
      return 1;
#endif
  }
}
int vv_dialogue_wake(struct vv_dialogue *d) {
  if(!d)return -EINVAL;
  int rc=claim(d);if(rc)return rc;
  uint64_t now=d->ops.now_ms(d->ops.arg);
  if(!d->awake||atomic_load(&d->sleep_requested)||expired(d,now))clear_history(d);
  if(now==UINT64_MAX){asleep(d);rc=-ETIMEDOUT;}
  else {atomic_store(&d->sleep_requested,false);d->awake=true;d->last_activity=now;}
  atomic_store(&d->busy,false);return rc;
}
int vv_dialogue_sleep(struct vv_dialogue *d) {
  if(!d)return -EINVAL;
  atomic_store(&d->sleep_requested,true);vv_dialogue_interrupt(d);
  /* If a reply owns storage, its guard stops work and its exit clears history.
   * No second thread touches the non-atomic session or history fields. */
  if(claim(d))return 0;
  asleep(d);atomic_store(&d->busy,false);return 0;
}
static int reply_common(struct vv_dialogue *d,const char *text,const struct vv_https_cancel *outer,
  struct vv_reply_scope *scope,uint64_t owner_epoch,uint32_t asr_epoch,
  vv_dialogue_scoped_speak scoped,void *scoped_arg) {
  if(!d||!text)return -EINVAL;
  int rc=claim(d);if(rc)return rc;
  struct vv_reply_scope_token lease={0};bool leased=false,admitted=false;
  size_t bytes=0,scalars=0;bool content=false;
  char *answer=NULL,*question=NULL;char segment[321]={0};
  uint64_t now=d->ops.now_ms(d->ops.arg);
  struct reply_guard guard={d,atomic_load(&d->generation),outer,0};
  struct vv_https_cancel cancel={retired,&guard};
  if(atomic_load(&d->sleep_requested)){asleep(d);goto done;}
  if(retired(&guard)){rc=-ECANCELED;goto done;}
  if(!d->awake)goto done;
  if(expired(d,now)){
    /* Resident listening outlives conversational history. Do not silently
     * discard the first question after a normal idle interval. STOP/asleep
     * were checked above; clock failure still fails closed as before. */
    if(!d->ops.resident_idle||now==UINT64_MAX||now<d->last_activity){asleep(d);goto done;}
    clear_history(d);
  }
  rc=vv_chat_text_utf16(text,VV_CHAT_QUESTION_BYTES,&bytes,&scalars,&content);
  if(rc){if(rc==VV_CHAT_LIMIT)rc=-E2BIG;goto done;}
  if(!content)goto done;
  if(scalars>VV_CHAT_QUESTION_UTF16_MAX){rc=-E2BIG;goto done;}
  if(d->reply_sequence>=(UINT64_MAX>>2)){rc=-EOVERFLOW;goto done;}
  answer=calloc(VV_DIALOGUE_ANSWER_CAP,1);question=calloc(VV_CHAT_QUESTION_BYTES+1u,1);
  if(!answer||!question){rc=-ENOMEM;goto done;}
  rc=request(d,text,bytes,scalars,question);if(rc)goto done;
  if(retired(&guard)){rc=-ECANCELED;goto done;}
  guard.reply_id=++d->reply_sequence;
  atomic_store(&d->reply_state,reply_state(guard.reply_id,VV_DIALOGUE_REPLY_THINKING));
  if(retired(&guard)){rc=-ECANCELED;goto done;}
  d->last_activity=now;
  if(scoped){
    rc=vv_reply_scope_begin(scope,owner_epoch,asr_epoch,guard.reply_id,&lease);
    if(rc)goto done;
    leased=true;
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
    (void)k7_echo_reply_begin(d->diagnostic_instance,lease.incarnation,lease.serial,lease.owner_epoch,lease.reply_id,lease.asr_epoch);
#endif
  }
  admitted=true;
  if(scoped||d->ops.ask_speak){
    uint_fast64_t thinking=reply_state(guard.reply_id,VV_DIALOGUE_REPLY_THINKING);
    if(!atomic_compare_exchange_strong(&d->reply_state,&thinking,
        reply_state(guard.reply_id,VV_DIALOGUE_REPLY_SPEAKING))){rc=-ECANCELED;goto done;}
    int local=d->ops.local_reply?d->ops.local_reply(d->ops.arg,text,answer,
      VV_DIALOGUE_ANSWER_CAP,&cancel,leased?&lease:NULL):0;
    if(local<0)rc=local;
    else if(local>1)rc=-EPROTO;
    else if(local==1)rc=0;
    else rc=scoped?scoped(scoped_arg,question,answer,VV_DIALOGUE_ANSWER_CAP,&cancel,&lease):
      d->ops.ask_speak(d->ops.arg,question,answer,VV_DIALOGUE_ANSWER_CAP,&cancel);
    if(!rc&&leased)rc=vv_reply_scope_speaker_safe(&lease);
  }else rc=d->ops.ask(d->ops.arg,question,answer,VV_DIALOGUE_ANSWER_CAP,&cancel);
  if(retired(&guard)){rc=-ECANCELED;goto done;}
  if(rc)goto done;
  size_t answer_bytes=0,answer_scalars=0;
  rc=vv_chat_text_utf16(answer,VV_DIALOGUE_ANSWER_CAP-1u,&answer_bytes,&answer_scalars,&content);
  if(rc||!content){rc=-EPROTO;goto done;}
  if(!scoped&&!d->ops.ask_speak){
  struct vv_tts_segments cursor;
  rc=vv_tts_segments_init(&cursor,answer,answer_bytes);if(rc){rc=-EPROTO;goto done;}
  uint_fast64_t thinking=reply_state(guard.reply_id,VV_DIALOGUE_REPLY_THINKING);
  if(!atomic_compare_exchange_strong(&d->reply_state,&thinking,
      reply_state(guard.reply_id,VV_DIALOGUE_REPLY_SPEAKING))){rc=-ECANCELED;goto done;}
  for(;;){
    size_t written=0;
    if(retired(&guard)){rc=-ECANCELED;break;}
    int next=vv_tts_segments_next_limit(&cursor,segment,sizeof(segment),&written,80);
    if(next==VV_TTS_SEGMENT_DONE){rc=0;break;}
    if(next!=VV_TTS_SEGMENT_READY){rc=-EPROTO;break;}
    rc=d->ops.speak(d->ops.arg,segment,&cancel);
    vv_https_secure_zero(segment,sizeof(segment));
    if(retired(&guard))rc=-ECANCELED;
    if(rc)break;
  }
  }
  if(!rc){
    uint64_t completed=d->ops.now_ms(d->ops.arg);
    if(completed==UINT64_MAX||completed<now){asleep(d);rc=-ETIMEDOUT;goto done;}
    if(retired(&guard)){rc=-ECANCELED;goto done;}
    /* Retire eligibility before committing history. Targeted cancellation
     * either wins this exact ID or observes completion; it cannot be accepted
     * after the last cancellation check but before history is committed. */
    uint_fast64_t speaking=reply_state(guard.reply_id,VV_DIALOGUE_REPLY_SPEAKING);
    if(!atomic_compare_exchange_strong(&d->reply_state,&speaking,0)){rc=-ECANCELED;goto done;}
    remember(d,text,bytes,scalars,answer,answer_bytes,answer_scalars);
    d->replies++;d->last_activity=completed;
  }
 done:
  /* Only the busy reply owner retains a valid, admitted user's question.
   * Cancellation never makes a partial or unplayed answer authoritative. */
  if(admitted&&(rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)&&reply_only_cancelled(&guard)){
    uint64_t cancelled_at=d->ops.now_ms(d->ops.arg);
    if(cancelled_at==UINT64_MAX||cancelled_at<now)asleep(d);
    else if(reply_only_cancelled(&guard)){
      remember(d,text,bytes,scalars,"",0,0);d->last_activity=cancelled_at;
    }
  }
  atomic_store(&d->reply_state,0);
  if(rc==-ECANCELED||rc==VV_HTTPS_ECANCELLED)d->interruptions++;
  if(atomic_load(&d->sleep_requested))asleep(d);
  d->last_error=rc;
  if(answer){vv_https_secure_zero(answer,VV_DIALOGUE_ANSWER_CAP);free(answer);}
  if(question){vv_https_secure_zero(question,VV_CHAT_QUESTION_BYTES+1u);free(question);}
  vv_https_secure_zero(segment,sizeof(segment));atomic_store(&d->busy,false);
  if(leased){int retired=vv_reply_scope_reply_end(&lease);if(!rc&&retired)rc=retired;}
  return rc;
}

int vv_dialogue_reply(struct vv_dialogue*d,const char*text,const struct vv_https_cancel*outer){
  return reply_common(d,text,outer,NULL,0,0,NULL,NULL);
}
int vv_dialogue_reply_scoped(struct vv_dialogue*d,const char*text,const struct vv_https_cancel*outer,
  struct vv_reply_scope*scope,uint64_t owner,uint32_t asr,vv_dialogue_scoped_speak speak,void*arg){
  if(!scope||!owner||!asr||!speak)return -EINVAL;
  return reply_common(d,text,outer,scope,owner,asr,speak,arg);
}
