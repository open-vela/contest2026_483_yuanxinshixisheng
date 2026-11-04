/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_SCAN_COLLECTOR_H
#define K7_SCAN_COLLECTOR_H
#include <stdint.h>
#define SC_CAPACITY 64u
enum sc_state { SC_IDLE, SC_COLLECTING, SC_FROZEN, SC_CANCELLED };
enum sc_rc { SC_OK, SC_IGNORED, SC_FULL, SC_INVALID, SC_STATE, SC_STALE };
struct sc_record {
  uint8_t ssid[32], ssid_len, bssid[6];
  int16_t rssi;
  uint8_t security, channel, band;
};
struct sc_collector {
  uint64_t generation;
  enum sc_state state;
  uint8_t count, truncated;
  struct sc_record records[SC_CAPACITY];
};
/* Initialize once before publication. All calls and reads require external
 * serialization. The caller must not mutate fields. No internal locks/heap.
 * generation strictly increases, nonzero; exhaustion requires a new lifetime.
 * It binds a LOCAL slot only: caller must prove firmware report provenance.
 * init must not be used to bypass active ownership or erase a published slot. */
void sc_init(struct sc_collector *c);
enum sc_rc sc_begin(struct sc_collector *c, uint64_t generation);
/* Fully decoded record required. security 0=open 1=WPA 2=RSN 3=protected;
 * band 0=2.4G 1=5G; channel 1..255. No string interpretation. */
enum sc_rc sc_accept(struct sc_collector *c, uint64_t generation,
                     const struct sc_record *record);
/* freeze is one-way for this generation; consume under same external lock.
 * cancel only accepts COLLECTING, wipes records, retains generation tombstone.
 * A higher-generation begin can reuse FROZEN/CANCELLED only after the caller
 * has released snapshot readers and proven the old worker/RX quiescent. */
enum sc_rc sc_freeze(struct sc_collector *c, uint64_t generation);
enum sc_rc sc_cancel(struct sc_collector *c, uint64_t generation);
#endif
