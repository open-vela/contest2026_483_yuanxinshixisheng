/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SKW_EAPOL_QUEUE_H
#define SKW_EAPOL_QUEUE_H
#include "skw_wifi_eapol.h"
#define SKW_EAPOL_DEPTH 4
/* Caller serializes every access. No allocation, waiting, TX, or callbacks
 * here: SDIO RX must remain available for Bluetooth and command ACKs. */
struct skw_eapol_packet { size_t length; uint8_t data[SKW_EAPOL_MAX]; };
struct skw_eapol_queue {
  struct skw_eapol_packet packets[SKW_EAPOL_DEPTH];
  uint8_t own[6], bssid[6], instance, peer;
  unsigned head, count, accepted, rejected, overflow;
  bool active, pn_reuse;
};
static inline void skw_eapol_clear(struct skw_eapol_queue *q)
{
  volatile uint8_t *p=(volatile uint8_t *)q;
  for (size_t n=0;n<sizeof(*q);n++) p[n]=0;
}
static inline int skw_eapol_begin(struct skw_eapol_queue *q, bool pn_reuse,
  uint8_t instance,uint8_t peer,const uint8_t own[6],const uint8_t bssid[6])
{
  skw_eapol_clear(q);
  if (!own || !bssid || instance>3 || peer>31) return -EINVAL;
  memcpy(q->own,own,6);memcpy(q->bssid,bssid,6);
  q->instance=instance;q->peer=peer;q->pn_reuse=pn_reuse;q->active=true;
  return 0;
}
static inline int skw_eapol_push(struct skw_eapol_queue *q,
  const uint8_t *slot,size_t length)
{
  struct skw_eapol_view v;
  if (!q->active) return -ENOTCONN;
  int ret=skw_eapol_decode(slot,length,q->pn_reuse,q->instance,q->peer,
    q->own,q->bssid,&v);
  if (ret) { q->rejected++;return ret; }
  if (q->count==SKW_EAPOL_DEPTH) { q->overflow++;return -ENOSPC; }
  struct skw_eapol_packet *p=&q->packets[(q->head+q->count)%SKW_EAPOL_DEPTH];
  memcpy(p->data,v.data,v.length);p->length=v.length;
  q->count++;q->accepted++;return 0;
}
static inline int skw_eapol_pop(struct skw_eapol_queue *q,
  struct skw_eapol_packet *out)
{
  if (!out) return -EINVAL;
  memset(out,0,sizeof(*out));
  if (!q->active) return -ENOTCONN;
  if (!q->count) return -EAGAIN;
  *out=q->packets[q->head];
  volatile uint8_t *p=(volatile uint8_t *)&q->packets[q->head];
  for (size_t n=0;n<sizeof(q->packets[0]);n++) p[n]=0;
  q->head=(q->head+1)%SKW_EAPOL_DEPTH;q->count--;return 0;
}
#endif
