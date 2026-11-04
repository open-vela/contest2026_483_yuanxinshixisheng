#ifndef VV_K7_TRNG_DEVELOPMENT_H
#define VV_K7_TRNG_DEVELOPMENT_H
#include "vv_trng_development_policy.h"
/* Explicit one-shot NS development probe, no clock/reset/firewall writes.
 * Approval lasts only this boot (max 3 hours), never persists or auto-retries.
 * No random bytes logged. Not hardware certification/secure-world ownership. */
int vv_k7_trng_development_probe(void);
bool vv_k7_trng_development_approved(void *unused);
ssize_t vv_k7_trng_development_read(void *unused,uint8_t *,size_t,uint64_t);
void vv_k7_trng_development_revoke(void);
int vv_k7_trng_development_state(void);
/* Thin same-static-policy adapters; never choose/configure/probe a source. */
int vv_k7_trng_development_snapshot(struct vv_trng_development_lease *);
int vv_k7_trng_development_renew(uint64_t generation,uint64_t deadline_ms);
#endif
