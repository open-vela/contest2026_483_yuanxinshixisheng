#include "prov_phone_time.h"
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <time.h>
static bool leap(unsigned y){return y%4==0&&(y%100!=0||y%400==0);}
static unsigned number(const char*p,unsigned n){unsigned v=0;while(n--)v=v*10+(unsigned)(*p++-'0');return v;}
int prov_phone_time_parse(const char*p,size_t n,uint64_t*out){
 if(out)*out=0;
 if(!p||!out||n!=19)return -EINVAL;
 for(unsigned i=0;i<19;i++){
  char sep=i==4||i==7?'-':i==10?' ':i==13||i==16?':':0;
  if(sep?p[i]!=sep:(p[i]<'0'||p[i]>'9'))return -EINVAL;
 }
 unsigned y=number(p,4),m=number(p+5,2),d=number(p+8,2),h=number(p+11,2),mi=number(p+14,2),s=number(p+17,2);
 static const unsigned md[]={31,28,31,30,31,30,31,31,30,31,30,31};
 if(y<2000||y>2099||m<1||m>12||d<1||d>md[m-1]+(m==2&&leap(y))||h>23||mi>59||s>59)return -ERANGE;
 uint64_t days=0;for(unsigned a=1970;a<y;a++)days+=365+leap(a);
 for(unsigned a=1;a<m;a++)days+=md[a-1]+(a==2&&leap(y));
 uint64_t utc=(days+d-1)*86400+h*3600+mi*60+s-28800; /* Explicit UTC+8, no host TZ. */
 if(utc<UINT64_C(946684800)||utc>UINT64_C(4102444799))return -ERANGE;
 *out=utc;return 0;
}
int prov_phone_time_feed(struct prov_phone_rx*r,const void*data,size_t n,uint64_t now,uint64_t*out){
 if(!r||!data||!n||n>20||!out)return -EINVAL;
 const char*p=data;*out=0;
 if(r->used&&(now<r->began||now-r->began>=10000)){memset(r,0,sizeof(*r));return -ETIMEDOUT;}
 if(!r->used&&r->suffix){
  unsigned suffix=r->suffix;r->suffix=0;
  if(n==1&&p[0]=='\n')return 0;
  if(suffix==1&&n==1&&p[0]=='\r'){r->suffix=2;return 0;}
  if(suffix==1&&n==2&&p[0]=='\r'&&p[1]=='\n')return 0;
 }
 if(!r->used){if(p[0]<'0'||p[0]>'9')return 2;r->began=now;}
 size_t need=19-r->used,take=n<need?n:need;
 memcpy(r->text+r->used,p,take);r->used+=(unsigned)take;
 if(r->used<19)return 0;
 size_t tail=n-take;unsigned suffix=1;
 if(tail){if(tail==1&&p[take]=='\n')suffix=0;
  else if(tail==1&&p[take]=='\r')suffix=2;
  else if(tail==2&&p[take]=='\r'&&p[take+1]=='\n')suffix=0;
  else{memset(r,0,sizeof(*r));return -EPROTO;}}
 int rc=prov_phone_time_parse(r->text,19,out);memset(r,0,sizeof(*r));if(rc)return rc;r->suffix=suffix;return 1;
}
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool sealed,pending;static uint32_t epoch;static uint64_t seconds,received;
static void clear(void){pending=false;epoch=0;seconds=received=0;}
int k7radio_submit_phone_time(uint32_t session,uint64_t utc,uint64_t mono){
 if(!session||mono==UINT64_MAX||utc<UINT64_C(946684800)||utc>UINT64_C(4102444799))return -EINVAL;
 pthread_mutex_lock(&lock);int rc=sealed?-ECANCELED:pending?-EBUSY:0;
 if(!rc){epoch=session;seconds=utc;received=mono;pending=true;}pthread_mutex_unlock(&lock);return rc;
}
void k7radio_phone_time_disconnect(void){pthread_mutex_lock(&lock);clear();pthread_mutex_unlock(&lock);}
void k7radio_close_phone_time(void){pthread_mutex_lock(&lock);sealed=true;clear();pthread_mutex_unlock(&lock);}
int k7radio_take_phone_time(uint64_t*utc,uint64_t*mono){
 if(!utc||!mono)return -EINVAL;
 struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)||ts.tv_sec<0||ts.tv_nsec<0||ts.tv_nsec>=1000000000||(uint64_t)ts.tv_sec>(UINT64_MAX-999)/1000)return -EIO;
 uint64_t now=(uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
 pthread_mutex_lock(&lock);int rc=sealed?-ECANCELED:!pending?-EAGAIN:0;
 /* The same-session sample may arrive while preview is still preparing.
  * Age at most 60s; compensate whole seconds, retaining fractional age for
  * the factory freshness/RTC adjustment. Never relabel old UTC as fresh. */
 if(!rc){
  if(now<received||now-received>60000){clear();rc=-ETIMEDOUT;}
  else{uint64_t elapsed=now-received,delta=elapsed/1000;
   if(seconds>UINT64_C(4102444799)-delta){clear();rc=-ERANGE;}
   else{*utc=seconds+delta;*mono=now-elapsed%1000;sealed=true;clear();}}
 }
 pthread_mutex_unlock(&lock);return rc;
}
