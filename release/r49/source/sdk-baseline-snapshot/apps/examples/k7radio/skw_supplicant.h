/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SKW_SUPPLICANT_H
#define SKW_SUPPLICANT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "skw_wpa_transport.h"
struct skw_supplicant;
/* One serialized Wi-Fi worker owns all calls, including timer dispatch. */
struct skw_supplicant *skw_supplicant_start(struct skw_wpa_transport *transport,
  const uint8_t *ssid,size_t ssid_len,const char *password,
  const uint8_t *ap_rsn,size_t ap_rsn_len,
  int (*random_bytes)(uint8_t *,size_t));
int skw_supplicant_assoc_ie(struct skw_supplicant *,uint8_t *,size_t *);
void skw_supplicant_associated(struct skw_supplicant *);
int skw_supplicant_receive(struct skw_supplicant *,const uint8_t *,size_t);
int skw_supplicant_poll(struct skw_supplicant *);
bool skw_supplicant_completed(struct skw_supplicant *);
void skw_supplicant_stop(struct skw_supplicant *);
void skw_supplicant_timers(void);
#endif
