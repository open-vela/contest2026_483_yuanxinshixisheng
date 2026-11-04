/* SPDX-License-Identifier: GPL-2.0-only */
/* Protocol adapted from Seekwave's skw_platform_data.h, skw_sdio_rx.c and
 * skw_btdriver.c. Original copyright (C) Seekwave Tech Inc.; GPL version 2.
 * Explicit byte access replaces compiler-dependent bitfields/unaligned u32.
 */
#include "skw_native.h"
#include <errno.h>
#include <string.h>

static uint16_t get16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int skw_sdio2_encode(uint8_t channel, const void *payload, size_t length,
                      void *output, size_t capacity, size_t *written)
{
  uint8_t *dst = output;
  size_t padded, total;
  if (!written) return -EINVAL;
  *written = 0;
  if (!payload || !output || length == 0 || channel >= 12) return -EINVAL;
  if (length > SKW_MAX_PAYLOAD) return -EMSGSIZE;
  padded = (length + 3) & ~(size_t)3;
  total = (padded + 8 + SKW_BLOCK_SIZE - 1) & ~(size_t)(SKW_BLOCK_SIZE - 1);
  if (capacity < total) return -ENOSPC;
  /* Input and output must not overlap. Pad ourselves, never overread input
   * up to a rounded length as the vendor's original helper does.
   */
  memset(dst, 0, total);
  dst[0] = padded & 0xff;
  dst[1] = (uint8_t)(padded >> 8);
  dst[3] = channel;
  memcpy(dst + 4, payload, length);
  dst[4 + padded + 2] = 0x80;     /* EOF is header bit 23. */
  *written = total;
  return 0;
}

int skw_sdio2_decode_slot(const void *slot, size_t available,
                           struct skw_packet *packet)
{
  const uint8_t *src = slot;
  size_t length;
  if (!packet) return -EINVAL;
  memset(packet, 0, sizeof(*packet));
  if (!slot || available < 4) return -EMSGSIZE;
  /* The vendor's 7-bit pad field is not a must-be-zero reserved field.
   * Actual SWT6621S startup header is 0x013f004f. Linux ignores pad and
   * uses len + fixed 1536-byte slots; keep those bounds below unchanged.
   */
  if (src[2] & 0x80)
    {
      if (get16(src) != 0) return -EPROTO;
      packet->eof = true;
      return 0;
    }
  length = get16(src);
  if (length > SKW_MAX_PAYLOAD || length > available - 4) return -EMSGSIZE;
  /* Captured id24: fc 05 3f ff, valid=0. Linux skw_sdio2_adma_parser
   * discards channel 0xff slots, including a whole filler payload.
   * This is NOT EOF: valid packets can follow in the same read.
   */
  if (src[3] == 0xff) { packet->discard = true; return 0; }
  if (src[3] >= 12 || length == 0) return -EPROTO;
  packet->channel = src[3];
  packet->payload = src + 4;
  packet->length = length;
  return 0;
}

static int hci_length(uint8_t type, const uint8_t *data, size_t available,
                        size_t *length)
{
  size_t header, body;
  switch (type)
    {
      case 1: case 3:
        header = 3;
        if (available < header) return -EMSGSIZE;
        body = data[2];
        break;
      case 2:
        header = 4;
        if (available < header) return -EMSGSIZE;
        body = get16(data + 2);
        break;
      case 4:
        header = 2;
        if (available < header) return -EMSGSIZE;
        body = data[1];
        break;
      default:
        return -ENOTSUP;         /* ISO/vendor-log handling remains separate. */
    }
  *length = header + body;
  return *length <= available ? 0 : -EMSGSIZE;
}

int skw_hci_encode(uint8_t type, const void *data, size_t length,
                     void *output, size_t capacity, size_t *written)
{
  uint8_t packet[SKW_MAX_PAYLOAD];
  uint8_t channel;
  size_t expected;
  int ret;
  if (!written) return -EINVAL;
  *written = 0;
  if (!data || !output) return -EINVAL;
  if (length >= sizeof(packet)) return -EMSGSIZE;
  if (type == 1) channel = SKW_BT_CMD_PORT;
  else if (type == 2) channel = SKW_BT_DATA_PORT;
  else if (type == 3) channel = SKW_BT_AUDIO_PORT;
  else return -ENOTSUP;
  ret = hci_length(type, data, length, &expected);
  if (ret) return ret;
  if (expected != length) return -EPROTO;
  packet[0] = type;
  memcpy(packet + 1, data, length);
  return skw_sdio2_encode(channel, packet, length + 1, output, capacity, written);
}

int skw_hci_decode(const struct skw_packet *packet, struct skw_hci_packet *hci)
{
  const uint8_t *h4;
  uint8_t channel;
  size_t available, expected, i;
  int ret;
  if (!hci) return -EINVAL;
  memset(hci, 0, sizeof(*hci));
  if (!packet || packet->eof || !packet->payload) return -EINVAL;
  channel = packet->channel;
  if (channel != SKW_BT_CMD_PORT && channel != SKW_BT_DATA_PORT &&
      channel != SKW_BT_AUDIO_PORT) return -ENOTSUP;
  if (packet->length < 12) return -EMSGSIZE;
  hci->sequence = get16(packet->payload + 8);
  if (packet->length == 12)
    {
      hci->ack = true;
      return 0;
    }
  h4 = packet->payload + 12;
  available = packet->length - 13;
  if (h4[0] == 7)
    { hci->vendor_log = true; hci->data = h4 + 1; hci->length = available; return 0; }
  if (!((channel == SKW_BT_CMD_PORT && h4[0] == 4) ||
        (channel == SKW_BT_DATA_PORT && h4[0] == 2) ||
        (channel == SKW_BT_AUDIO_PORT && h4[0] == 3))) return -EPROTO;
  ret = hci_length(h4[0], h4 + 1, available, &expected);
  if (ret) return ret;
  /* Accept at most the alignment tail, not an accidental second HCI frame. */
  if (available - expected > 3) return -EPROTO;
  for (i = expected; i < available; i++)
    if (h4[1 + i]) return -EPROTO;
  hci->type = h4[0];
  hci->data = h4 + 1;
  hci->length = expected;
  return 0;
}
