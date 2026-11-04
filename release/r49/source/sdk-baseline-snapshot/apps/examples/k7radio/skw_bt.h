/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K7_SKW_BT_H
#define K7_SKW_BT_H

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/wireless/bluetooth/bt_driver.h>
#include "skw_native.h"

/* The owner must finish chip identity, firmware boot, BTREADY and BT NV/HCI
 * initialization BEFORE ready can return 0. There is no implicit power-on.
 * send takes a complete zero-padded SDIO2 transfer and returns 0 or -errno.
 * link_ack updates the lower half's flow control; it is not HCI completion.
 * These callbacks must not recursively call this adapter.
 */
struct skw_bt_lower
{
  int (*ready)(void *ctx);
  int (*send)(void *ctx, const void *packet, size_t length);
  int (*link_ack)(void *ctx, uint8_t channel, uint16_t sequence);
};

struct skw_bt
{
  struct bt_driver_s driver;
  mutex_t lock;
  const struct skw_bt_lower *lower;
  void *ctx;
  bool opened;
  bool connected;
  uint16_t handle;
  unsigned int connections, disconnections;
  uint8_t disconnect_reason, encryption_status, encryption_enabled;
  unsigned int acl_rx, acl_tx;
  uint8_t adv_status[4]; /* 2008 data, 2009 response, 2006 params, 200a enable */
  uint8_t tx[2048];
};

/* Persistent caller-owned storage. No stack registration or device creation.
 * Owner calls bt_driver_register only when its real lower half is ready.
 * RX runs in worker context. Owner must quiesce RX before close/unregister
 * and before destroying this instance; receive synchronously borrows data.
 */
int skw_bt_init(struct skw_bt *bt, const struct skw_bt_lower *lower, void *ctx);
int skw_bt_receive_slot(struct skw_bt *bt, const void *slot, size_t available);

#endif
