#ifndef VV_REPLY_SCOPE_H
#define VV_REPLY_SCOPE_H
#include <stdbool.h>
#include <stdint.h>
/* Diagnostic identity and retirement only. No cancellation/ASR/motion action,
 * no AEC proof. All calls are worker/control context, never PIO or ISR.
 * Keep this object alive until all observers and borrowed leases retire.
 * destroy/owner-change are serial service-owner operations. */
struct vv_reply_scope;
struct vv_reply_scope_token {
  struct vv_reply_scope *scope;
  uint64_t incarnation, serial, owner_epoch, reply_id;
  uint32_t asr_epoch;
};
enum vv_reply_scope_phase { VV_SCOPE_IDLE, VV_SCOPE_RESERVED,
  VV_SCOPE_SPEAKER, VV_SCOPE_BOUND, VV_SCOPE_TX_RETIRED,
  VV_SCOPE_CLOSED, VV_SCOPE_QUARANTINED };
struct vv_reply_scope_status {
  struct vv_reply_scope_token token;
  uint32_t tx_generation;
  enum vv_reply_scope_phase phase;
  bool reply_retired, speaker_joined, tx_retired, rx_live;
  /* Diagnostic permission only; caller must revalidate current owner/RX and
   * use a fresh ASR epoch. It does not start ASR or assert clean audio. */
  bool handoff_eligible;
};
int vv_reply_scope_create(uint64_t audio_owner_epoch,struct vv_reply_scope **);
int vv_reply_scope_destroy(struct vv_reply_scope *);
int vv_reply_scope_set_owner(struct vv_reply_scope *,uint64_t new_owner_epoch);
int vv_reply_scope_begin(struct vv_reply_scope *,uint64_t expected_owner_epoch,
  uint32_t asr_epoch,uint64_t reply_id,struct vv_reply_scope_token *);
int vv_reply_scope_snapshot(struct vv_reply_scope *,struct vv_reply_scope_status *);
int vv_reply_scope_speaker_begin(const struct vv_reply_scope_token *);
/* Called once after actual TX admission. Epoch is read from the live manager,
 * not copied from token to pretend it was independently verified. */
int vv_reply_scope_bind(const struct vv_reply_scope_token *,uint64_t observed_owner,
  uint32_t tx_generation);
/* Coordinator alone reports actual tx_result + pthread_join + live RX checks.
 * Failed/unknown retirement quarantines; it never permits next-turn reuse. */
int vv_reply_scope_speaker_end(const struct vv_reply_scope_token *,uint32_t tx_generation,
  bool tx_retired,bool network_joined,bool rx_live);
int vv_reply_scope_speaker_safe(const struct vv_reply_scope_token *);
/* Dialogue owner calls only after storage cleanup and busy=false publication. */
int vv_reply_scope_reply_end(const struct vv_reply_scope_token *);
enum { VV_SCOPE_STALE = -21001, VV_SCOPE_OWNER_UNSAFE = -21002 };
#endif
