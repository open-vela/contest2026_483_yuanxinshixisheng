/* SPDX-License-Identifier: Apache-2.0 */
/* Native flat-build adapter, pinned to this tree's Bluetooth host APIs.
 * HCI connection/ATT events and notifications execute on single LPWORK.
 * Long Wi-Fi scans and JSON work execute on the separate provisioning task.
 */
#include <nuttx/wqueue.h>
#include <nuttx/wireless/bluetooth/bt_gatt.h>
#include "bt_hcicore.h"
#include "bt_conn.h"
#include "bt_att.h"
#include "prov_protocol.h"
#include "prov_phone_time.h"
#include "prov_wifi.inc"
#if CONFIG_SCHED_LPNTHREADS != 1
#error "Provisioning notification serialization requires one LPWORK thread"
#endif

#define PROV_TX_HANDLE 10
static mutex_t g_prov_lock=NXMUTEX_INITIALIZER;
static sem_t g_prov_event=SEM_INITIALIZER(0),g_prov_sent=SEM_INITIALIZER(0);
static struct bt_conn_s *g_prov_conn;
static struct prov_rx g_prov_rx;
static struct prov_phone_rx g_prov_phone_rx;
static char g_prov_job[PROV_MAX_CMD+1];
static bool g_prov_registered,g_prov_subscribed,g_prov_busy,g_prov_pending,g_prov_scanning;
static unsigned int g_prov_epoch,g_prov_job_epoch,g_prov_id,g_prov_commands,g_prov_fragments,g_prov_errors;
static unsigned int g_prov_connects,g_prov_disconnects,g_prov_reads;
/* Retain only request metadata to diagnose a missed frontend reply safely. */
static unsigned int g_prov_last_command,g_prov_last_id,g_prov_last_session;
static int g_prov_last_result,g_prov_last_notify;
struct prov_ap { struct skw_scan_record scan; uint8_t security; };
#ifndef CONFIG_EXAMPLES_K7RADIO_SHARED
static struct prov_ap g_prov_aps[64];
static unsigned int g_prov_ap_count;
#endif
static bool g_prov_truncated;
static struct { struct work_s work; unsigned int epoch; size_t len; uint8_t data[20]; int ret; bool terminal; } g_prov_notify;

static uint32_t prov_now(void)
{ return (uint32_t)TICK2MSEC(clock_systime_ticks()); }

static void prov_connected(struct bt_conn_s *conn,void *context)
{
 (void)context;
 nxmutex_lock(&g_prov_lock);
 if(g_prov_conn) { nxmutex_unlock(&g_prov_lock);return; }
 g_prov_conn=bt_conn_addref(conn);g_prov_epoch++;g_prov_connects++;
 g_prov_subscribed=false;prov_rx_reset(&g_prov_rx);
 nxmutex_unlock(&g_prov_lock);
 int ret=bt_conn_security(conn,BT_SECURITY_MEDIUM);
 printf("PROV connected security_request=%d\n",ret);
}
static void prov_disconnected(struct bt_conn_s *conn,void *context)
{
 (void)context;
 nxmutex_lock(&g_prov_lock);
 if(conn==g_prov_conn)
   {
    k7radio_phone_time_disconnect();memset(&g_prov_phone_rx,0,sizeof(g_prov_phone_rx));
    g_prov_conn=NULL;g_prov_epoch++;g_prov_disconnects++;g_prov_subscribed=false;
    prov_rx_reset(&g_prov_rx);prov_zero(g_prov_job,sizeof(g_prov_job));
    g_prov_pending=false;g_prov_scanning=false;
    /* Worker owns busy until cancelled operation finishes. No stale result
     * is delivered to a new connection, even when it reuses the same address. */
    bt_conn_release(conn);
   }
 nxmutex_unlock(&g_prov_lock);
 printf("PROV disconnected; input wiped; subscribe again after reconnect\n");
}
static struct bt_conn_cb_s g_prov_callbacks={.connected=prov_connected,.disconnected=prov_disconnected};

