#ifndef K7_ARENA_DIAGNOSTIC_H
#define K7_ARENA_DIAGNOSTIC_H
#include "k7_target_provider.h"
struct kap_report {
    int initialize_status,operation_error;
    size_t available_before,available_after,peak_bytes;
    unsigned values_checked,mismatches,allocations,releases,live_slots,errors,quarantined;
    int addresses_valid,returned_to_baseline;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Single synchronous owner. Small no-weight tensor set/get diagnostic.
 * State must outlive call, including quarantined pointers on failure. */
int kap_diagnostic(struct kap_state *,struct kap_report *);
int velavision_arena_provider_main(int,char **);
#ifdef __cplusplus
}
#endif
#endif
