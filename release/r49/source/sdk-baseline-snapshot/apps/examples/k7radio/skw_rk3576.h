/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K7_SKW_RK3576_H
#define K7_SKW_RK3576_H
#include <nuttx/mutex.h>
#include "skw_dw.h"

struct skw_rk3576
{
  struct skw_dw dw;
  mutex_t mutex;
};

/* Lifecycle operation, no concurrent callers. Owner must already exclusively
 * own SDMMC1 (including the legacy raw diagnostic), initialize/select the
 * card, obtain LIVE CIS IDs, enable F1 and set its block size to 512.
 * This checks F1 state on the card and connects the existing protocol layer.
 * It neither enumerates nor powers/boots the module nor registers a device.
 * Storage must outlive bus/RX workers. Never bind twice without unbind and
 * a complete, independently successful card/controller recovery.
 */
int skw_rk3576_bind_selected(struct skw_rk3576 *host, struct skw_bus *bus,
                             uint16_t vendor, uint16_t device);
/* Owner must stop/join all users first. This releases software only. */
void skw_rk3576_unbind(struct skw_rk3576 *host, struct skw_bus *bus);
#endif
