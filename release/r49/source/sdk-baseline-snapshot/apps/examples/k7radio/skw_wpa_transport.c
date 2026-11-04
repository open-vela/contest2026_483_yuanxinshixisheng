/* SPDX-License-Identifier: GPL-2.0-only */
#include "skw_wpa_transport.h"
/* SDK skw_iface.h skw_key_params: MAC[6],type,cipher,PN[6],id,len,key[32].
 * skw_cmd_add_key initializes TX PN to 1; RX RSC stays on the host.
 * Linux v6.1 ieee80211.h defines WLAN_MAX_KEY_LEN as 32. */
#define KEY_BYTES 48
void skw_wpa_wipe(void *p,size_t n)
{ volatile uint8_t *v=p;while(n--) *v++=0; }
int skw_wpa_begin(struct skw_wpa_transport *t,uint8_t instance,uint8_t lmac,
  uint8_t peer,const uint8_t own[6],const uint8_t ap[6],skw_wpa_command cmd,void *arg)
{
  static const uint8_t zero[6]={0};
  if (!t || !own || !ap || !cmd) return -EINVAL;
  if (t->active || t->attempted || t->faulted) return -EBUSY;
  if (instance || lmac>3 || peer>31 || (own[0]&1) || (ap[0]&1) ||
      !memcmp(own,zero,6) || !memcmp(ap,zero,6)) return -EINVAL;
  skw_wpa_wipe(t,sizeof(*t));
  memcpy(t->own,own,6);memcpy(t->bssid,ap,6);
  t->lmac=lmac;t->peer=peer;t->command=cmd;t->arg=arg;t->active=true;
  return 0;
}
static int ready(struct skw_wpa_transport *t)
{
  if (!t || !t->active || !t->command) return -ENOTCONN;
  return t->faulted ? -EIO:0;
}
int skw_wpa_send_eapol(struct skw_wpa_transport *t,const uint8_t *eap,size_t n)
{
  size_t written=0;int ret=ready(t);
  if (ret) return ret;
  ret=skw_eapol_encode(0,t->lmac,t->peer,t->own,t->bssid,eap,n,
    t->scratch,sizeof(t->scratch),&written);
  if (!ret) {
    ret=t->command(t->arg,15,t->scratch,written);
    if (ret) t->faulted=true;
  }
  skw_wpa_wipe(t->scratch,sizeof(t->scratch));return ret;
}
static void key_header(struct skw_wpa_transport *t,bool pairwise,uint8_t id)
{
  memset(t->scratch,0,sizeof(t->scratch));
  memcpy(t->scratch,t->bssid,6);t->scratch[6]=pairwise?0:1;
  t->scratch[7]=8;t->scratch[14]=id;
}
int skw_wpa_install_ccmp(struct skw_wpa_transport *t,bool pairwise,uint8_t id,
  const uint8_t *key,size_t n,const uint8_t *seq,size_t seq_len)
{
  int ret=ready(t);unsigned slot;
  if (ret) return ret;
  if (!key || n!=16 || id>3 || (pairwise && id) ||
      (seq_len!=0 && seq_len!=6) || (seq_len && !seq)) return -EINVAL;
  slot=pairwise?0:1+id;
  if (t->attempted&(1u<<slot)) return -EALREADY;
  key_header(t,pairwise,id);t->scratch[8]=1;t->scratch[15]=16;
  memcpy(t->scratch+16,key,16);
  /* Timeout does not prove the firmware did not install the key. */
  t->attempted|=1u<<slot;
  ret=t->command(t->arg,12,t->scratch,KEY_BYTES);
  skw_wpa_wipe(t->scratch,sizeof(t->scratch));
  if (ret) t->faulted=true;
  else {
    t->installed|=1u<<slot;
    if (seq_len) memcpy(t->initial_rsc[slot],seq,6);
  }
  return ret;
}
int skw_wpa_clear_keys(struct skw_wpa_transport *t)
{
  int first=0;
  if (!t || !t->active || !t->command) return -ENOTCONN;
  for (unsigned slot=0;slot<5;slot++) if (t->attempted&(1u<<slot)) {
    key_header(t,slot==0,slot?slot-1:0);
    int ret=t->command(t->arg,13,t->scratch,KEY_BYTES);
    skw_wpa_wipe(t->scratch,sizeof(t->scratch));
    skw_wpa_wipe(t->initial_rsc[slot],6);t->installed&=~(1u<<slot);
    if (ret) { if (!first) first=ret;t->faulted=true; }
    else t->attempted&=~(1u<<slot);
  }
  return first;
}
void skw_wpa_closed(struct skw_wpa_transport *t)
{ if(t) skw_wpa_wipe(t,sizeof(*t)); }
