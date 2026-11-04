#include "graph_probe.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static struct gp_state instance; /* BSS, not the NuttX task stack. */
static atomic_flag owner=ATOMIC_FLAG_INIT;
static int initialized;
static void report(unsigned threads,int rc) {
    struct gp_report r;gp_snapshot(&instance,&r);
    printf("threads_configured=%u result=%d operation=%d cleanup=%d compute=%d checked=%u mismatches=%u checksum=%" PRId64
           " worker_mask=%u starts=%u exits=%u pool_bytes=%ld handles=%ld running=%ld mutexes=%ld conds=%ld attrs=%ld"
           " context_used=%zu plan_work=%zu pool_budget=%zu retained_pool=%d retained_context=%d affinity=default_unbound physical_cpu_observed=0 weights=0\n",
           threads,rc,r.operation_error,r.cleanup_error,r.compute_status,r.checked_values,r.mismatches,r.checksum,
           r.worker_mask,r.starts,r.exits,r.pool_bytes,r.handles,r.running,r.mutexes,r.conds,r.attrs,
           r.context_used,r.plan_work,r.requested_pool_budget,r.retained_pool,r.retained_context);
}
int velavision_graph_probe_main(int argc,char **argv) {
    int rc=0;
    if(argc>2 || (argc==2 && strcmp(argv[1],"2") && strcmp(argv[1],"4") && strcmp(argv[1],"both") && strcmp(argv[1],"cleanup"))) {
        puts("usage: graph_probe [2|4|both|cleanup]");return EINVAL;
    }
    if(atomic_flag_test_and_set(&owner))return EBUSY;
    if(!initialized){gp_initialize(&instance);initialized=1;}
    if(argc==2 && !strcmp(argv[1],"cleanup")){rc=gp_cleanup(&instance);report(0,rc);}
    else {
        if(argc==1 || !strcmp(argv[1],"both") || !strcmp(argv[1],"2")){rc=gp_run(&instance,2);report(2,rc);}
        if(!rc && (argc==1 || !strcmp(argv[1],"both") || !strcmp(argv[1],"4"))){rc=gp_run(&instance,4);report(4,rc);}
    }
    atomic_flag_clear(&owner);return rc;
}
#ifdef GP_HOST_MAIN
int main(int argc,char **argv){return velavision_graph_probe_main(argc,argv);}
#endif
