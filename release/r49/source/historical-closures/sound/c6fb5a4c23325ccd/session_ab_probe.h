/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_SESSION_AB_PROBE_H
#define K7_SESSION_AB_PROBE_H
#include "voice_session.h"
#define K7_AB_EVENTS 24u
struct k7_ab_config {
  const unsigned char *pcm;
  unsigned samples, cut[2], prebuffer, rebuffer, gap_ms, timeout_ms;
};
struct k7_ab_io {
  void *arg;
  uint64_t (*ms)(void *);
  void (*yield)(void *);
  bool (*cancelled)(void *);
};
struct k7_ab_pass {
  unsigned submitted, rendered, pauses, discarded, queued;
  uint32_t crc;
  uint64_t start_rx, end_rx;
  int tx_result;
};
struct k7_ab_result {
  struct k7_ab_pass pass[2];
  struct k7_voice_anchor event[K7_AB_EVENTS];
  unsigned events, rx_samples, rx_gaps, event_gaps;
  uint64_t rx_first;
  uint32_t source_crc, rx_crc;
  bool capture_complete, capture_truncated;
  int result;
};
/* One caller exclusively consumes KWS/render/anchors for this fresh session.
 * These are FIFO enqueue references, not a DAC acknowledgement. No logging,
 * network or second audio owner. Caller always stops/joins physical owner. */
int k7_session_ab_run(struct k7_voice_session *, const struct k7_ab_config *,
                      const struct k7_ab_io *, int16_t *, unsigned,
                      struct k7_ab_result *);
uint32_t k7_session_ab_crc(const unsigned char *, unsigned);
/* Explicit board diagnostic CLI; no arbitrary PCM paths or unbounded dump. */
int k7_session_ab_command(int, char **);
bool k7_session_ab_active(void);
void k7_session_ab_codec(unsigned, unsigned, int);
#endif
