/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_FIFO_REFERENCE_H
#define K7_FIFO_REFERENCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define K7_FIFO_REF_SAMPLES 320u
#define K7_FIFO_REF_SLOTS 32u
enum k7_fifo_ref_kind { K7_FIFO_REF_BEGIN=1, K7_FIFO_REF_PCM, K7_FIFO_REF_END };
enum k7_fifo_ref_fault {
  K7_FIFO_REF_QUEUE=1u, K7_FIFO_REF_HALF_WRITE=2u, K7_FIFO_REF_IO=4u,
  K7_FIFO_REF_COUNT=8u, K7_FIFO_REF_CLOCK=16u, K7_FIFO_REF_CLEAR=32u,
  K7_FIFO_REF_STOP=64u, K7_FIFO_REF_OVERFLOW=128u
};
/* Every PCM element describes one successfully written L/R pair, with equal
 * channels. Zero bits identify deliberate refill silence; PCM value 0 alone
 * never identifies its origin. Sample ranges are half-open. first_source is
 * the next source index before this block, not an acoustic time coordinate.
 * before/after_us enclose successful enqueue operations, NOT DAC consumption.
 * END is always revoked diagnostic data, optionally retaining a partial tail;
 * fifo_empty means an observed empty FIFO, never completed acoustic output. */
struct k7_fifo_ref_record {
  uint64_t instance, first_enqueue, first_source, before_us, after_us;
  uint32_t epoch, generation, kind, faults, discarded_words;
  int result;
  uint16_t samples, source_samples;
  bool fifo_empty;
  uint8_t zero_bits[K7_FIFO_REF_SAMPLES/8];
  int16_t pcm[K7_FIFO_REF_SAMPLES];
};
struct k7_fifo_ref_status {
  uint64_t instance;
  uint32_t epoch, generation, faults;
  bool eligible, exhausted;
};
struct k7_fifo_reference;
/* Allocate only before threads start. All consumers must retain the parent
 * session until they stop and join; raw pointers/instance IDs are not retain.
 * Producer is the sole PIO owner. Consumer is one dedicated SPSC drain.
 * No allocation, locks, waits or callbacks in producer/consumer operations.
 * Errors revoke diagnostics only: callers MUST NOT propagate them into audio.
 * New BEGIN never resets ring p/c; exhausted identities require a new object. */
int k7_fifo_reference_create(struct k7_fifo_reference **);
void k7_fifo_reference_destroy(struct k7_fifo_reference *);
size_t k7_fifo_reference_storage_bytes(void);
void k7_fifo_reference_begin(struct k7_fifo_reference *, uint32_t generation);
void k7_fifo_reference_pair(struct k7_fifo_reference *, uint32_t generation,
  int16_t sample, bool explicit_zero, uint64_t source_index,
  uint64_t before_us, uint64_t after_us);
void k7_fifo_reference_revoke(struct k7_fifo_reference *, unsigned faults);
void k7_fifo_reference_end(struct k7_fifo_reference *, int result,
                           unsigned discarded_words, bool fifo_empty);
/* 1 record, 0 empty, -EPIPE invalidates all previously derived correlation;
 * old-epoch records are discarded. Recheck matches() at use/commit time even
 * after a successful take. Returned END is never eligible. No AEC alignment
 * or speaker-clock mapping is provided by this interface. */
int k7_fifo_reference_take(struct k7_fifo_reference *, struct k7_fifo_ref_record *);
/* Offline/diagnostic archive drain: sole consumer selects this OR take(),
 * never both, for the object's lifetime. Preserves queued old-epoch records
 * after revoke. Returned records carry their ORIGINAL identities; they are
 * historical bytes, NOT eligible reference. A missing range/control event,
 * full ring or current fault still invalidates completeness. No overwritten
 * history can be recovered. Caller must not feed these into action/AEC APIs. */
int k7_fifo_reference_take_archive(struct k7_fifo_reference *,struct k7_fifo_ref_record *);
int k7_fifo_reference_status(const struct k7_fifo_reference *, struct k7_fifo_ref_status *);
bool k7_fifo_reference_matches(const struct k7_fifo_reference *, uint64_t instance,
                               uint32_t epoch, uint32_t generation);
#endif
