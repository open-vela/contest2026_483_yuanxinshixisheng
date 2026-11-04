/* SPDX-License-Identifier: GPL-2.0-only */
/* Byte layouts from official SWT6621S skw_cfg80211.h and skw_msg.h.
 * These codecs do not imply association, WPA authentication, or IP success.
 */
#ifndef SKW_WIFI_JOIN_H
#define SKW_WIFI_JOIN_H
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline uint16_t skw_join_u16(const uint8_t *p)
{ return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static inline void skw_join_put16(uint8_t *p, uint16_t v)
{ p[0]=v; p[1]=v>>8; }
static inline void skw_join_put32(uint8_t *p, uint32_t v)
{ for (unsigned int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline int skw_join_ie_valid(const uint8_t *p, size_t n)
{
  if (n && !p) return -EINVAL;
  for (size_t pos=0;pos<n;)
    {
      if (n-pos<2 || p[pos+1]>n-pos-2) return -EPROTO;
      pos+=2+p[pos+1];
    }
  return 0;
}

/* Initial station bring-up is explicitly 20 MHz, center = primary channel.
 * No host-side packed structs, bitfields, or unaligned integer accesses.
 */
static inline int skw_join_encode(uint8_t channel, uint8_t band,
                                  uint16_t beacon, uint16_t capability,
                                  const uint8_t bssid[6],
                                  const uint8_t *ies, size_t ie_len,
                                  uint8_t *out, size_t capacity, size_t *written)
{
  int ret;
  if (!written) return -EINVAL;
  *written=0;
  if (!out || !bssid || !channel || band>1 || !beacon ||
      (bssid[0]&1) || !memcmp(bssid,"\0\0\0\0\0\0",6)) return -EINVAL;
  if (ie_len>1024) return -EMSGSIZE;
  if ((ret=skw_join_ie_valid(ies,ie_len))) return ret;
  if (capacity<25+ie_len) return -ENOSPC;
  memset(out,0,25+ie_len);
  out[0]=channel;out[1]=channel;out[3]=0;out[4]=band;
  skw_join_put16(out+5,beacon);skw_join_put16(out+7,capability);
  memcpy(out+11,bssid,6);
  if (ie_len)
    {
      skw_join_put16(out+19,25);skw_join_put32(out+21,(uint32_t)ie_len);
      memcpy(out+25,ies,ie_len);
    }
  *written=25+ie_len;return 0;
}

static inline int skw_auth_open_encode(uint8_t *out, size_t capacity)
{
  if (!out) return -EINVAL;
  if (capacity<14) return -ENOSPC;
  /* 802.11 Open System authentication precedes WPA2; no PSK in this command. */
  memset(out,0,14);return 0;
}

/* Pass the supplicant's selected association IE, not the AP's complete beacon.
 * Zero HT/VHT masks match cfg80211's default station association request.
 */
static inline int skw_assoc_encode(const uint8_t bssid[6],
                                   const uint8_t *ies, size_t ie_len,
                                   uint8_t *out, size_t capacity, size_t *written)
{
  int ret;
  if (!written) return -EINVAL;
  *written=0;
  if (!bssid || !out || (bssid[0]&1) ||
      !memcmp(bssid,"\0\0\0\0\0\0",6)) return -EINVAL;
  if (ie_len>1024) return -EMSGSIZE;
  if ((ret=skw_join_ie_valid(ies,ie_len))) return ret;
  if (capacity<54+ie_len) return -ENOSPC;
  memset(out,0,54+ie_len);memcpy(out+38,bssid,6);
  skw_join_put16(out+50,54);skw_join_put16(out+52,(uint16_t)ie_len);
  if (ie_len) memcpy(out+54,ies,ie_len);
  *written=54+ie_len;return 0;
}

struct skw_join_reply { uint8_t peer,lmac,instance,multicast; };
static inline int skw_join_reply_decode(const uint8_t *p, size_t n,
                                        struct skw_join_reply *out)
{
  if (!p || !out) return -EINVAL;
  if (n!=4) return -EMSGSIZE;
  out->peer=p[0];out->lmac=p[1];out->instance=p[2];out->multicast=p[3];
  return 0;
}

enum skw_mgmt_kind { SKW_MGMT_NONE,SKW_MGMT_AUTH,SKW_MGMT_ASSOC,
                     SKW_MGMT_DEAUTH,SKW_MGMT_DISASSOC };
struct skw_mgmt_result {
  enum skw_mgmt_kind kind;
  uint16_t status,aid,algorithm,transaction;
};

/* Decode SKW_EVNET_RX_MGMT (event 4). SDK event numbers 3/7 do NOT replace
 * the management-frame response used by the actual vendor STA path.
 * Caller must also match event instance, expected phase and command lifetime.
 */
static inline int skw_mgmt_decode(const uint8_t *p, size_t n,
                                  const uint8_t own[6],const uint8_t ap[6],
                                  uint8_t channel,uint8_t band,
                                  struct skw_mgmt_result *out)
{
  size_t len;const uint8_t *m;uint16_t fc;
  if (!p || !own || !ap || !out) return -EINVAL;
  memset(out,0,sizeof(*out));
  if (n<8) return -EMSGSIZE;
  len=skw_join_u16(p+4);
  if (len<24 || len>n-8) return -EMSGSIZE;
  m=p+8;fc=skw_join_u16(m);
  /* Reject fragmented/encrypted or non-management frames at this layer. */
  if ((fc&0x470f) || (skw_join_u16(m+22)&15)) return -EPROTO;
  if (p[0]!=channel || p[1]!=band || memcmp(m+4,own,6) ||
      memcmp(m+10,ap,6) || memcmp(m+16,ap,6)) return 0;
  switch (fc&0xfc)
    {
      case 0xb0:
        if (len<30) return -EMSGSIZE;
        out->algorithm=skw_join_u16(m+24);
        out->transaction=skw_join_u16(m+26);
        if (out->algorithm || out->transaction!=2) return -EPROTO;
        out->status=skw_join_u16(m+28);out->kind=SKW_MGMT_AUTH;return 0;
      case 0x10:
        if (len<30) return -EMSGSIZE;
        out->status=skw_join_u16(m+26);
        out->aid=skw_join_u16(m+28)&0x3fff;
        out->kind=SKW_MGMT_ASSOC;return 0;
      case 0xc0: case 0xa0:
        if (len<26) return -EMSGSIZE;
        out->status=skw_join_u16(m+24);
        out->kind=(fc&0xfc)==0xc0 ? SKW_MGMT_DEAUTH:SKW_MGMT_DISASSOC;
        return 0;
      default:return 0;
    }
}

static inline int skw_disconnect_encode(uint16_t reason,
                                        uint8_t *out,size_t capacity)
{
  if (!out) return -EINVAL;
  if (capacity<8) return -ENOSPC;
  memset(out,0,8);out[0]=2;skw_join_put16(out+2,reason);
  /* Send deauthentication for this station only; no AP/global radio reset. */
  return 0;
}
#endif
