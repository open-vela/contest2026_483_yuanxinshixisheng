/* SPDX-License-Identifier: Apache-2.0 */
/* Included only by the new candidate's BLE adapter. The accepted R44 adapter
 * remains frozen. Existing busy owns every peer access across both modes. */
#include "vv_microcrystal_voice.h"
static pthread_mutex_t manual_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool manual_active;
static unsigned manual_generation;
static unsigned manual_level;
static uint64_t manual_deadline;
#ifndef K7_MICRO_MANUAL_LIMIT_MS
#define K7_MICRO_MANUAL_LIMIT_MS 90000u
#endif
/* Keep stop ASR available while a failed write leaves physical output unknown. */
bool vv_microcrystal_manual_active(void){return atomic_load(&manual_active)||atomic_load(&output_uncertain);}
/* Caller holds manual_lock. Never reconnect merely to continue a session. */
static int manual_stop_locked(void){
 if(!atomic_load(&manual_active))return atomic_load(&output_uncertain)?-EAGAIN:0;
 int rc=connected(NULL)?write_ascii(NULL,'0'):-ENOTCONN;
 bool verified=!rc&&stop_policy(NULL);
 disconnect_target(NULL);
 if(peer&&!rc)rc=-EIO;
 manual_level=0;manual_generation++;
 atomic_store(&manual_active,false);cancel_fn=NULL;cancel_arg=NULL;
 atomic_store(&warm_abort,false);atomic_flag_clear(&busy);
 printf("MICRO_MANUAL stopped rc=%d stop_written=%u verified_stop_policy=%u physical_ACK=0 disconnected=%u\n",rc,verified,verified,peer==NULL);fflush(stdout);
 return rc;
}
static void *manual_watchdog(void *arg){
 unsigned generation=(unsigned)(uintptr_t)arg;
 for(;;){
  sleep_ms(NULL,50);
  pthread_mutex_lock(&manual_lock);
  if(!atomic_load(&manual_active)||manual_generation!=generation){pthread_mutex_unlock(&manual_lock);break;}
  uint64_t now=clock_ms(NULL);
  if(!connected(NULL)||now>=manual_deadline){
   printf("MICRO_MANUAL automatic_stop reason=%s\n",connected(NULL)?"session_limit":"link_lost");fflush(stdout);
   (void)manual_stop_locked();pthread_mutex_unlock(&manual_lock);break;
  }
  pthread_mutex_unlock(&manual_lock);
 }
 return NULL;
}
void vv_microcrystal_manual_shutdown(void){
 if(!atomic_load(&manual_active))return;
 atomic_store(&warm_abort,true);
 pthread_mutex_lock(&manual_lock);(void)manual_stop_locked();pthread_mutex_unlock(&manual_lock);
}
int vv_microcrystal_manual_stop_for_assessment(void){
 pthread_mutex_lock(&manual_lock);int rc=manual_stop_locked();pthread_mutex_unlock(&manual_lock);return rc;
}
int vv_microcrystal_manual_command(enum vv_micro_voice_action action,int value,char *ack,size_t capacity){
 if(!ack||capacity<128)return -EINVAL;
 ack[0]=0;pthread_mutex_lock(&manual_lock);
 bool active=atomic_load(&manual_active),attention=active||atomic_load(&output_uncertain);int rc=0;
 if(action==VV_MICRO_STOP){
  if(!active&&atomic_load(&output_uncertain)){
   /* A new explicit stop request may reconnect only to send 0. The failed
    * session itself never reconnects or resumes energizing output. */
   if(atomic_flag_test_and_set(&busy)){rc=-EBUSY;goto stop_ack;}
   if(context_ready()){atomic_flag_clear(&busy);rc=-EIO;goto stop_ack;}
   cancel_fn=NULL;cancel_arg=NULL;atomic_store(&warm_abort,false);last_write=0;
   atomic_store(&manual_active,true);manual_generation++;
   rc=connect_target(NULL);int stopped=manual_stop_locked();if(!rc)rc=stopped;
  }else rc=manual_stop_locked();
 stop_ack:
  snprintf(ack,capacity,"%s",rc?"微晶停机未确认，请检查设备。":attention?"已停止。":"微晶当前未启动。");
  goto done;
 }
 if(action==VV_MICRO_START){
  if(active){if(!connected(NULL)){rc=-ENOTCONN;(void)manual_stop_locked();goto rejected;}snprintf(ack,capacity,"%s档。",manual_level==1?"一":manual_level==2?"二":"三");goto done;}
  if(atomic_flag_test_and_set(&busy)){rc=-EBUSY;goto rejected;}
  if(context_ready()){atomic_flag_clear(&busy);rc=-EIO;goto rejected;}
  cancel_fn=NULL;cancel_arg=NULL;atomic_store(&warm_abort,false);last_write=0;
  atomic_store(&manual_active,true);manual_level=0;manual_generation++;
  rc=connect_target_scoped(NULL,true);
  if(!rc&&!cancelled(NULL))rc=write_ascii(NULL,'0');
  if(!rc&&!cancelled(NULL))rc=write_ascii(NULL,'1');
  if(!rc){sleep_ms(NULL,300);if(cancelled(NULL))rc=-ECANCELED;}
  if(!rc)rc=write_ascii(NULL,'1');
  if(!rc){
   manual_level=1;manual_deadline=clock_ms(NULL)+K7_MICRO_MANUAL_LIMIT_MS;
   pthread_attr_t attr;pthread_t thread;int pr=pthread_attr_init(&attr);
   if(!pr){pr=pthread_attr_setstacksize(&attr,16384);if(!pr)pr=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!pr)pr=pthread_create(&thread,&attr,manual_watchdog,(void *)(uintptr_t)manual_generation);pthread_attr_destroy(&attr);}
   if(pr)rc=-pr;
  }
  if(rc){int stop_rc=manual_stop_locked();printf("MICRO_MANUAL start_failed rc=%d cleanup=%d\n",rc,stop_rc);fflush(stdout);goto rejected;}
  snprintf(ack,capacity,"一档。");
  printf("MICRO_MANUAL started level=1 limit_ms=%u\n",(unsigned)K7_MICRO_MANUAL_LIMIT_MS);fflush(stdout);goto done;
 }
 if(action!=VV_MICRO_LEVEL&&action!=VV_MICRO_RELATIVE){rc=-EINVAL;goto rejected;}
 if(!active){snprintf(ack,capacity,"请先说启动微晶。");rc=2;goto done;}
 if(action==VV_MICRO_RELATIVE)value=(int)manual_level+value;
 if(value<1||value>3){snprintf(ack,capacity,"微晶只有一档到三档。");rc=2;goto done;}
 if(!connected(NULL)){rc=-ENOTCONN;(void)manual_stop_locked();goto rejected;}
 rc=write_ascii(NULL,(char)('0'+value));
 if(rc){(void)manual_stop_locked();goto rejected;}
 manual_level=(unsigned)value;
 /* One fixed session cap; repeated commands do not extend unattended output. */
 snprintf(ack,capacity,"%s档。",value==1?"一":value==2?"二":"三");
 printf("MICRO_MANUAL level=%d result=0\n",value);fflush(stdout);goto done;
 rejected:
 snprintf(ack,capacity,"%s",rc==-EBUSY?"护理方案正在执行，请稍候。":"微晶操作未完成，请检查设备。");
 done:
 printf("MICRO_MANUAL command=%d rc=%d active=%u level=%u output_uncertain=%u\n",action,rc,(unsigned)atomic_load(&manual_active),manual_level,(unsigned)atomic_load(&output_uncertain));fflush(stdout);
 pthread_mutex_unlock(&manual_lock);
 /* Recognized operational failures stay in this control branch, never chat. */
 return rc==0?1:2;
}
