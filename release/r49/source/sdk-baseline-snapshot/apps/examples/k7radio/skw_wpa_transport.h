/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SKW_WPA_TRANSPORT_H
#define SKW_WPA_TRANSPORT_H
#include "skw_wifi_eapol.h"
/* Synchronous command returns zero only for a matching successful firmware
 * ACK. Caller owns g_wifi_operation; never call from the SDIO RX worker.
 * No callback may retain params or recursively enter this transport. */
typedef int (*skw_wpa_command)(void *,uint8_t,const void *,size_t);
struct skw_wpa_transport {
  skw_wpa_command command;
  void *arg;
  uint8_t own[6],bssid[6],peer,lmac;
  uint8_t attempted,installed;
  uint8_t initial_rsc[5][6]; /* For future host RX replay filter, not TX PN. */
  bool active,faulted;
  uint8_t scratch[22+SKW_EAPOL_MAX];
};
void skw_wpa_wipe(void *p,size_t n);
/* Zero-initialize first. Begin refuses to discard an active/uncertain session.
 * Only station instance 0 is supported by the existing command envelope. */
int skw_wpa_begin(struct skw_wpa_transport *,uint8_t instance,uint8_t lmac,
  uint8_t peer,const uint8_t own[6],const uint8_t bssid[6],skw_wpa_command,void *);
int skw_wpa_send_eapol(struct skw_wpa_transport *,const uint8_t *,size_t);
/* CCMP-128 only, initial install only: PTK id 0, GTK id 0..3.
 * Reject reinstall instead of resetting packet numbers. No data authorization.
 * seq is the initial host RX replay counter, length 0 or 6. */
int skw_wpa_install_ccmp(struct skw_wpa_transport *,bool pairwise,uint8_t id,
  const uint8_t *key,size_t key_len,const uint8_t *seq,size_t seq_len);
/* Try DEL_KEY even after an uncertain ADD_KEY; failed deletes remain pending.
 * Only a successful CLOSE/reset may justify discarding pending state. */
int skw_wpa_clear_keys(struct skw_wpa_transport *);
void skw_wpa_closed(struct skw_wpa_transport *);
#endif
