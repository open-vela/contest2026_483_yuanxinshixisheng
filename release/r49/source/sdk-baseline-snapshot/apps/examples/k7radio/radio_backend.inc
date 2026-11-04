/* Target integration candidate. Requires joinable POSIX threads in flat NuttX.
 * All RF work has one lease. Persistent reaper may block in pthread_join, never
 * service pump/LPWORK; a stuck worker leaves the lease held, without forced kill. */
static void rb_dlock(void*p){(void)p;nxmutex_lock(&rb_lock);}
static void rb_dunlock(void*p){(void)p;nxmutex_unlock(&rb_lock);}
static uint64_t rb_now(void*p){(void)p;return now_us()/1000;}
static int rb_enqueue(const struct wb_job*j,uint64_t ticket,bool scan)
{
 nxmutex_lock(&rb_lock);
 if(rb_queued){nxmutex_unlock(&rb_lock);return -EBUSY;}
 rf_init(&rb_facts);rb_facts.keys_clean=rb_facts.unjoined=true;
 rb_job=*j;rb_ticket=ticket;rb_is_scan=scan;rb_queued=true;rb_ip_created=false;
 __atomic_store_n(&rb_cancelled,false,__ATOMIC_RELEASE);
 nxmutex_unlock(&rb_lock);nxsem_post(&rb_jobs);return 0;
}
static int rb_submit(void*p,const struct wb_job*j){(void)p;return rb_enqueue(j,0,false);}
static int rb_submit_scan(void*p,uint64_t id){struct wb_job j;memset(&j,0,sizeof(j));(void)p;return rb_enqueue(&j,id,true);}
static int rb_stop(void*p,uint64_t owner,uint64_t id)
{(void)p;nxmutex_lock(&rb_lock);bool match=rb_queued&&rb_job.owner==owner&&rb_job.id==id;nxmutex_unlock(&rb_lock);if(!match)return -ENOENT;__atomic_store_n(&rb_cancelled,true,__ATOMIC_RELEASE);__atomic_store_n(&g_wifi_network_stop,true,__ATOMIC_RELEASE);return 0;}
static int rb_stop_scan(void*p,uint64_t id)
{(void)p;nxmutex_lock(&rb_lock);bool match=rb_queued&&rb_is_scan&&rb_ticket==id;nxmutex_unlock(&rb_lock);if(!match)return -ENOENT;__atomic_store_n(&rb_cancelled,true,__ATOMIC_RELEASE);return 0;}
static void*rb_operation(void*p)
{
 (void)p;int ret=nxmutex_lock(&g_wifi_operation);
 if(!ret){
  if(rb_stopping())ret=-ECANCELED;
  else if(rb_is_scan){ret=g_wifi_info_valid?0:fw_wifi_info();if(!ret)ret=fw_wifi_scan();}
  else {
   ret=nxmutex_lock(&g_wifi_state);
   if(!ret){memcpy(g_wifi_target_ssid,rb_job.ssid,rb_job.ssid_len);g_wifi_target_ssid_len=rb_job.ssid_len;nxmutex_unlock(&g_wifi_state);
    ret=fw_wifi_join_run(true,rb_job.password);
    nxmutex_lock(&g_wifi_state);memset(g_wifi_target_ssid,0,sizeof(g_wifi_target_ssid));memcpy(g_wifi_target_ssid,"Lansee",6);g_wifi_target_ssid_len=6;nxmutex_unlock(&g_wifi_state);}
  }
  nxmutex_unlock(&g_wifi_operation);
 }
 rb_result=ret;return NULL;
}
static int rb_thread(pthread_t*t,void*(*fn)(void*))
{pthread_attr_t a;int rc=pthread_attr_init(&a);if(rc)return -rc;rc=pthread_attr_setstacksize(&a,16384);if(!rc)rc=pthread_create(t,&a,fn,NULL);pthread_attr_destroy(&a);return -rc;}
static struct rf_facts rb_snapshot(void)
{struct rf_facts f;nxmutex_lock(&rb_lock);f=rb_facts;nxmutex_unlock(&rb_lock);return f;}
static bool rb_wait_rx(bool connect)
{
 uint64_t until=now_us()+2000000;
 do{struct rf_facts f=rb_snapshot();if(connect?rf_connect_releasable(&f):rf_scan_releasable(&f))return true;usleep(1000);}while(now_us()<until);
 return false;
}
static void rb_backend_idle(void)
{nxmutex_lock(&rb_lock);wb_job_clear(&rb_job);rb_queued=false;rb_is_scan=false;rb_ticket=0;nxmutex_unlock(&rb_lock);}
static void rb_post_connect(uint64_t seq,bool success,bool exited,bool up,const uint8_t ip[4])
{
 struct k7wd_event e;memset(&e,0,sizeof(e));struct rf_facts f=rb_snapshot();
 e.kind=WD_COMPLETION;e.done.owner=rb_job.owner;e.done.id=rb_job.id;e.done.sequence=seq;
 e.done.success=success;e.done.authenticated=f.authenticated;e.done.dhcp=success;
 e.done.worker_exited=exited;e.done.link_up=up;if(ip)memcpy(e.done.ip,ip,4);
 if(exited&&!up)rb_backend_idle();
 while(k7wd_post(&rb_dispatch,&e)==WD_FULL)usleep(1000);
}
static void*rb_reaper(void*p)
{
 (void)p;
 for(;;){
  nxsem_wait_uninterruptible(&rb_jobs);pthread_t thread;int rc=rb_thread(&thread,rb_operation);
  bool created=rc==0;
  if(created)rc=-pthread_join(thread,NULL);
  if(rc&&created){/* A create failure has no operation; a join failure may still be live. */
   nxmutex_lock(&rb_lock);rb_join_error=rc;nxmutex_unlock(&rb_lock);
   /* Conservative held: no invented task-exit proof on ambiguous pthread errors. */
   continue;
  }
  if(!created)rb_result=rc;
  nxmutex_lock(&rb_lock);rb_facts.worker_joined=true;
  uint64_t owner=rb_job.owner,job_id=rb_job.id;wb_job_clear(&rb_job);rb_job.owner=owner;rb_job.id=job_id;nxmutex_unlock(&rb_lock);
  if(rb_is_scan){
   bool clean=rb_wait_rx(false);struct rf_facts f=rb_snapshot();struct ws_done e;
   memset(&e,0,sizeof(e));e.ticket=rb_ticket;e.sequence=1;e.success=rb_result==0;e.error=rb_result;e.scan_done=f.scan_done;
   e.stop_ok=!f.touched||f.stop_ok||f.scan_done;e.close_ok=!f.touched||f.close_ok;e.worker_exited=f.worker_joined;
   e.rx_quiescent=clean;e.offline=clean;
   if(clean)rb_backend_idle();
   while(ws_post(&rb_scan,&e)==WD_FULL)usleep(1000);
   if(!clean)continue;
  }else {
   uint8_t ip[4]={0};bool ready=false;
   nxmutex_lock(&g_wifi_ip_state);
   unsigned a,b,c,d;char tail;
   if(!rb_result&&g_wifi_ip_result==0&&sscanf(g_wifi_ip,"%u.%u.%u.%u%c",&a,&b,&c,&d,&tail)==4&&a<256&&b<256&&c<256&&d<256){ip[0]=a;ip[1]=b;ip[2]=c;ip[3]=d;ready=__atomic_load_n(&g_wifi_network_active,__ATOMIC_ACQUIRE);}
   nxmutex_unlock(&g_wifi_ip_state);
   /* Do not publish WB_IP_READY on the first DHCP event alone.  The shared
    * transport used to report success, then tear the IP worker down a moment
    * later on a BLE timeout, leaving the App with a stale "联网成功" state.
    * Hold the lease briefly and publish success only if the worker remains
    * active and the address is still present. */
   if(ready)
     {
       bool stable=true;
       uint64_t until=now_us()+10000000;
       while(now_us()<until)
         {
           nxmutex_lock(&g_wifi_ip_state);
           bool active=__atomic_load_n(&g_wifi_network_active,__ATOMIC_ACQUIRE);
           bool lease=g_wifi_ip_result==0 && g_wifi_ip[0];
           nxmutex_unlock(&g_wifi_ip_state);
           if(!active || !lease || __atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE))
             {stable=false;break;}
           usleep(50000);
         }
       if(stable)rb_post_connect(1,true,true,true,ip);
       else rb_post_connect(1,false,true,false,NULL);
     }
   if(rb_ip_created){if(rb_stopping())__atomic_store_n(&g_wifi_network_stop,true,__ATOMIC_RELEASE);if(pthread_join(rb_ip_thread,NULL))continue;}
   nxmutex_lock(&rb_lock);rb_facts.maintenance_joined=true;nxmutex_unlock(&rb_lock);
   bool clean=rb_wait_rx(true);rb_post_connect(2,false,clean,!clean,NULL);if(!clean)continue;
  }
  /* All clean terminal publishers cleared the slot before queue visibility. */
 }
 return NULL;
}
static void*rb_pump(void*p){(void)p;for(;;){k7wd_pump(&rb_dispatch);usleep(5000);}return NULL;}
#include <sched.h>
static sem_t rb_lifetime=SEM_INITIALIZER(0);
static int rb_start_result=-EINPROGRESS;
/* NuttX removes a task's pthreads when its main exits. Own service threads in
 * a separate persistent task, never in the short-lived NSH command task. */
