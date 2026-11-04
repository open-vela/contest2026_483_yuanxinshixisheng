/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7DISPLAY_NATIVE_IO_H
#define K7DISPLAY_NATIVE_IO_H
#include "rk3576_dcphy.h"
#include <stdint.h>

/* One caller holds the display lease across all callbacks. bus_ready may
 * become true only after the real PMU/clock/reset transition was verified.
 * The dedicated profile maps these addresses in rk3576_boot.c. */
struct k7dn_io_state { unsigned bus_ready; };
int k7dn_read(void *, uintptr_t, uint32_t *);
int k7dn_write(void *, uintptr_t, uint32_t);
int k7dn_barrier(void *);
int k7dn_delay_us(void *, unsigned);
int k7dn_delay_ms(void *, unsigned);
int k7dn_link_read(void *, enum k7_display_block, uint32_t, uint32_t *);
int k7dn_link_write(void *, enum k7_display_block, uint32_t, uint32_t);
int k7dn_phy_reset(void *, bool);
int k7dn_vop_read(void *, uint32_t, uint32_t *);
int k7dn_vop_write(void *, uint32_t, uint32_t);
int k7dn_iommu_bypass(void *, uint32_t status[2]);
#endif
