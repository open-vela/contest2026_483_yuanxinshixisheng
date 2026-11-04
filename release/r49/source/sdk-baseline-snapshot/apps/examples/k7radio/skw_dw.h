/* SPDX-License-Identifier: GPL-2.0-only */
/* RK3576 DW-MSHC PIO, based on the verified K7 Linux 6.1.75 SDK. */
#ifndef K7_SKW_DW_H
#define K7_SKW_DW_H
#include "skw_native.h"

struct skw_dw_io
{
  uint32_t (*read)(void *ctx, unsigned int offset);
  void (*write)(void *ctx, unsigned int offset, uint32_t value);
  uint64_t (*time_us)(void *ctx);
  void (*delay_us)(void *ctx, unsigned int us);
};

struct skw_dw
{
  const struct skw_dw_io *io;
  void *ctx;
  bool prepared;
  bool faulted;
  uint32_t last_raw;
  uint32_t last_status;
  uint32_t card_bytes;
  size_t host_bytes;
  int cleanup_error;
};

/* The owner MUST already hold exclusive SDMMC1 ownership, establish power,
 * clock/pins, enumerate/select the card and verify its CIS. prepare only
 * checks controller prerequisites; it does not enumerate or assert selection.
 * No use alongside another IRQ/DMA driver. All calls require owner locking.
 */
int skw_dw_prepare(struct skw_dw *dw, const struct skw_dw_io *io, void *ctx);
int skw_dw_cmd52(struct skw_dw *dw, uint32_t arg, uint32_t *r5);
int skw_dw_cmd53(struct skw_dw *dw, uint32_t arg, void *buffer,
                  size_t length, uint32_t *r5);
#endif
