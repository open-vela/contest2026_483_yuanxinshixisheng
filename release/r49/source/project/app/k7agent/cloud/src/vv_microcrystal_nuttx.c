/* SPDX-License-Identifier: Apache-2.0 */
/* Independent candidate adapter. Not enabled in the existing R42 build. */
#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <nuttx/wireless/bluetooth/bt_core.h>
#include <nuttx/wireless/bluetooth/bt_gatt.h>
#include <nuttx/wireless/bluetooth/bt_hci.h>
#include "bt_conn.h"
#include "bt_att.h"
#include <nuttx/wireless/bluetooth/bt_buf.h>
#include <string.h>
#include "vv_microcrystal.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include K7_MICROCRYSTAL_TARGET_HEADER

static atomic_flag busy=ATOMIC_FLAG_INIT;
static _Atomic(struct bt_conn_s *) peer;
/* A single CAS publishes completion for one non-reused transaction. A copied
 * old ATT callback can never clear a newer write, even if the conn slot is
 * reused. Low nine bits are pending(256) or the uint8_t ATT error. */
static atomic_uint_fast64_t write_state;
static uint64_t write_sequence;
static bool write_ids_exhausted;
static atomic_bool output_uncertain;
/* This lease only selects initiating scan parameters. It authorizes no ATT
 * output, and is retired before startup's mandatory 0/1/300ms/1 sequence. */
