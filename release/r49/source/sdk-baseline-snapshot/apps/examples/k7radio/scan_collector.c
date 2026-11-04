/* SPDX-License-Identifier: Apache-2.0 */
#include "scan_collector.h"
#include <string.h>
void sc_init(struct sc_collector *c) { if (c) memset(c, 0, sizeof *c); }
static enum sc_rc active(struct sc_collector *c, uint64_t generation)
{
  if (!c || !generation) return SC_INVALID;
  if (generation != c->generation) return SC_STALE;
  return c->state == SC_COLLECTING ? SC_OK : SC_STATE;
}
enum sc_rc sc_begin(struct sc_collector *c, uint64_t generation)
{
  if (!c || !generation) return SC_INVALID;
  if (c->state == SC_COLLECTING) return SC_STATE;
  if (generation <= c->generation) return SC_STALE;
  memset(c, 0, sizeof *c);
  c->generation = generation; c->state = SC_COLLECTING;
  return SC_OK;
}
enum sc_rc sc_accept(struct sc_collector *c, uint64_t generation,
                     const struct sc_record *r)
{
  unsigned i;
  struct sc_record clean = {0};
  enum sc_rc rc = active(c, generation);
  if (rc != SC_OK) return rc;
  if (!r || r->ssid_len > 32 || r->security > 3 || r->band > 1 || !r->channel)
    return SC_INVALID;
  /* Normalize unused SSID bytes; copying fields avoids caller padding leaks.
   * Construct before writing so a caller alias into records remains safe. */
  memcpy(clean.ssid, r->ssid, r->ssid_len); clean.ssid_len = r->ssid_len;
  memcpy(clean.bssid, r->bssid, 6); clean.rssi = r->rssi;
  clean.security = r->security; clean.channel = r->channel; clean.band = r->band;
  for (i = 0; i < c->count; ++i) {
    if (!memcmp(c->records[i].bssid, clean.bssid, 6)) {
      if (clean.rssi <= c->records[i].rssi) return SC_IGNORED;
      c->records[i] = clean; return SC_OK;
    }
  }
  if (c->count == SC_CAPACITY) { c->truncated = 1; return SC_FULL; }
  c->records[c->count++] = clean;
  return SC_OK;
}
enum sc_rc sc_freeze(struct sc_collector *c, uint64_t generation)
{
  enum sc_rc rc = active(c, generation);
  if (rc == SC_OK) c->state = SC_FROZEN;
  return rc;
}
enum sc_rc sc_cancel(struct sc_collector *c, uint64_t generation)
{
  enum sc_rc rc = active(c, generation);
  if (rc != SC_OK) return rc;
  memset(c->records, 0, sizeof c->records);
  c->count = c->truncated = 0; c->state = SC_CANCELLED;
  return SC_OK;
}
