/* Included after fw_wifi_target_scan. One serialized operation; existing RX
 * worker owns the FIFO. Association can receive EAPOL, but no keys are sent.
 */
static enum skw_mgmt_kind g_wifi_wait_kind=SKW_MGMT_AUTH;
static uint16_t g_wifi_assoc_aid;
static void fw_wifi_mgmt_event(uint8_t instance,const uint8_t *p,size_t n)
{
  struct skw_mgmt_result result;
  int ret;
  if (nxmutex_lock(&g_wifi_state)) return;
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
  if(__atomic_load_n(&g_wifi_network_active,__ATOMIC_ACQUIRE) && instance==g_wifi_join_instance)
    {
      ret=skw_mgmt_decode(p,n,g_wifi_mac,g_wifi_join_bss.scan.bssid,
        g_wifi_join_bss.scan.channel,g_wifi_join_bss.scan.band,&result);
      if(!ret && (result.kind==SKW_MGMT_DEAUTH || result.kind==SKW_MGMT_DISASSOC))
        {__atomic_store_n(&g_wifi_link_lost,true,__ATOMIC_RELEASE);printf("WIFI link lost reason=%u\n",result.status);}
    }
#endif
  if (!g_wifi_auth_waiting || instance!=g_wifi_join_instance)
    { nxmutex_unlock(&g_wifi_state); return; }
  ret=skw_mgmt_decode(p,n,g_wifi_mac,g_wifi_join_bss.scan.bssid,
    g_wifi_join_bss.scan.channel,g_wifi_join_bss.scan.band,&result);
  if (ret)
    printf("RADIO Wi-Fi ignored malformed management ret=%d bytes=%zu\n",ret,n);
  else if (result.kind==g_wifi_wait_kind)
    {
      g_wifi_auth_status=result.status;g_wifi_assoc_aid=result.aid;
      g_wifi_auth_received=true;g_wifi_auth_waiting=false;
      printf("RADIO Wi-Fi management response status=%u kind=%u\n",
        result.status,result.kind);
      nxsem_post(&g_wifi_authsem);
    }
  nxmutex_unlock(&g_wifi_state);
}

