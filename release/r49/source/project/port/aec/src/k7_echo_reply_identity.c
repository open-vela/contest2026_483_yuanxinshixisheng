#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#include "k7_echo_reply_identity.h"
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <limits.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct k7_echo_reply_identity value;
static uint64_t dialogue_key;
static uint64_t sequence;
static bool exhausted;
static int advance(void){
 if(exhausted||sequence==UINT64_MAX){exhausted=true;value.live=false;return -EOVERFLOW;}
 value.revision=++sequence;return 0;
}
static int leave(int rc){int e=pthread_mutex_unlock(&lock);return rc?rc:-e;}
static uint64_t now_us(void){struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||
 (uint64_t)t.tv_sec>(UINT64_MAX-999999)/1000000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
int k7_echo_reply_begin(uint64_t d,uint64_t scope,uint64_t lease,uint64_t owner,uint64_t reply,uint32_t asr){
 if(!d||!scope||!lease||!owner||!reply||!asr)return -EINVAL;
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 bool collision=value.live;int rc=advance();if(rc)return leave(rc);
 if(collision){value.live=false;return leave(-EBUSY);}
 value=(struct k7_echo_reply_identity){.instance=sequence,.revision=sequence,
 .scope_incarnation=scope,.lease_serial=lease,.owner=owner,.reply=reply,.asr=asr,.live=true};
 dialogue_key=d;return leave(0);
}
int k7_echo_reply_bind(uint64_t scope,uint64_t lease,uint64_t owner,uint32_t input,uint32_t tx){
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 if(!value.live||value.scope_incarnation!=scope||value.lease_serial!=lease||value.owner!=owner)return leave(-ESTALE);
 if(!input||!tx||value.bound||value.cancelled){int rc=advance();value.live=false;return leave(rc?rc:-EPROTO);}
 int rc=advance();if(!rc){value.input=input;value.tx=tx;value.bound=true;}return leave(rc);
}
int k7_echo_reply_cancel(uint64_t d,uint64_t reply){
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 if(!value.live||dialogue_key!=d||value.reply!=reply)return leave(-ESTALE);
 if(value.cancelled)return leave(-EALREADY);
 int rc=advance();if(!rc){value.cancelled=true;value.cancel_observed_us=now_us();
  if(value.cancel_observed_us==UINT64_MAX){value.live=false;rc=-EIO;}}
 return leave(rc);
}
int k7_echo_reply_retire(uint64_t scope,uint64_t lease){
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 if(value.scope_incarnation!=scope||value.lease_serial!=lease)return leave(-ESTALE);
 bool was_live=value.live;int rc=advance();value.live=false;if(!rc&&was_live)value.retired_terminal=true;return leave(rc);
}
int k7_echo_reply_snapshot(struct k7_echo_reply_identity *out){
 if(!out)return -EINVAL;
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 int rc=exhausted?-EOVERFLOW:0;if(!rc)*out=value;return leave(rc);
}
#endif

#else
#include "k7_echo_reply_identity.h"
#if defined(K7_ECHO_RUNTIME_CONTINUITY) && K7_ECHO_RUNTIME_CONTINUITY
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <limits.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct k7_echo_reply_identity value;
static uint64_t dialogue_key;
static uint64_t sequence;
static bool exhausted;
static int advance(void){
 if(exhausted||sequence==UINT64_MAX){exhausted=true;value.live=false;return -EOVERFLOW;}
 value.revision=++sequence;return 0;
}
static int leave(int rc){int e=pthread_mutex_unlock(&lock);return rc?rc:-e;}
static uint64_t now_us(void){struct timespec t;
 if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000||
 (uint64_t)t.tv_sec>(UINT64_MAX-999999)/1000000)return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
int k7_echo_reply_begin(uint64_t d,uint64_t scope,uint64_t lease,uint64_t owner,uint64_t reply,uint32_t asr){
 if(!d||!scope||!lease||!owner||!reply||!asr)return -EINVAL;
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 bool collision=value.live;int rc=advance();if(rc)return leave(rc);
 if(collision){value.live=false;return leave(-EBUSY);}
 value=(struct k7_echo_reply_identity){.instance=sequence,.revision=sequence,
 .scope_incarnation=scope,.lease_serial=lease,.owner=owner,.reply=reply,.asr=asr,.live=true};
 dialogue_key=d;return leave(0);
}
int k7_echo_reply_bind(uint64_t scope,uint64_t lease,uint64_t owner,uint32_t input,uint32_t tx){
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 if(!value.live||value.scope_incarnation!=scope||value.lease_serial!=lease||value.owner!=owner)return leave(-ESTALE);
 if(!input||!tx||value.bound||value.cancelled){int rc=advance();value.live=false;return leave(rc?rc:-EPROTO);}
 int rc=advance();if(!rc){value.input=input;value.tx=tx;value.bound=true;}return leave(rc);
}
int k7_echo_reply_cancel(uint64_t d,uint64_t reply){
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 if(!value.live||dialogue_key!=d||value.reply!=reply)return leave(-ESTALE);
 if(value.cancelled)return leave(-EALREADY);
 int rc=advance();if(!rc){value.cancelled=true;value.cancel_observed_us=now_us();
  if(value.cancel_observed_us==UINT64_MAX){value.live=false;rc=-EIO;}}
 return leave(rc);
}
int k7_echo_reply_retire(uint64_t scope,uint64_t lease){
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 if(value.scope_incarnation!=scope||value.lease_serial!=lease)return leave(-ESTALE);
 int rc=advance();value.live=false;return leave(rc);
}
int k7_echo_reply_snapshot(struct k7_echo_reply_identity *out){
 if(!out)return -EINVAL;
 int e=pthread_mutex_lock(&lock);if(e)return -e;
 int rc=exhausted?-EOVERFLOW:0;if(!rc)*out=value;return leave(rc);
}
#endif

#endif
