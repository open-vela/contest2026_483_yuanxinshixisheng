#ifndef NEAR_EVIDENCE_H
#define NEAR_EVIDENCE_H
#include "k7_vad.h"
#include <stdint.h>
/* Single echo-worker consumer. Borrow an EXCLUSIVE residual VAD instance;
 * never lend the concurrently running RAW model. This is an evidence producer,
 * not a clean authority. Thresholds need field validation before activation. */
struct ne_identity {
 uint64_t owner, reference_instance, reference_epoch, clock_epoch;
 uint64_t dialogue_instance, scope, lease, reply, mapping_revision;
 uint32_t input_epoch, tx_generation;
};
struct ne_policy {
 double minimum_residual_power, maximum_echo_correlation2;
 double minimum_residual_reference_ratio;
 float minimum_probability;
 unsigned consecutive_windows;
};
struct ne_report {
 struct ne_identity identity;
 struct k7_vad_observation model;
 uint64_t completing_first, completing_next, evidence_first, evidence_next;
 double microphone_power, reference_power, residual_power, correlation2;
 unsigned consecutive, candidate, qualified, clipped;
};
struct ne_worker;
int ne_create(struct k7_vad *,const struct ne_policy *,struct ne_worker **);
/* Explicit restart after gap/reply/reference/mapping/cancel boundary. */
int ne_begin(struct ne_worker *,const struct ne_identity *,uint64_t mic_first);
/* before/after are owner snapshots bracketing AEC processing; equal identity
 * alone is NOT calibration. Feed real aligned 320 samples only. Output support
 * covers actual model windows, never rounded up to completed 320-frame range. */
int ne_feed(struct ne_worker *,const struct ne_identity *before,
 const struct ne_identity *after,uint64_t first,const int16_t mic[320],
 const int16_t reference[320],const int16_t residual[320],struct ne_report *);
void ne_invalidate(struct ne_worker *);
void ne_destroy(struct ne_worker *);
#endif