static int prov_read_name(struct bt_conn_s *conn,const struct bt_gatt_attr_s *attr,void *buf,uint8_t len,uint16_t offset)
{ static const char name[]="VelaVision K7";return bt_gatt_attr_read(conn,attr,buf,len,offset,name,sizeof(name)-1); }
static int prov_read_appearance(struct bt_conn_s *conn,const struct bt_gatt_attr_s *attr,void *buf,uint8_t len,uint16_t offset)
{ static const uint8_t value[2]={0};return bt_gatt_attr_read(conn,attr,buf,len,offset,value,2); }
static int prov_read_status(struct bt_conn_s *conn,const struct bt_gatt_attr_s *attr,void *buf,uint8_t len,uint16_t offset)
{
 char value[180];
 nxmutex_lock(&g_prov_lock);g_prov_reads++;
 int n=snprintf(value,sizeof(value),"{\"v\":1,\"scan\":true,\"receive_credentials\":true,\"connect\":" PROV_CONNECT_CAP ",\"encrypted\":%s,\"busy\":%s,\"session\":%u,\"commands\":%u}\n",
  conn->encrypt?"true":"false",g_prov_busy?"true":"false",g_prov_epoch,g_prov_commands);
 nxmutex_unlock(&g_prov_lock);
 return bt_gatt_attr_read(conn,attr,buf,len,offset,value,n);
}
static int prov_read_ccc(struct bt_conn_s *conn,const struct bt_gatt_attr_s *attr,void *buf,uint8_t len,uint16_t offset)
{
 uint8_t value[2]={0};nxmutex_lock(&g_prov_lock);
 value[0]=(conn==g_prov_conn && g_prov_subscribed)?1:0;nxmutex_unlock(&g_prov_lock);
 return bt_gatt_attr_read(conn,attr,buf,len,offset,value,2);
}
static int prov_write_ccc(struct bt_conn_s *conn,const struct bt_gatt_attr_s *attr,const void *buf,uint8_t len,uint16_t offset)
{
 const uint8_t *p=buf;(void)attr;
 if(offset || len!=2 || p[1] || p[0]>1) return -EINVAL;
 nxmutex_lock(&g_prov_lock);
 if(conn!=g_prov_conn) { nxmutex_unlock(&g_prov_lock);return -ENOTCONN; }
 g_prov_subscribed=p[0]!=0;
 if(!g_prov_subscribed) { k7radio_phone_time_disconnect();memset(&g_prov_phone_rx,0,sizeof(g_prov_phone_rx));prov_rx_reset(&g_prov_rx);g_prov_epoch++; }
 nxmutex_unlock(&g_prov_lock);return len;
}
static int prov_write_command(struct bt_conn_s *conn,const struct bt_gatt_attr_s *attr,const void *buf,uint8_t len,uint16_t offset)
{
 int ret;(void)attr;
 if(offset) return -EINVAL;
 nxmutex_lock(&g_prov_lock);
 if(conn!=g_prov_conn || !g_prov_subscribed || !conn->encrypt)
   { nxmutex_unlock(&g_prov_lock);return -EACCES; }
 /* Phone date frames are independent of a still-finishing Wi-Fi job. No RTC
  * writes or slow network/audio operations run in this ATT callback. */
 if(!g_prov_rx.used&&!g_prov_rx.discard){
  uint64_t utc=0,mono=now_us()/1000;
  ret=prov_phone_time_feed(&g_prov_phone_rx,buf,len,mono,&utc);
  if(ret!=2){
   if(ret==1){ret=k7radio_submit_phone_time(g_prov_epoch,utc,mono);
    printf("TIME phone_submit rc=%d session=%u\n",ret,g_prov_epoch);}
   else if(ret<0)printf("TIME phone_frame rc=%d\n",ret);
   nxmutex_unlock(&g_prov_lock);return ret<0?ret:len;
  }
 }
 if(g_prov_busy) { nxmutex_unlock(&g_prov_lock);return -EBUSY; }
 ret=prov_rx_feed(&g_prov_rx,buf,len,prov_now());
 if(ret==1)
   {
    memcpy(g_prov_job,g_prov_rx.data,g_prov_rx.used+1);prov_rx_reset(&g_prov_rx);
    g_prov_busy=true;g_prov_pending=true;g_prov_job_epoch=g_prov_epoch;
    nxsem_post(&g_prov_event);
   }
 if(ret<0) g_prov_errors++;
 nxmutex_unlock(&g_prov_lock);
 return ret<0?ret:len;
}
/* UUID bytes are little-endian on ATT, matching the published 128-bit UUID. */
#define PROV_UUID(n) {.type=BT_UUID_128,.u={.u128={0x80,0x46,0xd7,0xc2,0xcb,0x3e,0x2f,0x9c,0x2e,0x4f,0xc3,0x78,n,0,0x7a,0x6b}}}
static struct bt_uuid_s g_prov_service=PROV_UUID(1),g_prov_rx_uuid=PROV_UUID(2),g_prov_tx_uuid=PROV_UUID(3),g_prov_status_uuid=PROV_UUID(4);
static struct bt_uuid_s g_prov_gap={.type=BT_UUID_16,.u={.u16=BT_UUID_GAP}};
static struct bt_uuid_s g_prov_name_uuid={.type=BT_UUID_16,.u={.u16=BT_UUID_GAP_DEVICE_NAME}};
static struct bt_uuid_s g_prov_app_uuid={.type=BT_UUID_16,.u={.u16=BT_UUID_GAP_APPEARANCE}};
static struct bt_uuid_s g_prov_ccc_uuid={.type=BT_UUID_16,.u={.u16=BT_UUID_GATT_CCC}};
static struct bt_gatt_chrc_s g_prov_name_ch={.uuid=&g_prov_name_uuid,.value_handle=3,.properties=BT_GATT_CHRC_READ};
static struct bt_gatt_chrc_s g_prov_app_ch={.uuid=&g_prov_app_uuid,.value_handle=5,.properties=BT_GATT_CHRC_READ};
static struct bt_gatt_chrc_s g_prov_rx_ch={.uuid=&g_prov_rx_uuid,.value_handle=8,.properties=BT_GATT_CHRC_WRITE};
static struct bt_gatt_chrc_s g_prov_tx_ch={.uuid=&g_prov_tx_uuid,.value_handle=10,.properties=BT_GATT_CHRC_NOTIFY};
static struct bt_gatt_chrc_s g_prov_status_ch={.uuid=&g_prov_status_uuid,.value_handle=13,.properties=BT_GATT_CHRC_READ};
static const struct bt_gatt_attr_s g_prov_attrs[]={
 BT_GATT_PRIMARY_SERVICE(1,&g_prov_gap),
 BT_GATT_CHARACTERISTIC(2,&g_prov_name_ch),
 BT_GATT_DESCRIPTOR(3,&g_prov_name_uuid,BT_GATT_PERM_READ,prov_read_name,NULL,NULL),
 BT_GATT_CHARACTERISTIC(4,&g_prov_app_ch),
 BT_GATT_DESCRIPTOR(5,&g_prov_app_uuid,BT_GATT_PERM_READ,prov_read_appearance,NULL,NULL),
 BT_GATT_PRIMARY_SERVICE(6,&g_prov_service),
 BT_GATT_CHARACTERISTIC(7,&g_prov_rx_ch),
 BT_GATT_DESCRIPTOR(8,&g_prov_rx_uuid,BT_GATT_PERM_WRITE|BT_GATT_PERM_WRITE_ENCRYPT,NULL,prov_write_command,NULL),
 BT_GATT_CHARACTERISTIC(9,&g_prov_tx_ch),
 BT_GATT_DESCRIPTOR(10,&g_prov_tx_uuid,0,NULL,NULL,NULL),
 BT_GATT_DESCRIPTOR(11,&g_prov_ccc_uuid,BT_GATT_PERM_READ|BT_GATT_PERM_WRITE,prov_read_ccc,prov_write_ccc,NULL),
 BT_GATT_CHARACTERISTIC(12,&g_prov_status_ch),
 BT_GATT_DESCRIPTOR(13,&g_prov_status_uuid,BT_GATT_PERM_READ,prov_read_status,NULL,NULL)
};

