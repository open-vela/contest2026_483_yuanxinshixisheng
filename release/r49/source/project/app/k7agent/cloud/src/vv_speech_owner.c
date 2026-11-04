#include "vv_speech_owner.h"
#include <pthread.h>
#include <limits.h>
#include <errno.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned legacy;
static bool poisoned;
static const void *native;
int vv_speech_owner_legacy_enter(void){
 pthread_mutex_lock(&lock);
 int rc=(native||poisoned||legacy==UINT_MAX)?-EBUSY:0;
 if(!rc)++legacy;
 pthread_mutex_unlock(&lock);return rc;
}
void vv_speech_owner_legacy_leave(void){pthread_mutex_lock(&lock);if(legacy)--legacy;pthread_mutex_unlock(&lock);}
void vv_speech_owner_legacy_poison(void){pthread_mutex_lock(&lock);poisoned=true;pthread_mutex_unlock(&lock);}
int vv_speech_owner_native_claim(const void *token){
 if(!token)return -EINVAL;
 pthread_mutex_lock(&lock);int rc=(native||legacy||poisoned)?-EBUSY:0;
 if(!rc)native=token;
 pthread_mutex_unlock(&lock);return rc;
}
void vv_speech_owner_native_release(const void *token){pthread_mutex_lock(&lock);if(token&&native==token)native=0;pthread_mutex_unlock(&lock);}
bool vv_speech_owner_native_available(const void *token){
 pthread_mutex_lock(&lock);bool ok=token&&!legacy&&!poisoned&&(!native||native==token);
 pthread_mutex_unlock(&lock);return ok;
}
