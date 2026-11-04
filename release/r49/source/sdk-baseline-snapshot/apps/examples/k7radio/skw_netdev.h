/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SKW_NETDEV_H
#define SKW_NETDEV_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
int skw_netdev_open(const uint8_t mac[6]);
void skw_netdev_close(void);
int skw_netdev_rx(const uint8_t *frame,size_t length);
int skw_netdev_poll(int (*transmit)(const uint8_t *,size_t));
int skw_netdev_dhcp_start(void);
/* 0 pending, 1 has current lease; negative means failed/revoked. */
int skw_netdev_dhcp_status(char ip[16],char gateway[16]);
/* Copy applied DHCP option-6 resolver under lease lock. 0 means ready;
 * -ENODATA means IP lease has no DNS, other negative values retain failure.
 * No lookup or network I/O, no guessed gateway/public fallback. */
int skw_netdev_dns_snapshot(uint32_t *ipv4_be);
void skw_netdev_stats(void);
#endif
