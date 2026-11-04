#ifndef K7_TARGET_PROVIDER_H
#define K7_TARGET_PROVIDER_H
#include "k7_host_buft.h"
#include <stdint.h>
#define KAP_LIMIT (1024u*1024u)
#define KAP_SLOTS 8u
struct kap_slot {void *pointer;size_t bytes;int valid;};
/* Zero-initialize once. Noncopyable by contract, external single owner.
 * Keep alive for ALL buft callbacks; do not reset if slots/quarantine remain. */
struct kap_state {
    ggml_backend_buffer_type_t retained_type;
    struct kap_slot slots[KAP_SLOTS];
    size_t live_bytes,peak_bytes;
    unsigned allocations,releases,errors,quarantined;
    int ready,initialize_status;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Preserves actual k7_model_arena_initialize return (0 / negative NuttX errno). */
int kap_initialize(struct kap_state *);
struct k7_buft_provider kap_provider(struct kap_state *);
int kap_range_valid(const void *,size_t);
unsigned kap_live_slots(const struct kap_state *);
#ifdef __cplusplus
}
#endif
#endif

