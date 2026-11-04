/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_SAI_PLATFORM_H
#define K7_SAI_PLATFORM_H
#include <stdint.h>
struct sap_port {
 void *ctx;
 int (*read)(void *,uintptr_t,uint32_t *);
 int (*write)(void *,uintptr_t,uint32_t);
 uint64_t (*us)(void *);
};
struct sap_state {
 unsigned attempted,saved,modified,ready,held;
 uint32_t old[10],version,repair,ack,idle,clk46,fraction;
 uint32_t mclkout_before,mclkout_after;
};
/* Zero-init state before single-owner lifetime. exclusive is trusted proof:
 * no users of audio_frac0/SAI1/selected pins; codec muted and amp off;
 * MMU/rails/HCLK_AUDIO parent and reset deassertion confirmed by root.
 * No blind SAI access before repair+ack+idle+clock readback.
 * Does NOT start stream, reset codec, touch amp or prove acoustic readiness. */
int sap_setup(const struct sap_port *,struct sap_state *,int exclusive);
/* quiescent: root proves no stream/DMA/worker plus >=2 BCLK since FS idle.
 * Restore only saved pin/clock fields; leave AUDIO power ON/de-idled.
 * Never power off shared AUDIO or retry failed transition automatically. */
int sap_cleanup(const struct sap_port *,struct sap_state *,int quiescent);
#endif