#include "skw_supplicant.h"
int skw_controller_random(uint8_t *,size_t);
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
static int fw_wifi_ip_session(struct skw_supplicant *);
#endif
static int fw_wifi_join_run(bool associate,const char *password)
{
  static uint8_t params[25+SKW_BSS_IE_MAX];
  uint8_t open[10]={1,0},auth[14],disconnect[8];
  struct skw_join_reply reply;
  size_t bytes;
  bool opened=false,joined=false,auth_ok=false,assoc_ok=false;
  bool wpa_ok=false;
  struct skw_supplicant *supp=NULL;
  int ret,unjoin=0,close_ret=0,key_cleanup=0;
  /* A previous failed CLOSE cannot be treated as clean firmware state. */
  if (g_wifi_wpa.active || g_wifi_wpa.faulted) return -EIO;
  ret=fw_wifi_target_scan();
  if (ret) return ret;
  if ((ret=nxmutex_lock(&g_wifi_state))) return ret;
  memcpy(&g_wifi_join_bss,&g_wifi_target,sizeof(g_wifi_join_bss));
  nxmutex_unlock(&g_wifi_state);
  const struct skw_bss *b=&g_wifi_join_bss;
  if (!b->rsn || b->unknown_suite || b->group_cipher!=4 ||
      !(b->pairwise&(1u<<4)) || !(b->akm&(1u<<2))) return -ENOTSUP;
  ret=skw_join_encode(b->scan.channel,b->scan.band,b->beacon_interval,
    b->capability,b->scan.bssid,b->ies,b->ie_len,params,sizeof(params),&bytes);
  if (ret) return ret;
  memcpy(open+4,g_wifi_mac,6);
  ret=fw_wifi_command(3,open,sizeof(open));
  if (ret) return ret;
  opened=true;
  ret=fw_wifi_command(9,params,bytes);
  if (ret) goto done;
  joined=true;
  ret=skw_join_reply_decode(g_wifi_response,g_wifi_response_len,&reply);
  if (ret) goto done;
  printf("RADIO Wi-Fi JOIN peer=%u lmac=%u instance=%u multicast=%u\n",
    reply.peer,reply.lmac,reply.instance,reply.multicast);
  /* This candidate registers only station instance 0. */
  if (reply.instance!=0) { ret=-EPROTO; goto done; }
  if ((ret=nxmutex_lock(&g_wifi_state))) goto done;
  while (nxsem_trywait(&g_wifi_authsem)==0) {}
  g_wifi_join_instance=reply.instance;
  g_wifi_wait_kind=SKW_MGMT_AUTH;g_wifi_auth_received=false;g_wifi_auth_status=0xffff;g_wifi_auth_waiting=true;
  nxmutex_unlock(&g_wifi_state);
  ret=skw_auth_open_encode(auth,sizeof(auth));
  if (!ret) ret=fw_wifi_command(10,auth,sizeof(auth));
  if (!ret) ret=nxsem_tickwait_uninterruptible(&g_wifi_authsem,MSEC2TICK(3000));
  if (nxmutex_lock(&g_wifi_state)==0)
    {
      g_wifi_auth_waiting=false;
      if (!ret && (!g_wifi_auth_received || g_wifi_auth_status)) ret=-EACCES;
      auth_ok=!ret && g_wifi_auth_received && !g_wifi_auth_status;
      nxmutex_unlock(&g_wifi_state);
    }
  else if (!ret) ret=-EIO;
  if (!ret && associate)
    {
      /* Explicit narrow WPA2-PSK/CCMP association proposal, no PMF support.
       * Full supplicant will later generate its own negotiated RSN IE.
       * This probe copies EAPOL transport frames, without authenticating them.
       */
      static const uint8_t rsn[]={0x30,20,1,0,0,15,172,4,1,0,0,15,172,4,
                                  1,0,0,15,172,2,0,0};
      bool compatible=false;
      for (size_t pos=0;pos+2<=b->ie_len;pos+=2+b->ies[pos+1])
        if (b->ies[pos]==48 && b->ies[pos+1]==20 &&
            pos+22<=b->ie_len && !(b->ies[pos+20]&0x40)) compatible=true;
      if (!compatible) { ret=-ENOTSUP; goto done; }
      ret=skw_assoc_encode(b->scan.bssid,rsn,sizeof(rsn),params,sizeof(params),&bytes);
      if (ret) goto done;
      ret=skw_wpa_begin(&g_wifi_wpa,reply.instance,reply.lmac,reply.peer,
        g_wifi_mac,b->scan.bssid,fw_wifi_wpa_command,NULL);
      if (ret) goto done;
      if(password)
        {
          const uint8_t *ap_rsn=NULL;size_t ap_rsn_len=0;
          uint8_t selected[128];size_t selected_len=sizeof(selected);
          for(size_t pos=0;pos+2<=b->ie_len;pos+=2+b->ies[pos+1])
            if(b->ies[pos]==48){ap_rsn=b->ies+pos;ap_rsn_len=2+b->ies[pos+1];break;}
          supp=skw_supplicant_start(&g_wifi_wpa,b->scan.ssid,b->scan.ssid_len,
            password,ap_rsn,ap_rsn_len,skw_controller_random);
          if(!supp){ret=-EACCES;goto done;}
          ret=skw_supplicant_assoc_ie(supp,selected,&selected_len);
          if(!ret)ret=skw_assoc_encode(b->scan.bssid,selected,selected_len,params,sizeof(params),&bytes);
          if(ret)goto done;
        }
      if ((ret=nxmutex_lock(&g_wifi_state))) goto done;
      /* Arm before ASSOC: an AP may send its first key frame immediately. */
      ret=skw_eapol_begin(&g_wifi_eapol,g_wifi_pn_reuse,reply.instance,
        reply.peer,g_wifi_mac,b->scan.bssid);
      if (ret) { nxmutex_unlock(&g_wifi_state); goto done; }
      while (nxsem_trywait(&g_wifi_authsem)==0) {}
      g_wifi_wait_kind=SKW_MGMT_ASSOC;g_wifi_assoc_aid=0;
      g_wifi_auth_received=false;g_wifi_auth_status=0xffff;g_wifi_auth_waiting=true;
      nxmutex_unlock(&g_wifi_state);
      ret=fw_wifi_command(11,params,bytes);
      if (!ret) ret=nxsem_tickwait_uninterruptible(&g_wifi_authsem,MSEC2TICK(3000));
      if (nxmutex_lock(&g_wifi_state)==0)
        {
          g_wifi_auth_waiting=false;
          if (!ret && (!g_wifi_auth_received || g_wifi_auth_status ||
              !g_wifi_assoc_aid || g_wifi_assoc_aid>2007)) ret=-EACCES;
          assoc_ok=!ret;
          printf("RADIO Wi-Fi association response_ok=%u status=%u AID=%u ret=%d\n",
            assoc_ok,g_wifi_auth_status,g_wifi_assoc_aid,ret);
          nxmutex_unlock(&g_wifi_state);
        }
      else if (!ret) ret=-EIO;
      if (!ret && supp)
        {
          struct skw_eapol_packet packet;
          skw_supplicant_associated(supp);
          uint64_t deadline=now_us()+20000000;
          while(now_us()<deadline && !ret && !skw_supplicant_completed(supp))
            {
              ret=nxmutex_lock(&g_wifi_state);if(ret)break;
              int received=skw_eapol_pop(&g_wifi_eapol,&packet);
              nxmutex_unlock(&g_wifi_state);
              if(!received)
                {
                  /* Malformed/unauthenticated EAPOL is dropped by hostap;
                   * only latched driver/state failure terminates the join. */
                  skw_supplicant_receive(supp,packet.data,packet.length);
                  skw_wpa_wipe(&packet,sizeof(packet));
                }
              else if(received!=-EAGAIN){ret=received;break;}
              ret=skw_supplicant_poll(supp);usleep(5000);
            }
          wpa_ok=skw_supplicant_completed(supp);
          if(!ret && !wpa_ok)ret=-ETIMEDOUT;
          printf("WIFI authentication completed=%u ret=%d installed=%02x IP_acquired=0\n",wpa_ok,ret,g_wifi_wpa.installed);
#ifdef CONFIG_EXAMPLES_K7RADIO_IP
          /* Ownership of supplicant/keys/interface transfers to IP worker,
           * which performs all cleanup on failure, disconnect or shutdown. */
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
          rb_authenticated(wpa_ok&&!ret);
#endif
          if(!ret && wpa_ok)return fw_wifi_ip_session(supp);
#endif
        }
      else if (!ret) usleep(1500000); /* Association-only diagnostic. */
    }
done:
  if(supp)skw_supplicant_stop(supp);
  if (nxmutex_lock(&g_wifi_state)==0)
    {
      printf("RADIO Wi-Fi EAPOL transport accepted=%u rejected=%u overflow=%u queued=%u\n",
        g_wifi_eapol.accepted,g_wifi_eapol.rejected,
        g_wifi_eapol.overflow,g_wifi_eapol.count);
      skw_eapol_clear(&g_wifi_eapol);
      g_wifi_auth_waiting=false;
      nxmutex_unlock(&g_wifi_state);
    }
  else if (!ret) ret=-EIO;
  if (g_wifi_wpa.active) key_cleanup=skw_wpa_clear_keys(&g_wifi_wpa);
  if (joined)
    {
      unjoin=skw_disconnect_encode(3,disconnect,sizeof(disconnect));
      if (!unjoin) unjoin=fw_wifi_command(17,disconnect,sizeof(disconnect));
    }
  if (opened) close_ret=fw_wifi_command(4,NULL,0);
  if (opened && !close_ret) skw_wpa_closed(&g_wifi_wpa);
#ifdef CONFIG_EXAMPLES_K7RADIO_SHARED
  rb_keys(key_cleanup,unjoin);
#endif
  if (!ret && key_cleanup) ret=key_cleanup;
  printf("RADIO Wi-Fi JOIN/AUTH result auth_response_ok=%u assoc_response_ok=%u ret=%d unjoin=%d close=%d; WPA_completed=%u IP_acquired=0\n",
    auth_ok,assoc_ok,ret,unjoin,close_ret,wpa_ok);
  return ret ? ret : (unjoin ? unjoin : close_ret);
}
static int fw_wifi_join_probe(bool associate)
{return fw_wifi_join_run(associate,NULL);}