static void prov_notify_work(void *arg)
{
 (void)arg;
 /* Same single LPWORK thread as HCI disconnect and ATT processing. */
 nxmutex_lock(&g_prov_lock);
 struct bt_conn_s *conn=g_prov_conn;
 bool valid=conn && g_prov_epoch==g_prov_notify.epoch && g_prov_subscribed && conn->encrypt;
 nxmutex_unlock(&g_prov_lock);
 g_prov_notify.ret=-ENOTCONN;
 if(valid && conn->state==BT_CONN_CONNECTED && conn->att)
   {
    struct bt_buf_s *buf=bt_att_create_pdu(conn,BT_ATT_OP_NOTIFY,2+g_prov_notify.len);
    if(buf)
      {
       uint8_t *p=bt_buf_extend(buf,2+g_prov_notify.len);
       p[0]=PROV_TX_HANDLE;p[1]=0;memcpy(p+2,g_prov_notify.data,g_prov_notify.len);
       g_prov_notify.ret=bt_att_send(conn,buf,NULL,NULL,NULL);
       if(g_prov_notify.ret) bt_buf_release(buf);
      }
    else g_prov_notify.ret=-ENOMEM;
   }
 /* ATT writes and this completion share one LPWORK. Publish readiness before
  * the peer can process the terminal notification and issue its next command.
  * Only a terminal fragment releases busy; intermediate scan rows do not. */
 if(valid && g_prov_notify.terminal && !g_prov_notify.ret)
   { nxmutex_lock(&g_prov_lock);g_prov_busy=false;nxmutex_unlock(&g_prov_lock); }
 nxsem_post(&g_prov_sent);
}
static int prov_send(unsigned int epoch,const char *line,bool terminal)
{
 size_t len=strlen(line);
 for(size_t pos=0;pos<len;)
   {
    g_prov_notify.epoch=epoch;g_prov_notify.len=len-pos>20?20:len-pos;
    g_prov_notify.terminal=terminal && pos+g_prov_notify.len==len;
    memcpy(g_prov_notify.data,line+pos,g_prov_notify.len);
    int ret=work_queue(LPWORK,&g_prov_notify.work,prov_notify_work,NULL,0);
    if(ret) return ret;
    ret=nxsem_wait_uninterruptible(&g_prov_sent);
    if(ret) return ret;
    if(g_prov_notify.ret) return g_prov_notify.ret;
    __atomic_add_fetch(&g_prov_fragments,1,__ATOMIC_RELAXED);
    pos+=g_prov_notify.len;if(pos<len || !terminal) usleep(20000);
   }
 return 0; /* Queued notifications; only client receipt proves delivery. */
}
#ifndef CONFIG_EXAMPLES_K7RADIO_SHARED
static void prov_scan_report(const uint8_t *payload,size_t length)
{
 struct skw_bss b;
 if(skw_bss_decode(payload,length,&b)) return;
 nxmutex_lock(&g_prov_lock);
 if(!g_prov_scanning) { nxmutex_unlock(&g_prov_lock);return; }
 for(unsigned int i=0;i<g_prov_ap_count;i++)
   if(!memcmp(g_prov_aps[i].scan.bssid,b.scan.bssid,6))
     { if(b.scan.rssi>g_prov_aps[i].scan.rssi) g_prov_aps[i].scan=b.scan;nxmutex_unlock(&g_prov_lock);return; }
 if(g_prov_ap_count==64) g_prov_truncated=true;
 else
   { struct prov_ap *ap=&g_prov_aps[g_prov_ap_count++];ap->scan=b.scan;
     ap->security=b.rsn?2:(b.wpa?1:((b.capability&16)?3:0)); }
 nxmutex_unlock(&g_prov_lock);
}
#endif
static int prov_worker(int argc,char **argv)
{
 char command[PROV_MAX_CMD+1],line[240];
 (void)argc;(void)argv;
 for(;;)
   {
    nxsem_tickwait_uninterruptible(&g_prov_event,MSEC2TICK(100));
    nxmutex_lock(&g_prov_lock);
    if((g_prov_rx.used || g_prov_rx.discard) && (uint32_t)(prov_now()-g_prov_rx.started)>=10000) prov_rx_reset(&g_prov_rx);
    if(!g_prov_pending) { g_prov_busy=false;nxmutex_unlock(&g_prov_lock);continue; }
    unsigned int epoch=g_prov_job_epoch;
    memcpy(command,g_prov_job,sizeof(command));prov_zero(g_prov_job,sizeof(g_prov_job));g_prov_pending=false;
    g_prov_commands++;
    nxmutex_unlock(&g_prov_lock);
    struct prov_request req;int ret=prov_parse(command,&req);prov_zero(command,sizeof(command));
    if(!req.id) req.id=++g_prov_id;
    if(ret) snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"session\":%u,\"event\":\"error\",\"code\":\"%s\"}\n",req.id,epoch,
      req.error==PROV_INVALID_SSID?"invalid_ssid":(req.error==PROV_INVALID_PASSWORD?"invalid_password":"invalid_command"));
    else if(req.command==PROV_CREDENTIALS)
      {
       /* This phase validates reception only. No persistence, WPA or DHCP.
        * Wipe plaintext before replying; a later reconnect must resubmit. */
       prov_zero(req.password,sizeof(req.password));
       prov_zero(req.ssid,sizeof(req.ssid));req.ssid_len=0;
       snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"session\":%u,\"event\":\"credentials_received\",\"validated\":true,\"stored\":false,\"wifi_connected\":false,\"ip\":null}\n",req.id,epoch);
      }
    else if(req.command==PROV_CONNECT)
      {
       snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"session\":%u,\"event\":\"wifi_connecting\"}\n",req.id,epoch);
       ret=prov_send(epoch,line,false);
       if(!ret)ret=prov_wifi_connect(&req);
       prov_zero(req.password,sizeof(req.password));prov_zero(req.ssid,sizeof(req.ssid));
       char ip[16],quoted[20];bool online=prov_wifi_snapshot(ip);
       if(online)snprintf(quoted,sizeof(quoted),"\"%s\"",ip);else strcpy(quoted,"null");
       if(!ret && !online)ret=-ENETDOWN;
       if(ret)snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"session\":%u,\"event\":\"error\",\"code\":\"%s\",\"detail\":%d,\"wifi_connected\":%s,\"ip\":%s}\n",req.id,epoch,prov_wifi_error(ret),ret,online?"true":"false",quoted);
       else snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"session\":%u,\"event\":\"wifi_connected\",\"wifi_connected\":true,\"ip\":%s,\"stored\":false}\n",req.id,epoch,quoted);
      }
    else if(req.command==PROV_STATUS)
      {
       char ip[16],quoted[20];bool online=prov_wifi_snapshot(ip);
       if(online)snprintf(quoted,sizeof(quoted),"\"%s\"",ip);else strcpy(quoted,"null");
       snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"event\":\"status\",\"wifi_connected\":%s,\"ip\":%s,\"session\":%u}\n",req.id,online?"true":"false",quoted,epoch);
      }
    else
      {
       snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"event\":\"scan_started\"}\n",req.id);
       ret=prov_send(epoch,line,false);
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
       struct vs_snapshot snapshot;memset(&snapshot,0,sizeof(snapshot));
       uint64_t ticket=0;
       if(!ret&&!__atomic_load_n(&rb_ready,__ATOMIC_ACQUIRE))ret=-ENODEV;
       if(!ret){enum k7wd_rc rc=ws_ble_begin(&rb_scan,30000,&ticket);if(rc!=WD_OK)ret=rc==WD_BUSY?-EBUSY:-EIO;}
       uint64_t until=now_us()+31000000;
       while(!ret){
        if(ws_ble_poll(&rb_scan,ticket,&snapshot)!=WD_OK){ret=-EIO;break;}
        if(snapshot.state==VS_DONE)break;
        if(snapshot.state!=VS_PENDING){ret=snapshot.error?snapshot.error:(snapshot.state==VS_BUSY?-EBUSY:snapshot.state==VS_UNSUPPORTED?-ENOTSUP:-EIO);break;}
        if(now_us()>=until){ws_ble_cancel(&rb_scan,ticket);ret=-ETIMEDOUT;break;}usleep(5000);
       }
       unsigned int sent=0;
       for(unsigned int i=0;!ret&&i<snapshot.count;i++){
        const struct vs_ap *ap=&snapshot.ap[i];char b64[45];prov_base64(ap->ssid,ap->ssid_len,b64);
        snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"event\":\"ap\",\"ssid_b64\":\"%s\",\"rssi\":%d,\"security\":\"%s\",\"band\":\"%s\",\"channel\":%u}\n",req.id,b64,ap->rssi,
         ap->security==2?"rsn":ap->security==1?"wpa":ap->security==3?"protected":"open",ap->band?"5G":"2.4G",ap->channel);
        ret=prov_send(epoch,line,false);if(!ret)sent++;
       }
       g_prov_truncated=snapshot.truncated;
