/* SPDX-License-Identifier: GPL-2.0-only */
/* Wire layout derived from Seekwave skw_rx.h/skw_rx.c/skw_tx.h/skw_core.c.
 * Narrow WPA2 transport preparation, NOT a supplicant or authenticated netdev.
 * Input includes the SDIO2 header: PN_REUSE overlays that header with RX desc.
 */
#ifndef SKW_WIFI_EAPOL_H
#define SKW_WIFI_EAPOL_H
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define SKW_EAPOL_MAX 1024
struct skw_eapol_view {
  const uint8_t *data;
  size_t length, ethernet_offset, ethernet_length;
  uint16_t sequence;
  uint8_t instance, peer, tid;
};
static inline uint16_t skw_eap_u16(const uint8_t *p)
{ return (uint16_t)p[0] | (uint16_t)p[1]<<8; }
static inline void skw_eap_put16(uint8_t *p,uint16_t n)
{ p[0]=n;p[1]=n>>8; }
/* Only single, nonfragmented Ethernet EAPOL frames are supported here.
 * Caller must validate firmware GET_INFO.priv_pn_reuse, then copy data out of
 * the shared RX slot before returning to the single SDIO receive worker.
 * MIC/replay/key validation belongs to the upstream WPA state machine.
 */
static inline int skw_eapol_decode(const uint8_t *slot,size_t available,
  bool pn_reuse,uint8_t instance,uint8_t peer,const uint8_t own[6],
  const uint8_t bssid[6],struct skw_eapol_view *out)
{
  size_t wire,base,offset,bytes,body;
  const uint8_t *desc,*eth,*eap;
  uint16_t meta;
  if(!out) return -EINVAL;
  memset(out,0,sizeof(*out));
  if(!slot || !own || !bssid || instance>3 || peer>31) return -EINVAL;
  if(available<4) return -EMSGSIZE;
  if(slot[3]!=7 || (slot[2]&0x80)) return -EPROTO;
  wire=skw_eap_u16(slot)+4u;
  if(wire>available || wire>1536 || wire<24) return -EMSGSIZE;
  base=pn_reuse?0:4;desc=slot+base;
  if(wire-base<20) return -EMSGSIZE;
  meta=skw_eap_u16(desc+10);
  if(!(meta&4) || !(meta&0x200) || (meta&3)!=instance || ((meta>>4)&31)!=peer) return -EACCES;
  if(!(desc[4]&4) || (desc[4]&0x8a) || (meta&8) || (desc[9]&0xf0) || (desc[19]&0x80)) return -ENOTSUP;
  if(desc[18]<72) return -EPROTO;
  offset=base+(size_t)desc[18]-52u;
  bytes=skw_eap_u16(desc+(pn_reuse?16:2))+6u;
  if(offset>wire || bytes<18 || bytes>wire-offset) return -EMSGSIZE;
  eth=slot+offset;
  if(memcmp(eth,own,6) || memcmp(eth+6,bssid,6)) return -EACCES;
  if(eth[12]!=0x88 || eth[13]!=0x8e) return -EPROTONOSUPPORT;
  eap=eth+14;body=((size_t)eap[2]<<8)|eap[3];
  if(eap[0]<1 || eap[0]>3 || eap[1]!=3) return -ENOTSUP;
  if(body<95 || body+4>bytes-14 || body+4>SKW_EAPOL_MAX) return -EMSGSIZE;
  out->data=eap;out->length=body+4;out->ethernet_offset=offset;out->ethernet_length=bytes;
  out->instance=instance;out->peer=peer;out->tid=meta>>12;
  out->sequence=skw_eap_u16(desc+8)&0xfff;
  return 0;
}
/* Parameters for SKW_CMD_TX_DATA_FRAME (15), not a data-channel SDIO frame.
 * The official driver routes ETH_P_PAE through that command before WPA is
 * completed. Firmware ACK remains mandatory; this routine only encodes bytes.
 * Input and output buffers must not overlap.
 */
static inline int skw_eapol_encode(uint8_t instance,uint8_t lmac,uint8_t peer,
  const uint8_t own[6],const uint8_t bssid[6],const uint8_t *eap,size_t length,
  uint8_t *out,size_t capacity,size_t *written)
{
  size_t total=22u+length;
  if(!written) return -EINVAL;
  *written=0;
  if(!own || !bssid || !eap || !out || instance>3 || lmac>3 || peer>31) return -EINVAL;
  if(length<99 || length>SKW_EAPOL_MAX || eap[0]<1 || eap[0]>3 || eap[1]!=3) return -EINVAL;
  if((((size_t)eap[2]<<8)|eap[3])+4!=length) return -EMSGSIZE;
  if(capacity<total) return -ENOSPC;
  memset(out,0,total);
  skw_eap_put16(out,((uint16_t)instance<<2)|(6u<<4)|((uint16_t)peer<<8));
  skw_eap_put16(out+2,(uint16_t)(length+14)|((uint16_t)lmac<<12));
  out[4]=0x88;out[5]=0x8e;
  memcpy(out+8,bssid,6);memcpy(out+14,own,6);out[20]=0x88;out[21]=0x8e;
  memcpy(out+22,eap,length);*written=total;
  return 0;
}
#endif
