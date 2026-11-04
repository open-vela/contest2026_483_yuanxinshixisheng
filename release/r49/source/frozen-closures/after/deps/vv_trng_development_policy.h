#ifndef VV_TRNG_DEVELOPMENT_POLICY_H
#define VV_TRNG_DEVELOPMENT_POLICY_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
enum vv_trng_development_state {VV_TD_UNTRIED,VV_TD_PROBING,VV_TD_APPROVED,VV_TD_FAILED,VV_TD_REVOKED,VV_TD_RENEWING};
/* Explicit Linux NS-source development policy, NOT certified entropy or proof
 * of secure-world exclusivity. Callbacks must not write clocks/reset/firewall.
 * begin reserves the sole normal-world wrapper for this boot; it accepts a
 * dynamic admission callback that may authorize probing before TLS approval.
 * All callbacks/state are borrowed for the entire boot. Zero-init before use. */
struct vv_trng_development_ops {
 bool (*context_valid)(void *,uint64_t generation);
 uint64_t (*now_ms)(void *);
 int (*begin)(void *,bool (*admitted)(void *),void *);
 ssize_t (*read_exact)(void *,uint8_t *,size_t,uint64_t);
 void *arg;
};
struct vv_trng_development_policy {
 atomic_int state;
 atomic_bool busy;
 struct vv_trng_development_ops ops;
 uint64_t generation,started_ms;
 atomic_uint_fast64_t expires_ms,last_validation_ms;
};
/* One attempt, discards two complete blocks; raw layer must preserve continuous
 * full-block repetition detection across probe and later reads. */
int vv_trng_development_probe(struct vv_trng_development_policy *,
 const struct vv_trng_development_ops *,uint64_t generation,uint64_t deadline_ms);
bool vv_trng_development_approved(void *);
ssize_t vv_trng_development_read(void *,uint8_t *,size_t,uint64_t deadline_ms);
void vv_trng_development_revoke(struct vv_trng_development_policy *);
/* Explicit optional maintenance, NEVER called from TLS/read/approved. Caller
 * must first prove its whole TLS/entropy user tree quiescent and keep the same
 * raw owner/boot context pinned throughout. RENEWING briefly denies admission.
 * Existing APPROVED unexpired lease only; same stored ops/raw owner, no begin,
 * configure, history reset, FAILED/REVOKED/expired recovery. Reads and discards
 * two complete raw blocks under a <=100ms deadline within the OLD lease.
 * On success publishes at most another10800000ms from final validation.
 * Stale expected generation and policy-trygate busy have no policy effects.
 * Every read_exact error, including -EBUSY, latches failure: that return code
 * alone cannot prove the raw core was untouched. Caller must exclude raw
 * concurrency. Raw repetition history MUST persist across
 * this call (including previous read -> first discard -> second -> next read).
 * Neither successful maintenance nor existing probe certifies entropy quality
 * or secure-world exclusivity. Every borrowed callback remains boot-lifetime. */
int vv_trng_development_renew(struct vv_trng_development_policy *,
 uint64_t expected_generation,uint64_t deadline_ms);
struct vv_trng_development_lease {
 enum vv_trng_development_state state;
 uint64_t generation,started_ms,expires_ms,last_validation_ms,now_ms,remaining_ms;
 bool clock_valid,expired;
};
/* Read-only scheduling/diagnostic observation; NOT permission to use entropy.
 * Uses the same trygate as probe/read/renew, no raw operation/begin/context gate,
 * no state transition even when now is invalid or lease expired. Returns0 with
 * explicit clock_valid/expired fields, EBUSY with untouched output on conflict.
 * UNTRIED has now_ms=UINT64_MAX and clock_valid=false. REVOKED may still have
 * positive numeric remaining_ms; callers must check state too. Concurrent
 * revoke can make a copied snapshot stale immediately. Whole-user quiescence
 * and renew's final checks, never snapshot alone, govern a maintenance call. */
int vv_trng_development_snapshot(struct vv_trng_development_policy *,
 struct vv_trng_development_lease *);
#endif
