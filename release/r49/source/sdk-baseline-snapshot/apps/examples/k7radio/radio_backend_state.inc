/* Included once, only CONFIG_EXAMPLES_K7RADIO_SHARED. */
#include <pthread.h>
#include "wifi_scan.h"
#include "radio_fence.h"
static mutex_t rb_lock=NXMUTEX_INITIALIZER;
static sem_t rb_jobs=SEM_INITIALIZER(0);
static struct k7wd_dispatch rb_dispatch;
static struct ws_service rb_scan;
static struct rf_facts rb_facts;
static struct wb_job rb_job;
static uint64_t rb_ticket,rb_epoch;
static bool rb_ready,rb_enabled,rb_queued,rb_is_scan,rb_cancelled,rb_ip_created;
static pthread_t rb_ip_thread;
static int rb_result;
static int rb_join_error; /* reaper-owned; never alias live worker result */
static void rb_note(unsigned command,int rc)
{nxmutex_lock(&rb_lock);if(__atomic_load_n(&rb_enabled,__ATOMIC_ACQUIRE))rf_command(&rb_facts,command,rc,rb_epoch);nxmutex_unlock(&rb_lock);}
/* Called only by the sole actual RX task, outside g_wifi_state, between packets.
 * All report callbacks in the previous packet have synchronously returned. */
static void rb_rx_idle(void)
{nxmutex_lock(&rb_lock);if(rb_epoch!=UINT64_MAX)++rb_epoch;else{rb_facts.callbacks_exited=false;nxmutex_unlock(&rb_lock);return;}if(__atomic_load_n(&rb_enabled,__ATOMIC_ACQUIRE)&&rb_facts.close_ok){rb_facts.rx_empty=true;rb_facts.empty_epoch=rb_epoch;rb_facts.callbacks_exited=true;}nxmutex_unlock(&rb_lock);}
static void rb_rx_done(void)
{nxmutex_lock(&rb_lock);if(__atomic_load_n(&rb_enabled,__ATOMIC_ACQUIRE))rb_facts.scan_done=true;nxmutex_unlock(&rb_lock);}
static void rb_rx_report(const uint8_t*p,size_t n)
{
 struct skw_bss b;struct sc_record r;uint64_t id=0;
 if(skw_bss_decode(p,n,&b))return;
 nxmutex_lock(&rb_lock);if(__atomic_load_n(&rb_enabled,__ATOMIC_ACQUIRE)&&rb_is_scan)id=rb_ticket;nxmutex_unlock(&rb_lock);
 if(!id)return;
 memset(&r,0,sizeof(r));memcpy(r.ssid,b.scan.ssid,32);r.ssid_len=b.scan.ssid_len;
 memcpy(r.bssid,b.scan.bssid,6);r.rssi=b.scan.rssi;r.channel=b.scan.channel;r.band=b.scan.band;
 r.security=b.rsn?2:(b.wpa?1:((b.capability&16)?3:0));(void)ws_record(&rb_scan,id,&r);
}
static void rb_keys(int keys,int unjoin)
{nxmutex_lock(&rb_lock);if(__atomic_load_n(&rb_enabled,__ATOMIC_ACQUIRE)){rb_facts.keys_clean=keys==0;rb_facts.unjoined=unjoin==0;}nxmutex_unlock(&rb_lock);}
static void rb_authenticated(bool ok)
{nxmutex_lock(&rb_lock);if(__atomic_load_n(&rb_enabled,__ATOMIC_ACQUIRE))rb_facts.authenticated=ok;nxmutex_unlock(&rb_lock);}
static bool rb_stopping(void){return __atomic_load_n(&rb_cancelled,__ATOMIC_ACQUIRE);}
static int rb_start(void);
