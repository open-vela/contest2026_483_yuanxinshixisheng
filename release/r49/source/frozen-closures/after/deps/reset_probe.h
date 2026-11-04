/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SAI_RESET_PROBE_H
#define SAI_RESET_PROBE_H
#include <stdint.h>
enum sr_domain { SR_H, SR_M };
struct sr_port {
 void *ctx;
 int (*read_sai)(void *,uint32_t offset,uint32_t *);
 int (*set_reset)(void *,enum sr_domain,int asserted);
 int (*get_reset)(void *,enum sr_domain,int *asserted);
 uint64_t (*us)(void *);
};
struct sr_result {unsigned attempted,steps,completed,held,needs_reapply;int rc;
 uint32_t before[5],after[5];}; /* VERSION/TXCR/RXCR/PATH/DMACR */
/* One explicit experiment, result zero initialized. root_exclusive proves
 * rails/clocks/MMU valid, codec muted+ampoff, no DMA/IRQ/workers, reset mapping.
 * No SAI read during asserted reset. H pulse then M pulse, each edge followed
 * by10us, verified via get_reset. Callback failures latch held; no auto retry.
 * This does NOT restore configuration. completed still requires full profile
 * reapply+readback BEFORE audio use. Unknown reset mapping stays NOT_READY.
 */
int sr_once(const struct sr_port *,struct sr_result *,int root_exclusive);
#endif
