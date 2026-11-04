#ifndef K7_APM21_PRODUCT_STARTUP_H
#define K7_APM21_PRODUCT_STARTUP_H
#if defined(K7_APM21_NOMINAL_EXPERIMENT) && K7_APM21_NOMINAL_EXPERIMENT
#include <stdint.h>
/* State-only query then exact-token claim. Query never loads or consumes.
 * Boot must claim before preparing model; release after joined cleanup. */
int k7_raw_vad_owner_product_startup_ready(uint64_t *token);
int k7_raw_vad_owner_product_startup_claim(uint64_t token);
int k7_raw_vad_owner_product_startup_release(uint64_t token);
/* Register a unique nominal-policy consumer only after explicit product-arm.
 * This does not arm, consume the arm, load a model, or start audio.
 * Token is compared only, never dereferenced. Release is nonblocking and only
 * clears this exact token; platform failure/destruction must release it. */
int k7_raw_vad_owner_product_policy_register(const void *token);
void k7_raw_vad_owner_product_policy_unregister(const void *token);
#endif
#endif
