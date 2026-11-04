#ifndef GGML_POOL_SAFE_H
#define GGML_POOL_SAFE_H
#include "ggml-cpu.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
enum ggml_pool_site {POOL_ALLOC=1,WORKERS_ALLOC,MUTEX_INIT,COND_INIT,ATTR_INIT,ATTR_STACK,THREAD_CREATE,THREAD_JOIN};
enum ggml_pool_resource {POOL_BYTES=1,POOL_MUTEX,POOL_COND,POOL_ATTR,POOL_THREAD,POOL_RUNNING,POOL_COMPUTE};
struct ggml_pool_hooks {
    void *ctx;
    /* Optional host fault injection. Nonzero rejects BEFORE the real operation.
       Never substitutes successful allocation, thread creation, or join. */
    int (*fail)(void *,enum ggml_pool_site,unsigned ordinal);
    /* Notification of actual acquisition/release. Must be thread-safe,
       nonblocking and remain valid until successful checked destruction. */
    void (*note)(void *,enum ggml_pool_resource,int delta,size_t bytes_or_index);
};
struct ggml_pool_options {
    size_t max_reserved_bytes; /* metadata + requested secondary stack bytes */
    size_t stack_bytes;        /* explicit per-secondary pthread stack request */
    unsigned max_threads;     /* includes caller doing compute */
    struct ggml_pool_hooks hooks;
};
struct ggml_pool_options ggml_pool_options_default(void);
/* Returns errno code, never creates on unsupported cpumask/priority.
   On error *out normally NULL. If cleanup fails, *out retains stopped storage;
   retry destroy_checked; MUST NOT compute with that quarantined pool. */
int ggml_pool_create_checked(const struct ggml_threadpool_params *,const struct ggml_pool_options *,struct ggml_threadpool **out);
/* Owner only, graph quiescent. Error preserves pointer and unjoined resources. */
int ggml_pool_destroy_checked(struct ggml_threadpool **);
int ggml_pool_required_bytes(int threads,size_t stack_bytes,size_t *out);
/* This candidate deliberately has no validated affinity adapter. */
bool ggml_pool_affinity_supported(void);
#ifdef __cplusplus
}
#endif
#endif
