#include "echo_observer.h"
#include <errno.h>
#include <pthread.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle=PTHREAD_COND_INITIALIZER;
static k7_echo_observer_fn observer;
static void *argument;
static unsigned inflight;
static int retiring;
int k7_echo_observer_subscribe(k7_echo_observer_fn fn,void *arg){
 if(!fn)return -EINVAL;
 pthread_mutex_lock(&lock);int rc=observer||retiring?-EBUSY:0;
 if(!rc){observer=fn;argument=arg;}pthread_mutex_unlock(&lock);return rc;
}
int k7_echo_observer_unsubscribe(k7_echo_observer_fn fn,void *arg){
 pthread_mutex_lock(&lock);
 if(observer!=fn||argument!=arg){pthread_mutex_unlock(&lock);return -ESTALE;}
 observer=NULL;retiring=1;
 while(inflight)pthread_cond_wait(&idle,&lock);
 argument=NULL;retiring=0;pthread_mutex_unlock(&lock);return 0;
}
void k7_echo_observer_emit(const struct k7_echo_packet *packet,int reason){
 pthread_mutex_lock(&lock);k7_echo_observer_fn fn=observer;void *arg=argument;
 if(fn)inflight++;
 pthread_mutex_unlock(&lock);
 if(!fn)return;
 fn(arg,packet,reason);
 pthread_mutex_lock(&lock);inflight--;if(!inflight)pthread_cond_broadcast(&idle);pthread_mutex_unlock(&lock);
}
