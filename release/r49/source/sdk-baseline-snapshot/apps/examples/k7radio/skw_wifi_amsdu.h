/* SPDX-License-Identifier: GPL-2.0-only */
/* Bound firmware-deaggregated MSDUs into a complete authenticated MPDU before
 * delivery. Never relax PN equality except within one pending sequence/PN. */
#ifndef SKW_WIFI_AMSDU_H
#define SKW_WIFI_AMSDU_H
#include "skw_wifi_data.h"
#define SKW_AMSDU_MAX 8
struct skw_amsdu_pending {
  uint64_t pn,started_ms;uint16_t sequence;uint8_t bitmap,first_index,last_index;bool active,last_seen;
  struct {size_t length;bool supported;uint8_t data[SKW_NET_FRAME_MAX];} frames[SKW_AMSDU_MAX];
};
struct skw_amsdu_rx {struct skw_amsdu_pending pending[2][8];unsigned complete,subframes;};
static inline int skw_amsdu_receive(struct skw_amsdu_rx *a,struct skw_data_replay *r,
  const uint8_t *slot,size_t size,uint8_t instance,uint8_t peer,const uint8_t own[6],
  uint64_t now_ms,int (*deliver)(const uint8_t *,size_t))
{
  struct skw_data_view v;
  if(!a || !deliver)return -EINVAL;
  int ret=skw_data_inspect(slot,size,instance,peer,own,r,&v);if(ret)return ret;
  uint64_t *floor=v.multicast?&r->multicast[v.tid]:&r->unicast[v.tid];
  struct skw_amsdu_pending *p=&a->pending[v.multicast][v.tid];
  if(v.pn<=*floor)return -EACCES;
  if(!v.amsdu)
    {
      if(p->active && v.pn<p->pn)return -EACCES;
      memset(p,0,sizeof(*p));*floor=v.pn;
      return v.supported?deliver(v.ethernet,v.length):-EPROTONOSUPPORT;
    }
  if(p->active && now_ms-p->started_ms>250)memset(p,0,sizeof(*p));
  if(!p->active || v.pn>p->pn)
    {
      /* First MSDU must open a new pending group; late orphaned pieces drop. */
      if(!v.first)return -EPROTO;
      memset(p,0,sizeof(*p));p->active=true;p->pn=v.pn;
      p->sequence=v.sequence;p->started_ms=now_ms;p->first_index=v.index;
    }
  if(p->pn!=v.pn || p->sequence!=v.sequence)return -EACCES;
  /* Actual SWT6621S samples start at index 1. The explicit first marker opens
   * the group; store relative indices, still requiring every contained MSDU.
   * A second first marker cannot truncate an already pending aggregate. */
  if(v.index<p->first_index || v.index-p->first_index>=SKW_AMSDU_MAX ||
     v.first!=(v.index==p->first_index))return -EPROTO;
  v.index-=p->first_index;
  if(p->bitmap&(1u<<v.index))return -EACCES;
  if((p->last_seen && (v.index>p->last_index || (v.last && v.index!=p->last_index))) ||
     (v.last && (p->bitmap & ~((1u<<(v.index+1))-1))))
    {memset(p,0,sizeof(*p));return -EPROTO;}
  p->frames[v.index].length=v.length;p->frames[v.index].supported=v.supported;
  memcpy(p->frames[v.index].data,v.ethernet,v.length);p->bitmap|=1u<<v.index;
  if(v.last){p->last_seen=true;p->last_index=v.index;}
  if(!p->last_seen || p->bitmap!=((1u<<(p->last_index+1))-1))return 0;
  /* Commit once per complete MPDU, before exposing any contained packet. */
  *floor=p->pn;int first_error=0;a->complete++;a->subframes+=p->last_index+1;
  for(unsigned i=0;i<=p->last_index;i++)if(p->frames[i].supported)
    {ret=deliver(p->frames[i].data,p->frames[i].length);if(ret && !first_error)first_error=ret;}
  memset(p,0,sizeof(*p));return first_error;
}
#endif