static atomic_bool manual_fast_connect_scope;
bool vv_microcrystal_manual_fast_connect_target(const bt_addr_le_t *addr){
 static const bt_addr_le_t target=MICRO_TARGET_ADDRESS;
 return addr&&atomic_load(&manual_fast_connect_scope)&&addr->type==target.type&&
   !memcmp(addr->val,target.val,sizeof(target.val));
}
#define MICRO_WRITE_PENDING 256u
#define MICRO_WRITE_STATUS_MASK 511u
#define MICRO_WRITE_ID_MAX (UINT64_MAX >> 9)
_Static_assert(sizeof(uintptr_t)>=sizeof(uint64_t),"64-bit ATT transaction tag required");
static bool poisoned;
static char last_write;
static bool (*cancel_fn)(void *);
static void *cancel_arg;
static pthread_t warm_thread;
static bool warm_active;
static atomic_bool warm_abort;
static int warm_result;
static uint64_t clock_ms(void *arg) {
 (void)arg;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
 return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void sleep_ms(void *arg,unsigned ms){(void)arg;usleep(ms*1000);}
static bool cancelled(void *arg){(void)arg;return atomic_load(&warm_abort)||(cancel_fn&&cancel_fn(cancel_arg));}
static bool connected(void *arg){
 (void)arg;return peer&&__atomic_load_n(&peer->state,__ATOMIC_ACQUIRE)==BT_CONN_CONNECTED&&__atomic_load_n(&peer->att,__ATOMIC_ACQUIRE)!=NULL;
}
static bool detached(struct bt_conn_s *conn){
 return conn&&__atomic_load_n(&conn->state,__ATOMIC_ACQUIRE)==BT_CONN_DISCONNECTED&&
   __atomic_load_n(&conn->att,__ATOMIC_ACQUIRE)==NULL;
}
static void invalidate_write(void){atomic_store(&write_state,0);}
static void write_done(struct bt_conn_s *conn,uint8_t error,const void *pdu,
 uint16_t length,void *tag){
 (void)pdu;(void)length;uint64_t id=(uint64_t)(uintptr_t)tag;
 if(conn!=atomic_load(&peer)||!id)return;
 uint_fast64_t expected=(id<<9)|MICRO_WRITE_PENDING;
 (void)atomic_compare_exchange_strong(&write_state,&expected,(id<<9)|error);
}
/* The adapter holds its conn reference until both the controller state and ATT
 * detach are complete. Protocol retirement does not prove a physical stop. */
static bool release_retired_peer(void){
 struct bt_conn_s *conn=atomic_load(&peer);
 if(!conn)return !poisoned;
 if(!detached(conn))return false;
 invalidate_write();bt_conn_release(conn);atomic_store(&peer,NULL);last_write=0;
 poisoned=write_ids_exhausted;
 printf("MICRO retired protocol_ready=%u output_uncertain=%u physical_ACK=0\n",!poisoned,(unsigned)atomic_load(&output_uncertain));fflush(stdout);
 return !poisoned;
}
static int context_ready(void){
 if(poisoned&&!release_retired_peer())return -EIO;
 return poisoned?-EIO:0;
}

int vv_microcrystal_release_phone(void){
 struct bt_conn_s *other=bt_conn_lookup_state(BT_ADDR_LE_ANY,BT_CONN_CONNECTED);
 if(!other)return 0;
 /* HCI role 1 is the peripheral link accepted from the provisioning phone.
  * Do not disconnect a central-role instrument link here. */
 if(other->role!=1){bt_conn_release(other);return -EBUSY;}
 int rc=bt_conn_disconnect(other,0x13);
 if(!rc)for(unsigned i=0;i<300;i++){
  if(detached(other))break;
  sleep_ms(NULL,10);
 }
 if(!rc&&!detached(other))rc=-ETIMEDOUT;
 bt_conn_release(other);
 printf("MICRO release_phone rc=%d network_and_time_already_ready=1\n",rc);fflush(stdout);return rc;
}
static int connect_target_scoped(void *arg,bool manual_fast){
 (void)arg;uint64_t begin=clock_ms(NULL);int rc=context_ready();
 printf("MICRO_CONNECT begin manual_fast=%u monotonic_ms=%llu\n",manual_fast,(unsigned long long)begin);fflush(stdout);
 if(rc)goto done;
 if(connected(NULL))goto done;
 if(peer){rc=-EBUSY;goto done;}
 /* Network/time are already established. Release only the phone role. */
 rc=vv_microcrystal_release_phone();if(rc)goto done;
 struct bt_conn_s *other=bt_conn_lookup_state(BT_ADDR_LE_ANY,BT_CONN_CONNECTED);
 if(other){bt_conn_release(other);rc=-EBUSY;goto done;}
 static const bt_addr_le_t target=MICRO_TARGET_ADDRESS;
 /* Phone cleanup is outside the lease. Normal SDK state transitions own
  * discovery and initiating; the strong hook only selects target parameters. */
 atomic_store(&manual_fast_connect_scope,manual_fast);
 peer=bt_conn_create_le(&target);if(!peer){rc=-ENOTCONN;goto done;}
 uint64_t waiting=clock_ms(NULL);int previous=-1;rc=-ETIMEDOUT;
 for(unsigned i=0;i<1000;i++){
  int state=(int)__atomic_load_n(&peer->state,__ATOMIC_ACQUIRE);
  if(state!=previous){
   printf("MICRO_CONNECT state=%d att_ready=%u manual_fast=%u monotonic_ms=%llu\n",state,__atomic_load_n(&peer->att,__ATOMIC_ACQUIRE)!=NULL,manual_fast,(unsigned long long)clock_ms(NULL));fflush(stdout);previous=state;
  }
  if(cancelled(NULL)){rc=-ECANCELED;break;}
  if(connected(NULL)){rc=0;break;}
  uint64_t now=clock_ms(NULL);if(now<waiting||now-waiting>=10000)break;
  sleep_ms(NULL,10);
 }
 done:
 atomic_store(&manual_fast_connect_scope,false);
 printf("MICRO_CONNECT return rc=%d manual_fast=%u scope=0 monotonic_ms=%llu\n",rc,manual_fast,(unsigned long long)clock_ms(NULL));fflush(stdout);
 return rc;
}
static int connect_target(void *arg){return connect_target_scoped(arg,false);}
static int write_ascii(void *arg,char value){
 (void)arg;if(!connected(NULL))return -ENOTCONN;
 if((atomic_load(&write_state)&MICRO_WRITE_STATUS_MASK)==MICRO_WRITE_PENDING||poisoned)return -EBUSY;
 if(write_sequence==MICRO_WRITE_ID_MAX){write_ids_exhausted=true;poisoned=true;return -EOVERFLOW;}
 /* Reserve the final tag for a stop. An energizing command cannot consume
  * the last identity and strand a previously active output without a0 tag. */
 if(value!='0'&&write_sequence==MICRO_WRITE_ID_MAX-1){write_ids_exhausted=true;return -EOVERFLOW;}
 uint64_t id=++write_sequence;if(id==MICRO_WRITE_ID_MAX)write_ids_exhausted=true;
 struct bt_conn_s *conn=atomic_load(&peer);
 /* Exact bt_gatt_write request: opcode0x12, little-endian handle, one ASCII
  * byte. bt_att_send consumes buf on success; caller releases on failure. */
 struct bt_buf_s *buf=bt_att_create_pdu(conn,BT_ATT_OP_WRITE_REQ,3);
 if(!buf)return -ENOMEM;
 unsigned char *payload=bt_buf_extend(buf,3);
 payload[0]=(unsigned char)MICRO_TARGET_VALUE_HANDLE;
 payload[1]=(unsigned char)(MICRO_TARGET_VALUE_HANDLE>>8);payload[2]=(unsigned char)value;
 last_write=0;atomic_store(&write_state,(id<<9)|MICRO_WRITE_PENDING);
 if(value!='0')atomic_store(&output_uncertain,true);
 int rc=bt_att_send(conn,buf,write_done,(void *)(uintptr_t)id,NULL);
 if(rc){invalidate_write();bt_buf_release(buf);return rc;}
 uint64_t begin=clock_ms(NULL);
 for(unsigned i=0;i<200;i++){
  uint64_t state=atomic_load(&write_state);
  if((state>>9)!=id)return -EIO;
  if((state&MICRO_WRITE_STATUS_MASK)!=MICRO_WRITE_PENDING){
   rc=(state&MICRO_WRITE_STATUS_MASK)?-EPROTO:0;
   if(!rc){last_write=value;if(value=='0')atomic_store(&output_uncertain,false);}
   printf("MICRO write=%c gatt_rc=%d physical_ACK=0 monotonic_ms=%llu\n",value,rc,(unsigned long long)clock_ms(NULL));fflush(stdout);return rc;
  }
  if(!connected(NULL))break;
  uint64_t now=clock_ms(NULL);if(now<begin||now-begin>=1500)break;
  sleep_ms(NULL,10);
 }
 /* Invalidate before cancellation: an ATT callback already copied by the RX
  * thread still carries the retired id and cannot complete the next write. */
 invalidate_write();bt_gatt_cancel(conn);poisoned=true;
 printf("MICRO write_timeout value=%c id=%llu output_uncertain=%u\n",value,(unsigned long long)id,(unsigned)atomic_load(&output_uncertain));fflush(stdout);
 return -ETIMEDOUT;
}

static bool stop_policy(void *arg){
 (void)arg;
 /* This exact identity/channel was observed to stop by the operator.
  * It is a verified protocol policy, not per-run motor telemetry. */
 return MICRO_TARGET_OPERATOR_VERIFIED&&last_write=='0'&&connected(NULL);
}
static void disconnect_target(void *arg){
 (void)arg;struct bt_conn_s *conn=atomic_load(&peer);if(!conn)return;
 invalidate_write();bt_gatt_cancel(conn);
 enum bt_conn_state_e state=__atomic_load_n(&conn->state,__ATOMIC_ACQUIRE);
 int rc=0;
 if(state!=BT_CONN_DISCONNECTED&&state!=BT_CONN_DISCONNECT)rc=bt_conn_disconnect(conn,0x13);
 for(unsigned i=0;i<300&&!detached(conn);i++)sleep_ms(NULL,10);
 if(!detached(conn)){poisoned=true;printf("MICRO disconnect_uncertain rc=%d output_uncertain=%u\n",rc,(unsigned)atomic_load(&output_uncertain));fflush(stdout);return;}
 (void)release_retired_peer();
}

static void *prepare_worker(void *arg){
 (void)arg;uint64_t start=clock_ms(NULL);warm_result=connect_target(NULL);
 printf("MICRO preconnect_rc=%d elapsed_ms=%llu output_commands=0\n",warm_result,(unsigned long long)(clock_ms(NULL)-start));fflush(stdout);return NULL;
}
int vv_microcrystal_preconnect_start(void){
 if(atomic_flag_test_and_set(&busy))return -EBUSY;
 if(context_ready()){atomic_flag_clear(&busy);return -EIO;}
 if(atomic_load(&output_uncertain)){atomic_flag_clear(&busy);printf("MICRO preconnect_refused reason=stop_unconfirmed output_commands=0\n");fflush(stdout);return -EAGAIN;}
 cancel_fn=NULL;cancel_arg=NULL;atomic_store(&warm_abort,false);warm_result=-EINPROGRESS;
 pthread_attr_t attr;int rc=pthread_attr_init(&attr);
 if(!rc){rc=pthread_attr_setstacksize(&attr,16384);if(!rc)rc=pthread_create(&warm_thread,&attr,prepare_worker,NULL);pthread_attr_destroy(&attr);}
 if(rc){atomic_flag_clear(&busy);return -rc;}
 warm_active=true;printf("MICRO preconnect_started output_commands=0\n");fflush(stdout);return 0;
}
static int prepare_join(bool abort){
 if(!warm_active)return 0;
 if(abort)atomic_store(&warm_abort,true);
 int rc=pthread_join(warm_thread,NULL);
 if(rc){poisoned=true;return -rc;}
 warm_active=false;return warm_result;
}
void vv_microcrystal_preconnect_abort(void){
 if(!warm_active)return;
 int rc=prepare_join(true);
 if(warm_active)return; /* Never release a still-live worker's storage. */
 disconnect_target(NULL);cancel_fn=NULL;cancel_arg=NULL;
 atomic_store(&warm_abort,false);atomic_flag_clear(&busy);
 printf("MICRO preconnect_aborted rc=%d output_commands=0\n",rc);fflush(stdout);
}
int vv_microcrystal_after_report(bool (*cancel)(void *),void *arg){
 if(atomic_load(&output_uncertain)){
  if(warm_active)vv_microcrystal_preconnect_abort();
  printf("MICRO report_refused reason=stop_unconfirmed output_commands=0\n");fflush(stdout);return -EAGAIN;
 }
 if(warm_active){
  int prepared=prepare_join(false);
  if(prepared){if(!warm_active){disconnect_target(NULL);atomic_store(&warm_abort,false);atomic_flag_clear(&busy);}return prepared;}
 }else if(atomic_flag_test_and_set(&busy))return -EBUSY;
 atomic_store(&warm_abort,false);
 cancel_fn=cancel;cancel_arg=arg;last_write=0;
 struct vv_microcrystal_ops ops={NULL,clock_ms,sleep_ms,cancelled,connect_target,connected,write_ascii,stop_policy,disconnect_target};
 struct vv_microcrystal_result result;
 printf("MICRO begin fixed_sequence=1:2000,3:2000,2:3000 then_stop physical_ACK=0\n");fflush(stdout);
 int rc=vv_microcrystal_run(&ops,true,true,&result);
 /* An early admission cancellation may precede the core's own cleanup,
  * while the warm connection already exists. Always retire it here too. */
 if(peer)disconnect_target(NULL);
 if(peer&&!rc)rc=-EIO;
 printf("MICRO complete rc=%d steps=%u stop_written=%u verified_stop_policy=%u output_uncertain=%u elapsed_ms=%llu\n",rc,result.steps_completed,result.stop_written,result.stopped_confirmed,(result.output_may_be_active||atomic_load(&output_uncertain)),(unsigned long long)result.elapsed_ms);fflush(stdout);
 cancel_fn=NULL;cancel_arg=NULL;atomic_flag_clear(&busy);return rc;
}

#include "vv_microcrystal_manual.inc"
