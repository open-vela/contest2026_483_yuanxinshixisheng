/* SPDX-License-Identifier: GPL-2.0-only */
/* Seekwave protocol adaptation. See README.md for source provenance. */
#ifndef K7_SKW_NATIVE_H
#define K7_SKW_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SKW_BLOCK_SIZE 512u
#define SKW_RX_SLOT_SIZE 1536u
#define SKW_MAX_PAYLOAD 1532u
#define SKW_BT_CMD_PORT 2u
#define SKW_BT_AUDIO_PORT 3u
#define SKW_BT_DATA_PORT 5u
#define SKW_WIFI_CMD_PORT 6u
#define SKW_WIFI_DATA_PORT 7u

/* All callbacks are synchronous: 0 means the WHOLE operation completed,
 * negative errno means failure. Positive/short results are rejected.
 * The lower half must supply bounded timeouts and CMD/data error handling.
 * cmd53 must not modify a TX buffer. No callback may retry a partial write.
 * lock/unlock cover the entire indirect-address + data transaction.
 */
struct skw_bus_ops
{
  int (*lock)(void *ctx);
  void (*unlock)(void *ctx);
  int (*cmd52)(void *ctx, uint32_t arg, uint32_t *r5);
  int (*cmd53)(void *ctx, uint32_t arg, void *buffer, size_t length,
               uint32_t *r5);
};

struct skw_bus
{
  const struct skw_bus_ops *ops;
  void *ctx;
  bool attached;
  bool faulted;
  bool lite_verified;
};

/* Call attach only AFTER card enumeration, selection and function-1 enable.
 * IDs must come from the live CIS, not board labels or a DT compatible.
 * attach is not a recovery operation: after a fault, reset/re-enumerate first.
 * Initialization and lifetime changes must be serialized by the owner.
 */
int skw_bus_attach(struct skw_bus *bus, const struct skw_bus_ops *ops,
                   void *ctx, uint16_t vendor, uint16_t device);
int skw_r5_status(uint32_t response);
int skw_mem_read(struct skw_bus *bus, uint32_t address, void *buffer,
                 size_t length, size_t *completed);
int skw_mem_write(struct skw_bus *bus, uint32_t address, const void *buffer,
                  size_t length, size_t *completed);
int skw_probe_lite(struct skw_bus *bus, uint8_t chip_id[16]);
uint16_t skw_crc16(uint16_t crc, const void *buffer, size_t length);
/* A prepared region only: caller supplies reviewed module memory address,
 * correct image/NV variant and CRC from a trusted manifest. This never sets
 * download-done or starts firmware. CRC is integrity, not authentication.
 */
int skw_stage_region(struct skw_bus *bus, uint32_t address,
                       const void *data, size_t length, uint16_t expected_crc,
                       size_t *completed);

/* Pure SDIO2 codec, selected only after SV6160LITE identity is verified.
 * TX uses a 4-byte header, padded payload, EOF and zeroed block padding.
 * RX SDMA advances by 1536-byte slots, NOT by each packet's length.
 * The RX view borrows input memory; its lifetime ends with that buffer.
 */
struct skw_packet
{
  uint8_t channel;
  bool eof;
  bool discard;                  /* Vendor channel 0xff empty ADMA slot. */
  const uint8_t *payload;
  size_t length;
};

int skw_sdio2_encode(uint8_t channel, const void *payload, size_t length,
                      void *output, size_t capacity, size_t *written);
int skw_sdio2_decode_slot(const void *slot, size_t available,
                           struct skw_packet *packet);

struct skw_hci_packet
{
  uint8_t type;
  uint16_t sequence;
  const uint8_t *data;             /* HCI header + body, without H4 type */
  size_t length;
  bool vendor_log;               /* Vendor H4 type 0x07, never an HCI event. */
  bool ack;                      /* 12-byte link ACK, not an HCI event */
};

int skw_hci_encode(uint8_t type, const void *data, size_t length,
                     void *output, size_t capacity, size_t *written);
int skw_hci_decode(const struct skw_packet *packet,
                     struct skw_hci_packet *hci);

#endif
