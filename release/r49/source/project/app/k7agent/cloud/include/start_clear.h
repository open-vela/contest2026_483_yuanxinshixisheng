/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SAI_START_CLEAR_H
#define SAI_START_CLEAR_H
#include <stdint.h>
struct sc_port {
 void *ctx;
 int (*read)(void *,uint32_t offset,uint32_t *);
 int (*write)(void *,uint32_t offset,uint32_t value);
 uint64_t (*us)(void *);
};
struct sc_result {
 unsigned attempted,completed,held,needs_reapply,polls;
 int rc;uint32_t version,xfer,dmacr,status,clr_before,clr_after;
 uint32_t fifo_before[2],fifo_after[2];uint64_t start_us,end_us;
};
/* Zero-init result. Single explicit mode, never mixed with reset experiment.
 * trusted_quiescent means rails/clocks/MMU on, codec muted/ampoff, no worker,
 * IRQ or DMA owner, no prior failed stop. Must run BEFORE profile config.
 * Clears both TX/RX logic once via CLR bits0/1; no XFER/CSR/DMACR changes.
 * May discard old FIFO residue only under caller's ownership proof.
 * Self-clear <=1000us plus100000poll cap, all callbacks bounded/nonblocking.
 * Failure held forbids automatic retry/stream startup. Success still requires
 * profile reapply/readback; never bypass codec's readiness contract. */
int sc_once(const struct sc_port *,struct sc_result *,int trusted_quiescent);
#endif
