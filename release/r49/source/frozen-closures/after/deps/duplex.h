/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SAI_DUPLEX_DIAG_H
#define SAI_DUPLEX_DIAG_H
#include <stdint.h>
#define DL_MAX_FRAMES 256u
#define DL_TRACE 64u
struct dl_port {
 void *ctx;
 int (*read)(void *,uint32_t,uint32_t *);
 int (*write)(void *,uint32_t,uint32_t);
 uint64_t (*us)(void *);
 /* Idempotent pure GPIO force-low AND verify; no enable callback exists. */
 int (*amp_low)(void *);
};
struct dl_word {uint64_t us;uint32_t before,word,after;int rc;};
struct dl_result {
 int result,stop_rc,restore_rc,amp_rc,held;
 int rx_restore_rc;
 uint32_t rxcr_before,rxcr_active,rxcr_restored;
 unsigned frames,tx_words,polls,ntx,nrx,max_tx,max_rx;
 uint64_t start_us,end_us;
 uint32_t path_before,path_active,path_restored;
 struct dl_word tx[DL_TRACE],rx[DL_TRACE];
};
/* Exclusive owner; powered SAI; callbacks nonblocking/ordered; same-CPU.
 * common_clock_verified confirms same-SAI shared CKR/FSCR master route.
 * No codec hook, amp enable, DMA, allocation or print. frames 8..256.
 * Capture/port/result must not alias. Caller owns capture >=2*frames words.
 * Error held forbids clock/power teardown; restore/stop must be checked.
 * Does not assert marker correctness: captures raw zeros and all data. */
int dl_run(const struct dl_port *,int prepared,int common_clock_verified,
 uint32_t mclk,uint32_t *capture,unsigned capacity,unsigned frames,
 struct dl_result *);
/* Only explicit RX_ALL mode changes PATH[15:8] to select SDI0 for all
 * RX paths. Requires frames=128. All original stop/amp/fault fences remain.
 * Default dl_run remains source compatible and preserves RX paths1..3. */
enum {DL_ROUTE_DEFAULT=0,DL_ROUTE_RX_ALL_SDI0=1};
int dl_run_route(const struct dl_port *,int,int,uint32_t,uint32_t *,unsigned,
 unsigned frames,int route,struct dl_result *);
/* Explicit 128-frame numbered source; RX route must be default e4xx.
 * Cannot combine with RX_ALL. n is enqueue WORD index, not FIFO bank/frame. */
int dl_numbered_marker(unsigned n,uint32_t *out); /* 0<=n<288, else -22 */
int dl_run_numbered(const struct dl_port *,int,int,uint32_t,uint32_t *,unsigned,
 struct dl_result *);
/* Single variable: RX CSR=2 lanes, TX remains1; numbered/default route.
 * Fixed128 received WORD PAIRS (not128 LRCK frames in this mode).
 * RX CSR restored after successful stop; rx_restore_rc failure retains held. */
int dl_run_numbered_rx2(const struct dl_port *,int,int,uint32_t,uint32_t *,unsigned,
 struct dl_result *);
/* Explicit guarded RX2: both observed banks0/1 must have>=2 entries;
 * unexpected banks/depth reject. Other entry points keep original readiness. */
int dl_run_numbered_rx2_guard(const struct dl_port *,int,int,uint32_t,uint32_t *,unsigned,
 struct dl_result *);
#endif
