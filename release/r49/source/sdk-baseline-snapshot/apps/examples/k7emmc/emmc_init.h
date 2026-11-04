#ifndef VELAVISION_EMMC_INIT_H
#define VELAVISION_EMMC_INIT_H
#include <stdint.h>
#include <stddef.h>
enum vv_response { VV_NONE, VV_R1, VV_R1B, VV_R2, VV_R3 };
struct vv_card_io {
  void *ctx;
  /* Only initialization/read commands; budget in microseconds, bounded. */
  int (*command)(void *, unsigned, uint32_t, enum vv_response,
                 uint32_t response[4], uint8_t *data, uint32_t budget);
  uint64_t (*now_us)(void *);
  void (*pause)(void *);
};
struct vv_card {
  uint32_t ocr, sectors, cid[4], csd[4];
  unsigned last_command;
  uint8_t revision, partition, bus_width, timing;
  int ready;
};
/* Platform must have established legacy 1-bit, <=400kHz and stable supply.
 * Never switches partitions or writes EXT_CSD. Fails closed on unsupported
 * byte-addressed cards, non-user partitions or non-512-byte sectors.
 */
int vv_emmc_initialize(const struct vv_card_io *, struct vv_card *,
                       uint32_t timeout_us);
#endif