#else
       if(!ret)
         {
          ret=nxmutex_lock(&g_wifi_operation);
          if(!ret)
            {
             nxmutex_lock(&g_prov_lock);g_prov_scanning=g_prov_epoch==epoch;g_prov_ap_count=0;g_prov_truncated=false;nxmutex_unlock(&g_prov_lock);
             ret=!g_wifi_info_valid?fw_wifi_info():0;
             if(!ret) ret=fw_wifi_scan();
             nxmutex_lock(&g_prov_lock);g_prov_scanning=false;nxmutex_unlock(&g_prov_lock);
             nxmutex_unlock(&g_wifi_operation);
            }
         }
       unsigned int sent=0;
       for(unsigned int i=0;!ret && i<g_prov_ap_count;i++)
         {
          struct prov_ap *ap=&g_prov_aps[i];char b64[45];prov_base64(ap->scan.ssid,ap->scan.ssid_len,b64);
          snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"event\":\"ap\",\"ssid_b64\":\"%s\",\"rssi\":%d,\"security\":\"%s\",\"band\":\"%s\",\"channel\":%u}\n",req.id,b64,ap->scan.rssi,
           ap->security==2?"rsn":(ap->security==1?"wpa":(ap->security==3?"protected":"open")),ap->scan.band?"5G":"2.4G",ap->scan.channel);
          ret=prov_send(epoch,line,false);if(!ret) sent++;
         }