static int rb_daemon(int argc,char**argv)
{
 (void)argc;(void)argv;
 pthread_t reaper,pump;int rc=rb_thread(&reaper,rb_reaper);
 if(!rc)rc=rb_thread(&pump,rb_pump);
 if(!rc)__atomic_store_n(&rb_ready,true,__ATOMIC_RELEASE);
 __atomic_store_n(&rb_start_result,rc,__ATOMIC_RELEASE);
 /* Also retain partial startup ownership on failure; no detached live worker
  * may outlive its task group. Recovery is an explicit reboot, not fake ready. */
 for(;;)nxsem_wait_uninterruptible(&rb_lifetime);
 return 0;
}
/* Explicit startup only while cold, offline native host owns RX. Legacy CLI is
 * gated before this can race; failed initialization stays gated, not bypassable. */
static int rb_start(void)
{
 if(__atomic_load_n(&rb_enabled,__ATOMIC_ACQUIRE))return -EBUSY;
 if(!g_bt_host_mode||__atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE)||__atomic_load_n(&g_wifi_network_active,__ATOMIC_ACQUIRE)||g_wifi_wpa.active)return -EBUSY;
 bool expected=false;if(!__atomic_compare_exchange_n(&rb_enabled,&expected,true,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -EBUSY;
 /* This entry is only for a pristine native host. STOP before OPEN asserts in
  * the module firmware. Do not invent a close acknowledgement: no RF operation
  * has existed here, and all later operations use the ordinary cleanup fence. */
 int gate=nxmutex_trylock(&g_wifi_operation);if(gate)return -EBUSY;
 nxmutex_lock(&g_wifi_state);
 bool cold=!g_wifi_seq&&!g_wifi_info_valid&&!g_wifi_calibrated&&!g_wifi_waiting;
 nxmutex_unlock(&g_wifi_state);
 nxmutex_unlock(&g_wifi_operation);
 if(!cold)return -EBUSY;
 nxmutex_lock(&rb_lock);rf_init(&rb_facts);rb_facts.worker_joined=true;nxmutex_unlock(&rb_lock);
 struct k7wd_port dp={NULL,rb_dlock,rb_dunlock,rb_now,rb_submit,rb_stop};struct ws_port sp={NULL,rb_submit_scan,rb_stop_scan};
 int rc=k7wd_init(&rb_dispatch,&dp,true);if(!rc)rc=ws_attach(&rb_scan,&rb_dispatch,&sp);if(rc)return rc;
 int pid=task_create("k7_wifi_svc",90,16384,rb_daemon,NULL);
 if(pid<0)return -errno;
 uint64_t until=now_us()+5000000;
 do
   {
     rc=__atomic_load_n(&rb_start_result,__ATOMIC_ACQUIRE);
     if(rc!=-EINPROGRESS)return rc;
     usleep(1000);
   }
 while(now_us()<until);
 return -ETIMEDOUT;
}

struct k7wd_dispatch *k7_wifi_dispatch(void){return __atomic_load_n(&rb_ready,__ATOMIC_ACQUIRE)?&rb_dispatch:NULL;}
struct ws_service *k7_wifi_scans(void){return __atomic_load_n(&rb_ready,__ATOMIC_ACQUIRE)?&rb_scan:NULL;}
