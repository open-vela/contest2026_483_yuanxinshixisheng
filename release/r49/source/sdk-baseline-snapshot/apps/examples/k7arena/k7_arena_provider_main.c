#include "k7_arena_diagnostic.h"
#include <stdio.h>
#include <errno.h>
#include <stdatomic.h>
static struct kap_state provider;
static atomic_flag owner=ATOMIC_FLAG_INIT;
int velavision_arena_provider_main(int argc,char **argv) {
    struct kap_report r;int rc;(void)argv;
    if(argc!=1)return EINVAL;
    if(atomic_flag_test_and_set(&owner))return EBUSY;
    rc=kap_diagnostic(&provider,&r);
    printf("arena_provider result=%d init_raw=%d before=%zu after=%zu peak=%zu checked=%u mismatches=%u allocations=%u releases=%u live=%u errors=%u quarantine=%u range=%d returned=%d limit=%u weights=0\n",
           rc,r.initialize_status,r.available_before,r.available_after,r.peak_bytes,r.values_checked,r.mismatches,
           r.allocations,r.releases,r.live_slots,r.errors,r.quarantined,r.addresses_valid,r.returned_to_baseline,KAP_LIMIT);
    atomic_flag_clear(&owner);return rc;
}
#ifdef KAP_HOST_MAIN
int main(int argc,char **argv){return velavision_arena_provider_main(argc,argv);}
#endif