#endif
       if(ret) snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"event\":\"error\",\"code\":\"scan_failed\",\"detail\":%d}\n",req.id,ret);
       else snprintf(line,sizeof(line),"{\"v\":1,\"id\":%u,\"event\":\"scan_done\",\"count\":%u,\"truncated\":%s}\n",req.id,sent,g_prov_truncated?"true":"false");
      }
    int txret=prov_send(epoch,line,true);
    nxmutex_lock(&g_prov_lock);
    /* A new complete request may already be queued after terminal delivery. */
    if(!g_prov_pending) g_prov_busy=false;
    if(ret || txret) g_prov_errors++;
    g_prov_last_command=req.command;g_prov_last_id=req.id;g_prov_last_session=epoch;
    g_prov_last_result=ret;g_prov_last_notify=txret;
    nxmutex_unlock(&g_prov_lock);
    printf("PROV command=%u id=%u ret=%d notify_ret=%d\n",req.command,req.id,ret,txret);
    prov_zero(&req,sizeof(req));
   }
 return 0;
}
static int prov_start(void)
{
 if(g_prov_registered) return 0;
 int ret=bt_stop_advertising();
 if(ret) return ret;
 nxmutex_lock(&g_native_bt.lock);bool connected=g_native_bt.connected;nxmutex_unlock(&g_native_bt.lock);
 if(connected) return -EBUSY;
 prov_protocol_init();
 int pid=kthread_create("k7_provision",85,8192,prov_worker,NULL);
 if(pid<0) return pid;
 bt_gatt_register(g_prov_attrs,sizeof(g_prov_attrs)/sizeof(g_prov_attrs[0]));
 bt_conn_cb_register(&g_prov_callbacks);g_prov_registered=true;
 static const struct bt_eir_s ad[]={
  {.len=2,.type=BT_EIR_FLAGS,.data={BT_LE_AD_GENERAL|BT_LE_AD_NO_BREDR}},
  {.len=17,.type=0x07,.data={0x80,0x46,0xd7,0xc2,0xcb,0x3e,0x2f,0x9c,0x2e,0x4f,0xc3,0x78,1,0,0x7a,0x6b}},
  {0}};
 static const struct bt_eir_s sd[]={ {.len=14,.type=BT_EIR_NAME_COMPLETE,.data="VelaVision K7"},{0}};
 ret=bt_start_advertising(BT_LE_ADV_IND,ad,sd);
 printf("PROV service ready version=1 write=8 notify=10 status=13 scan=1 receive_credentials=1 connect=" PROV_CONNECT_CAP " advertise=%d\n",ret);return ret;
}
static void prov_status(void)
{
 nxmutex_lock(&g_prov_lock);
 printf("PROV registered=%u connected=%u subscribed=%u busy=%u session=%u commands=%u notify_queued=%u errors=%u connects=%u disconnects=%u reads=%u input_bytes=%zu\n",
  g_prov_registered,g_prov_conn!=NULL,g_prov_subscribed,g_prov_busy,g_prov_epoch,g_prov_commands,
  __atomic_load_n(&g_prov_fragments,__ATOMIC_RELAXED),g_prov_errors,g_prov_connects,g_prov_disconnects,g_prov_reads,g_prov_rx.used);
 printf("PROV last command=%u id=%u session=%u ret=%d notify_ret=%d\n",
  g_prov_last_command,g_prov_last_id,g_prov_last_session,g_prov_last_result,g_prov_last_notify);
 nxmutex_unlock(&g_prov_lock);
}
