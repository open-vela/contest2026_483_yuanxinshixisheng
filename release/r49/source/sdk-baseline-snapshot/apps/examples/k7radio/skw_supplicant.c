/* SPDX-License-Identifier: Apache-2.0 */
/* WPA2-PSK/CCMP adapter to unmodified hostap RSN state machine. */
#include "includes.h"
#include "common.h"
#include "rsn_supp/wpa.h"
#include "crypto/sha1.h"
#include "skw_supplicant.h"

struct skw_supplicant {
  struct wpa_sm *sm;
  struct skw_wpa_transport *transport;
  enum wpa_states state;
  int error;
};
extern int (*skw_supplicant_random)(uint8_t *,size_t);
extern int skw_supplicant_timer_error;
static struct skw_supplicant *owner;
static void state_set(void *ctx,enum wpa_states state)
{
  struct skw_supplicant *s=ctx;s->state=state;
  printf("WIFI WPA state=%d\n",state);
}
static enum wpa_states state_get(void *ctx)
{return ((struct skw_supplicant *)ctx)->state;}
static void deauth(void *ctx,uint16_t reason)
{
  struct skw_supplicant *s=ctx;s->error=-EACCES;s->state=WPA_DISCONNECTED;
  printf("WIFI WPA deauthenticate reason=%u\n",reason);
}
static void reconnect(void *ctx){deauth(ctx,0);}
static void *network(void *ctx){return ctx;}
static int bssid_get(void *ctx,uint8_t *bssid)
{memcpy(bssid,((struct skw_supplicant *)ctx)->transport->bssid,6);return 0;}
static int ether_send(void *ctx,const uint8_t *dest,uint16_t proto,
  const uint8_t *buf,size_t len)
{
  struct skw_supplicant *s=ctx;
  if(proto!=0x888e || memcmp(dest,s->transport->bssid,6))return -EINVAL;
  int ret=skw_wpa_send_eapol(s->transport,buf,len);
  if(ret)s->error=ret;
  printf("WIFI WPA EAPOL tx bytes=%zu ret=%d\n",len,ret);return ret;
}
static int key_set(void *ctx,int link,enum wpa_alg alg,const uint8_t *addr,
  int index,int tx,const uint8_t *seq,size_t seq_len,const uint8_t *key,
  size_t key_len,enum key_flag flags)
{
  struct skw_supplicant *s=ctx;(void)tx;
  bool pairwise=(flags&KEY_FLAG_PAIRWISE)!=0;
  /* hostap optionally probes an RX-only next PTK before M4. This firmware
   * has no separate RX/TX activation API; decline the optional probe so
   * hostap uses its normal, single installation after sending M4. */
  if(flags&KEY_FLAG_NEXT)return -ENOTSUP;
  if(link!=-1 || alg!=WPA_ALG_CCMP || index<0 || index>3 ||
     (pairwise && (!addr || memcmp(addr,s->transport->bssid,6))))
    {s->error=-ENOTSUP;return s->error;}
  int ret=skw_wpa_install_ccmp(s->transport,pairwise,index,key,key_len,seq,seq_len);
  if(ret)s->error=ret;
  printf("WIFI WPA key type=%s index=%d ret=%d\n",pairwise?"PTK":"GTK",index,ret);
  return ret;
}
static int beacon(void *ctx){(void)ctx;return 0;} /* AP RSN was supplied at start. */
static void timeout_cancel(void *ctx){(void)ctx;}
static uint8_t *eapol_alloc(void *ctx,uint8_t type,const void *data,uint16_t len,
  size_t *total,void **body)
{
  (void)ctx;uint8_t *p=calloc(1,4u+len);if(!p)return NULL;
  p[0]=2;p[1]=type;WPA_PUT_BE16(p+2,len);
  if(data)memcpy(p+4,data,len);
  *total=4u+len;*body=p+4;return p;
}
static int protect(void *ctx,const uint8_t *addr,int type,int key)
{(void)ctx;(void)addr;(void)type;(void)key;return 0;}
static int pmkid_add(void *ctx,void *net,const uint8_t *ap,const uint8_t *id,
  const uint8_t *cache,const uint8_t *pmk,size_t len,uint32_t life,uint8_t threshold,int akmp)
{(void)ctx;(void)net;(void)ap;(void)id;(void)cache;(void)pmk;(void)len;(void)life;(void)threshold;(void)akmp;return -ENOTSUP;}
static int pmkid_remove(void *ctx,void *net,const uint8_t *ap,const uint8_t *id,const uint8_t *cache)
{(void)ctx;(void)net;(void)ap;(void)id;(void)cache;return 0;}
struct skw_supplicant *skw_supplicant_start(struct skw_wpa_transport *t,
  const uint8_t *ssid,size_t ssid_len,const char *password,const uint8_t *rsn,
  size_t rsn_len,int (*rng)(uint8_t *,size_t))
{
  uint8_t pmk[32];struct wpa_ie_data ie;
  if(owner || !t || !t->active || !ssid || !ssid_len || ssid_len>32 ||
     !password || strlen(password)<8 || strlen(password)>63 || !rng ||
     wpa_parse_wpa_ie(rsn,rsn_len,&ie) || ie.proto!=WPA_PROTO_RSN ||
     ie.group_cipher!=WPA_CIPHER_CCMP || !(ie.pairwise_cipher&WPA_CIPHER_CCMP) ||
     !(ie.key_mgmt&WPA_KEY_MGMT_PSK) || (ie.capabilities&BIT(6)))return NULL;
  struct skw_supplicant *s=calloc(1,sizeof(*s));
  struct wpa_sm_ctx *c=calloc(1,sizeof(*c));
  if(!s || !c){free(s);free(c);return NULL;}
  s->transport=t;c->ctx=s;c->set_state=state_set;c->get_state=state_get;
  c->deauthenticate=deauth;c->reconnect=reconnect;c->set_key=key_set;
  c->get_network_ctx=network;c->get_bssid=bssid_get;c->ether_send=ether_send;
  c->get_beacon_ie=beacon;c->cancel_auth_timeout=timeout_cancel;
  c->alloc_eapol=eapol_alloc;c->mlme_setprotection=protect;
  c->add_pmkid=pmkid_add;c->remove_pmkid=pmkid_remove;
  s->sm=wpa_sm_init(c);if(!s->sm){free(c);free(s);return NULL;}
  owner=s;skw_supplicant_random=rng;skw_supplicant_timer_error=0;
  wpa_sm_set_own_addr(s->sm,t->own);
  wpa_sm_set_param(s->sm,WPA_PARAM_PROTO,WPA_PROTO_RSN);
  wpa_sm_set_param(s->sm,WPA_PARAM_PAIRWISE,WPA_CIPHER_CCMP);
  wpa_sm_set_param(s->sm,WPA_PARAM_GROUP,WPA_CIPHER_CCMP);
  wpa_sm_set_param(s->sm,WPA_PARAM_KEY_MGMT,WPA_KEY_MGMT_PSK);
  wpa_sm_set_param(s->sm,WPA_PARAM_RSN_ENABLED,1);
  wpa_sm_set_param(s->sm,WPA_PARAM_MFP,0);
  /* PTK0 rekey reconnects rather than reinstalling an active TX key. */
  wpa_sm_set_param(s->sm,WPA_PARAM_DENY_PTK0_REKEY,1);
  struct rsn_supp_config config={0};config.network_ctx=s;
  config.allowed_pairwise_cipher=WPA_CIPHER_CCMP;config.ssid=ssid;
  config.ssid_len=ssid_len;config.wpa_deny_ptk0_rekey=1;
  wpa_sm_set_config(s->sm,&config);
  if(wpa_sm_set_ap_rsn_ie(s->sm,rsn,rsn_len) ||
     pbkdf2_sha1(password,ssid,ssid_len,4096,pmk,sizeof(pmk)))
    {skw_wpa_wipe(pmk,sizeof(pmk));skw_supplicant_stop(s);return NULL;}
  wpa_sm_set_pmk(s->sm,pmk,sizeof(pmk),NULL,NULL);
  skw_wpa_wipe(pmk,sizeof(pmk));return s;
}
int skw_supplicant_assoc_ie(struct skw_supplicant *s,uint8_t *ie,size_t *len)
{return wpa_sm_set_assoc_wpa_ie_default(s->sm,ie,len);}
void skw_supplicant_associated(struct skw_supplicant *s)
{s->state=WPA_ASSOCIATED;wpa_sm_notify_assoc(s->sm,s->transport->bssid);}
int skw_supplicant_receive(struct skw_supplicant *s,const uint8_t *p,size_t n)
{
  if(s->error)return s->error;
  int ret=wpa_sm_rx_eapol(s->sm,s->transport->bssid,p,n,FRAME_ENCRYPTION_UNKNOWN);
  printf("WIFI WPA EAPOL rx bytes=%zu handled=%d\n",n,ret);
  return s->error ? s->error : (ret<0?-EPROTO:0);
}
int skw_supplicant_poll(struct skw_supplicant *s)
{skw_supplicant_timers();if(skw_supplicant_timer_error)s->error=skw_supplicant_timer_error;return s->error;}
bool skw_supplicant_completed(struct skw_supplicant *s)
{return s && !s->error && s->state==WPA_COMPLETED && (s->transport->installed&1) && (s->transport->installed&30);}
void skw_supplicant_stop(struct skw_supplicant *s)
{
  if(!s)return;
  wpa_sm_notify_disassoc(s->sm);wpa_sm_deinit(s->sm);
  if(owner==s){owner=NULL;skw_supplicant_random=NULL;}
  skw_wpa_wipe(s,sizeof(*s));free(s);
}
