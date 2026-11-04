/* SPDX-License-Identifier: GPL-2.0-only */
/* SWT6621S skw_mgmt_hdr wire layout; byte reads avoid unaligned access. */
#ifndef SKW_WIFI_DIAG_H
#define SKW_WIFI_DIAG_H
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
struct skw_scan_record {
  uint8_t channel, band, bssid[6], ssid[32], ssid_len;
  int16_t rssi;
};
static inline unsigned int skw_diag_u16(const uint8_t *p)
{ return p[0] | ((unsigned int)p[1] << 8); }
static inline int skw_diag_scan(const uint8_t *p, size_t n,
                                struct skw_scan_record *out)
{
  size_t length, pos;
  const uint8_t *m;
  if (!p || !out) return -EINVAL;
  memset(out, 0, sizeof(*out));
  if (n < 44) return -EMSGSIZE;
  length = skw_diag_u16(p + 4);
  if (length < 36 || length > n - 8) return -EMSGSIZE;
  m = p + 8;
  if ((skw_diag_u16(m) & 0xfc) != 0x80 &&
      (skw_diag_u16(m) & 0xfc) != 0x50) return -EPROTO;
  out->channel = p[0]; out->band = p[1];
  out->rssi = (int16_t)skw_diag_u16(p + 2);
  memcpy(out->bssid, m + 16, 6);
  for (pos = 36; pos < length; )
    {
      size_t bytes;
      if (length - pos < 2) return -EPROTO;
      bytes = m[pos + 1];
      if (bytes > length - pos - 2) return -EPROTO;
      if (m[pos] == 0)
        {
          if (bytes > 32) return -EPROTO;
          out->ssid_len = bytes;
          memcpy(out->ssid, m + pos + 2, bytes);
        }
      pos += bytes + 2;
    }
  return 0;
}
#endif
