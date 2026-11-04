/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded capture of an authorized scan target; no password storage. */
#ifndef SKW_WIFI_BSS_H
#define SKW_WIFI_BSS_H
#include "skw_wifi_diag.h"
#include <stdbool.h>
#define SKW_BSS_IE_MAX 1024u
struct skw_bss {
  struct skw_scan_record scan;
  uint16_t beacon_interval, capability, ie_len;
  uint32_t pairwise, akm;
  bool rsn, wpa, unknown_suite;
  uint8_t group_cipher, ies[SKW_BSS_IE_MAX];
};
static inline int skw_bss_rsn(const uint8_t *p, size_t n, struct skw_bss *b)
{
  size_t pos = 0, count;
  if (n < 8 || skw_diag_u16(p) != 1) return -EPROTO;
  b->rsn = true;
  if (memcmp(p+2, "\x00\x0f\xac", 3)) b->unknown_suite = true;
  b->group_cipher = p[5]; pos = 6;
  count = skw_diag_u16(p+pos); pos += 2;
  if (!count || count > (n-pos)/4) return -EPROTO;
  for (size_t i=0; i<count; i++,pos+=4)
    if (!memcmp(p+pos,"\x00\x0f\xac",3) && p[pos+3]<32)
      b->pairwise |= 1u << p[pos+3];
    else b->unknown_suite = true;
  if (n-pos < 2) return -EPROTO;
  count = skw_diag_u16(p+pos); pos += 2;
  if (!count || count > (n-pos)/4) return -EPROTO;
  for (size_t i=0; i<count; i++,pos+=4)
    if (!memcmp(p+pos,"\x00\x0f\xac",3) && p[pos+3]<32)
      b->akm |= 1u << p[pos+3];
    else b->unknown_suite = true;
  /* Remaining RSN optional fields are kept verbatim in the captured IE. */
  return 0;
}
static inline int skw_bss_decode(const uint8_t *p, size_t n, struct skw_bss *b)
{
  int ret;
  if (!b) return -EINVAL;
  memset(b,0,sizeof(*b));
  ret=skw_diag_scan(p,n,&b->scan);
  if (ret) return ret;
  size_t len=skw_diag_u16(p+4)-36;
  if (len>SKW_BSS_IE_MAX) return -EMSGSIZE;
  const uint8_t *m=p+8;
  b->beacon_interval=skw_diag_u16(m+32);
  b->capability=skw_diag_u16(m+34);
  b->ie_len=len; memcpy(b->ies,m+36,len);
  for (size_t pos=0;pos<len;)
    {
      size_t count=b->ies[pos+1];
      if (b->ies[pos]==48)
        { ret=skw_bss_rsn(b->ies+pos+2,count,b); if(ret) return ret; }
      if (b->ies[pos]==221 && count>=4 &&
          !memcmp(b->ies+pos+2,"\x00\x50\xf2\x01",4)) b->wpa=true;
      pos+=count+2;
    }
  return 0;
}
#endif
