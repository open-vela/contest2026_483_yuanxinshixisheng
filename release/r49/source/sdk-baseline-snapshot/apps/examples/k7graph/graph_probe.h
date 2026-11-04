#ifndef VV_GRAPH_PROBE_H
#define VV_GRAPH_PROBE_H
#include "ggml-pool-safe.h"
#include <stdatomic.h>
#include <stdint.h>
/* C11 API; one owner; statically allocate (large object, never task stack).
 * Never copy/reinitialize/release this object while pool or ctx is retained. */
#define GP_ARENA_BYTES (128u*1024u)
#define GP_WORK_BYTES 4096u
#define GP_STACK_BYTES (256u*1024u)
struct gp_witness {
    atomic_long bytes,mutexes,conds,attrs,handles,running;
    atomic_uint starts,exits,compute_mask;
};
struct gp_report {
    int operation_error,cleanup_error,compute_status;
    unsigned configured_threads,checked_values,mismatches,worker_mask,starts,exits;
    long pool_bytes,mutexes,conds,attrs,handles,running;
    size_t context_used,plan_work,requested_pool_budget;
    int retained_pool,retained_context; int64_t checksum;
};
struct gp_state {
    struct ggml_threadpool *pool;
    struct ggml_context *ctx;
    struct ggml_cplan plan;
    struct gp_witness witness;
    struct gp_report report;
#ifdef GP_TESTING
    enum ggml_pool_site fail_site; unsigned fail_ordinal;
    int fail_join,fail_stage;
#endif
    _Alignas(64) unsigned char arena[GP_ARENA_BYTES];
    _Alignas(64) unsigned char work[GP_WORK_BYTES];
};
/* Call once on fresh storage. Atomic counters are initialized by this function. */
void gp_initialize(struct gp_state *);
/* Returns errno; performs cleanup before return if possible. On cleanup failure,
 * state owns the quarantined pool AND graph/plan/context; retain it and retry
 * gp_cleanup only after owner knows graph execution has returned. */
int gp_run(struct gp_state *,unsigned threads);
int gp_cleanup(struct gp_state *);
void gp_snapshot(struct gp_state *,struct gp_report *);
int velavision_graph_probe_main(int argc,char **argv);
#endif
