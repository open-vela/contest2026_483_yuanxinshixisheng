/* Native single-owner Wi-Fi data/WPA worker. Shared SDIO RX stays nonblocking. */
#include "skw_netdev.h"
#include "skw_wifi_data.h"
#include "skw_wifi_amsdu.h"
static struct skw_data_replay g_wifi_replay;
static struct skw_amsdu_rx g_wifi_amsdu;
#include "k7_radio_service.h"

static struct skw_supplicant *g_wifi_supplicant;
static sem_t g_wifi_ip_ready=SEM_INITIALIZER(0),g_wifi_ip_done=SEM_INITIALIZER(0);
static mutex_t g_wifi_ip_state=NXMUTEX_INITIALIZER;
static int g_wifi_ip_result;
static char g_wifi_ip[16],g_wifi_gateway[16];
static uint64_t g_wifi_link_generation;
static unsigned g_wifi_ip_rejects;

static void fw_wifi_link_generation_next(void)
{
  if (g_wifi_link_generation != UINT64_MAX) g_wifi_link_generation++;
}

int k7radio_get_link_snapshot(struct k7radio_link_snapshot *snapshot)
{
  unsigned a, b, c, d;
  char tail;
  bool active;
  int ret;

  if (!snapshot) return -EINVAL;
  memset(snapshot, 0, sizeof(*snapshot));
  ret = nxmutex_lock(&g_wifi_ip_state);
  if (ret) return ret;
  snapshot->generation = g_wifi_link_generation;
  active = __atomic_load_n(&g_wifi_network_active, __ATOMIC_ACQUIRE);
  if (active && g_wifi_ip_result == 0 && g_wifi_ip[0] &&
      sscanf(g_wifi_ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 &&
      a > 0 && a < 224 && a != 127 && !(a == 169 && b == 254) &&
      b < 256 && c < 256 && d < 256)
    {
      snapshot->wifi_connected = true;
      snapshot->ipv4_ready = true;
      snapshot->ipv4[0] = (uint8_t)a;
      snapshot->ipv4[1] = (uint8_t)b;
      snapshot->ipv4[2] = (uint8_t)c;
      snapshot->ipv4[3] = (uint8_t)d;
    }
  nxmutex_unlock(&g_wifi_ip_state);
  return 0;
}
static void fw_wifi_net_slot(const uint8_t *slot,size_t n)
{
  if(!g_wifi_replay.active)return;
  int ret=skw_amsdu_receive(&g_wifi_amsdu,&g_wifi_replay,slot,n,
    g_wifi_join_instance,g_wifi_wpa.peer,g_wifi_mac,now_us()/1000,skw_netdev_rx);
  if(ret && ret!=-EPROTONOSUPPORT)
    {
      g_wifi_ip_rejects++;
      if(g_wifi_ip_rejects<=8 && n>=24)
        {
          struct skw_data_view view;
          int parsed=skw_data_inspect(slot,n,g_wifi_join_instance,g_wifi_wpa.peer,g_wifi_mac,&g_wifi_replay,&view);
          unsigned base=g_wifi_replay.pn_reuse?0:4;const uint8_t *d=slot+base;
          printf("WIFI data rejected ret=%d parsed=%d meta=%04x flags=%02x offset=%u index=%u seq=%u lastbyte=%02x wire=%u length=%u\n",ret,parsed,skw_eap_u16(d+10),d[4],d[18],d[19]&63,skw_eap_u16(d+8)&4095,d[19],skw_eap_u16(slot)+4u,skw_eap_u16(d+(g_wifi_replay.pn_reuse?16:2))+6u);
        }
    }
}
static int fw_wifi_net_transmit(const uint8_t *frame,size_t n)
{
  uint8_t data[SKW_NET_FRAME_MAX+8];size_t written;
  int ret=skw_data_encode(g_wifi_wpa.peer,g_wifi_wpa.lmac,frame,n,data,sizeof(data),&written);
  if(!ret)ret=fw_wifi_data_command(data,written);
  return ret;
}
static void fw_wifi_ip_cleanup(void)
{
  nxmutex_lock(&g_wifi_state);g_wifi_replay.active=false;memset(&g_wifi_amsdu,0,sizeof(g_wifi_amsdu));nxmutex_unlock(&g_wifi_state);
  skw_netdev_close();
  if(g_wifi_supplicant){skw_supplicant_stop(g_wifi_supplicant);g_wifi_supplicant=NULL;}
  nxmutex_lock(&g_wifi_state);skw_eapol_clear(&g_wifi_eapol);nxmutex_unlock(&g_wifi_state);
  int keys=skw_wpa_clear_keys(&g_wifi_wpa);uint8_t params[8];
  int unjoin=skw_disconnect_encode(3,params,sizeof(params));
  if(!unjoin)unjoin=fw_wifi_command(17,params,sizeof(params));
  int close=fw_wifi_command(4,NULL,0);if(!close)skw_wpa_closed(&g_wifi_wpa);
  nxmutex_lock(&g_wifi_ip_state);
  if (g_wifi_ip[0]) fw_wifi_link_generation_next();
  memset(g_wifi_ip,0,sizeof(g_wifi_ip));memset(g_wifi_gateway,0,sizeof(g_wifi_gateway));
  nxmutex_unlock(&g_wifi_ip_state);
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
  rb_keys(keys,unjoin);
#endif
  printf("WIFI link cleanup keys=%d unjoin=%d close=%d\n",keys,unjoin,close);
}
static int fw_wifi_ip_worker(int argc,char **argv)
{
  (void)argc;(void)argv;int ret=skw_netdev_dhcp_start();bool announced=false;
  unsigned bt_fault_grace=0;
  uint64_t deadline=now_us()+45000000,last_status=0;
  struct skw_eapol_packet packet;
  while(!ret && !__atomic_load_n(&g_wifi_network_stop,__ATOMIC_ACQUIRE))
    {
      if(__atomic_load_n(&g_wifi_link_lost,__ATOMIC_ACQUIRE)){ret=-ENETDOWN;break;}
      /* A BLE transport timeout can briefly raise the shared BT fault flag
       * while the Wi-Fi lease is already usable.  Do not tear down a valid
       * DHCP session on the first observation; give the shared transport a
       * bounded grace window so an upload in flight is not raced by cleanup.
       */
      if(__atomic_load_n(&g_bt_worker_fault,__ATOMIC_ACQUIRE))
        {
          if(announced && bt_fault_grace++ < 2000)
            {usleep(5000);continue;}
          ret=-EIO;break;
        }
      bt_fault_grace=0;
      nxmutex_lock(&g_wifi_state);int rx=skw_eapol_pop(&g_wifi_eapol,&packet);nxmutex_unlock(&g_wifi_state);
      if(!rx){skw_supplicant_receive(g_wifi_supplicant,packet.data,packet.length);skw_wpa_wipe(&packet,sizeof(packet));}
      else if(rx!=-EAGAIN){ret=rx;break;}
      ret=skw_supplicant_poll(g_wifi_supplicant);if(ret)break;
      ret=skw_netdev_poll(fw_wifi_net_transmit);if(ret)break;
      if(now_us()-last_status>=250000)
        {
          char ip[16],gateway[16];last_status=now_us();
          int lease=skw_netdev_dhcp_status(ip,gateway);
          if(lease<0){ret=lease;break;}
          if(lease==1)
            {
              nxmutex_lock(&g_wifi_ip_state);
              if(!announced)fw_wifi_link_generation_next();
              memcpy(g_wifi_ip,ip,16);memcpy(g_wifi_gateway,gateway,16);g_wifi_ip_result=0;
              nxmutex_unlock(&g_wifi_ip_state);
              if(!announced){announced=true;printf("WIFI DHCP acquired ip=%s gateway=%s wifi_connected=1\n",ip,gateway);nxsem_post(&g_wifi_ip_ready);}
            }
          else if(!announced && now_us()>=deadline){ret=-ETIMEDOUT;break;}
        }
      usleep(5000);
    }
  if(!ret)ret=-ECANCELED;
  nxmutex_lock(&g_wifi_ip_state);g_wifi_ip_result=ret;nxmutex_unlock(&g_wifi_ip_state);
  skw_netdev_stats();fw_wifi_ip_cleanup();
  if(!announced)nxsem_post(&g_wifi_ip_ready);
  printf("WIFI IP worker stopped ret=%d\n",ret);
  nxsem_post(&g_wifi_ip_done);
  __atomic_store_n(&g_wifi_network_active,false,__ATOMIC_RELEASE);return ret;
}
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
static void *rb_ip_entry(void*p){(void)p;fw_wifi_ip_worker(0,NULL);return NULL;}
static int rb_thread(pthread_t*,void*(*)(void*));
#endif
static int fw_wifi_ip_session(struct skw_supplicant *supp)
{
  g_wifi_supplicant=supp;g_wifi_ip_rejects=0;
  while(nxsem_trywait(&g_wifi_ip_ready)==0){}while(nxsem_trywait(&g_wifi_ip_done)==0){}
  nxmutex_lock(&g_wifi_ip_state);g_wifi_ip_result=-EINPROGRESS;nxmutex_unlock(&g_wifi_ip_state);
  __atomic_store_n(&g_wifi_network_stop,false,__ATOMIC_RELEASE);
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
  if(rb_stopping())__atomic_store_n(&g_wifi_network_stop,true,__ATOMIC_RELEASE);
#endif
  __atomic_store_n(&g_wifi_link_lost,false,__ATOMIC_RELEASE);
  __atomic_store_n(&g_wifi_network_active,true,__ATOMIC_RELEASE);
  unsigned group=1;while(group<5 && !(g_wifi_wpa.installed&(1u<<group)))group++;
  int ret=group==5?-ENOKEY:skw_netdev_open(g_wifi_mac);
  if(!ret)
    {
      nxmutex_lock(&g_wifi_state);
      memset(&g_wifi_amsdu,0,sizeof(g_wifi_amsdu));
      ret=skw_data_begin(&g_wifi_replay,g_wifi_pn_reuse,g_wifi_wpa.initial_rsc[0],g_wifi_wpa.initial_rsc[group]);
      nxmutex_unlock(&g_wifi_state);
    }
  if(!ret)
    {
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
      int pid=rb_thread(&rb_ip_thread,rb_ip_entry);
      rb_ip_created=pid==0;
#else
      int pid=kthread_create("skw_wifi_ip",100,16384,fw_wifi_ip_worker,NULL);
#endif
      if(pid<0)ret=pid;
      else
        {
          int wait=nxsem_tickwait_uninterruptible(&g_wifi_ip_ready,MSEC2TICK(55000));
          if(wait)
            {__atomic_store_n(&g_wifi_network_stop,true,__ATOMIC_RELEASE);nxsem_wait_uninterruptible(&g_wifi_ip_done);return wait;}
          nxmutex_lock(&g_wifi_ip_state);ret=g_wifi_ip_result;nxmutex_unlock(&g_wifi_ip_state);
          return ret;
        }
    }
  fw_wifi_ip_cleanup();__atomic_store_n(&g_wifi_network_active,false,__ATOMIC_RELEASE);return ret;
}
static void fw_wifi_ip_status(void)
{
  nxmutex_lock(&g_wifi_ip_state);
  printf("WIFI status active=%u wifi_connected=%u ip=%s gateway=%s result=%d\n",
    __atomic_load_n(&g_wifi_network_active,__ATOMIC_ACQUIRE),!g_wifi_ip_result && g_wifi_ip[0],
    g_wifi_ip[0]?g_wifi_ip:"none",g_wifi_gateway[0]?g_wifi_gateway:"none",g_wifi_ip_result);
  nxmutex_unlock(&g_wifi_ip_state);skw_netdev_stats();
  nxmutex_lock(&g_wifi_state);
  printf("WIFI AMSDU complete=%u subframes=%u rejected=%u\n",g_wifi_amsdu.complete,g_wifi_amsdu.subframes,g_wifi_ip_rejects);
  nxmutex_unlock(&g_wifi_state);
}
