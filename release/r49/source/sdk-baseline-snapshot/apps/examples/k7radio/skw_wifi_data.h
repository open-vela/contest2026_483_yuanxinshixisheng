/* SPDX-License-Identifier: GPL-2.0-only */
/* Seekwave Ethernet command/SDIO RX layouts, with explicit bounds and PN
 * checks. Firmware supplies individual Ethernet MSDUs, including MPDUs marked
 * as belonging to an AMPDU. A-MSDU grouping lives in skw_wifi_amsdu.h. */
#ifndef SKW_WIFI_DATA_H
#define SKW_WIFI_DATA_H
#include "skw_wifi_eapol.h"
#define SKW_NET_FRAME_MAX 1014
struct skw_data_view {
  const uint8_t *ethernet;size_t length;uint8_t tid;bool multicast;uint64_t pn;
  bool amsdu,first,last,supported;uint8_t index;uint16_t sequence;
};
struct skw_data_replay {
  uint64_t unicast[8],multicast[8];bool pn_reuse,active;
};
static inline uint64_t skw_data_pn(const uint8_t *p,unsigned bytes)
{uint64_t n=0;for(unsigned i=0;i<bytes;i++)n|=(uint64_t)p[i]<<(8*i);return n;}
static inline int skw_data_begin(struct skw_data_replay *r,bool reuse,
  const uint8_t unicast_rsc[6],const uint8_t group_rsc[6])
{
  memset(r,0,sizeof(*r));
  /* Reused descriptor exposes only 32 PN bits; never silently truncate an
   * already-large initial RSC or accept wrap-around as a new packet. */
  if(reuse && (unicast_rsc[4] || unicast_rsc[5] || group_rsc[4] || group_rsc[5]))return -ENOTSUP;
  for(unsigned i=0;i<8;i++)
    {r->unicast[i]=skw_data_pn(unicast_rsc,6);r->multicast[i]=skw_data_pn(group_rsc,6);}
  r->pn_reuse=reuse;r->active=true;return 0;
}
static inline int skw_data_inspect(const uint8_t *slot,size_t available,
  uint8_t instance,uint8_t peer,const uint8_t own[6],struct skw_data_replay *r,
  struct skw_data_view *out)
{
  if(!slot || !own || !r || !out)return -EINVAL;
  memset(out,0,sizeof(*out));if(!r->active)return -ENOTCONN;
  if(available<24 || slot[3]!=7 || (slot[2]&0x80))return -EPROTO;
  size_t wire=skw_eap_u16(slot)+4u,base=r->pn_reuse?0:4;
  if(wire>available || wire>1536 || wire<base+20)return -EMSGSIZE;
  const uint8_t *d=slot+base;uint16_t meta=skw_eap_u16(d+10);
  if(!(meta&4) || !(meta&0x200) || (meta&3)!=instance || ((meta>>4)&31)!=peer)return -EACCES;
  /* AMPDU membership does not make this independently described MSDU partial.
   * Retain strict per-TID PN ordering: out-of-order MPDUs may be dropped, never
   * accepted by resetting replay state. Reject sniffed or fragmented MSDUs. */
  if(!(d[4]&4) || (d[4]&0x80) || (meta&8) || (d[9]&0xf0) || (d[19]&0x80))return -ENOTSUP;
  if(!r->pn_reuse && ((skw_eap_u16(d)>>8)&15)!=8)return -EACCES;
  if(d[18]<72)return -EPROTO;
  size_t offset=base+d[18]-52u,length=skw_eap_u16(d+(r->pn_reuse?16:2))+6u;
  if(offset>wire || length<14 || length>SKW_NET_FRAME_MAX || length>wire-offset)return -EMSGSIZE;
  const uint8_t *eth=slot+offset;bool mc=(meta&0x400)!=0;
  bool amsdu=(d[4]&8)!=0;
  if((!(eth[0]&1) && memcmp(eth,own,6)) || (mc && !(eth[0]&1)) ||
     (!amsdu && (bool)(eth[0]&1)!=mc))return -EACCES;
  uint8_t tid=meta>>12;if(tid>=8)return -EPROTO;
  uint64_t pn=skw_data_pn(d+12,r->pn_reuse?4:6);
  if(!pn)return -EACCES;
  out->ethernet=eth;out->length=length;out->tid=tid;out->multicast=mc;out->pn=pn;
  out->amsdu=amsdu;out->first=(d[4]&0x20)!=0;out->last=(d[4]&0x40)!=0;
  out->index=d[19]&63;out->sequence=skw_eap_u16(d+8)&4095;
  out->supported=eth[12]==8 && (eth[13]==0 || eth[13]==6);
  return 0;
}
/* Non-aggregated compatibility entry: use the bounded assembler for AMSDU. */
static inline int skw_data_decode(const uint8_t *slot,size_t available,
  uint8_t instance,uint8_t peer,const uint8_t own[6],struct skw_data_replay *r,
  struct skw_data_view *out)
{
  int ret=skw_data_inspect(slot,available,instance,peer,own,r,out);
  if(ret)return ret;
  if(out->amsdu)return -ENOTSUP;
  if(!out->supported)return -EPROTONOSUPPORT;
  uint64_t *last=out->multicast?&r->multicast[out->tid]:&r->unicast[out->tid];
  if(out->pn<=*last)return -EACCES;
  *last=out->pn;return 0;
}
static inline int skw_data_encode(uint8_t peer,uint8_t lmac,const uint8_t *eth,
  size_t length,uint8_t *out,size_t capacity,size_t *written)
{
  if(!written)return -EINVAL;
  *written=0;
  if(!eth || !out || peer>31 || lmac>3 || length<14 || length>SKW_NET_FRAME_MAX)return -EINVAL;
  if(length+8>capacity)return -ENOSPC;
  memset(out,0,8);skw_eap_put16(out,(uint16_t)peer<<8);
  skw_eap_put16(out+2,(uint16_t)length|((uint16_t)lmac<<12));
  out[4]=eth[12];out[5]=eth[13];memcpy(out+8,eth,length);*written=length+8;
  return 0;
}
#endif
