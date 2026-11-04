#ifndef K7_HOST_BUFT_H
#define K7_HOST_BUFT_H
#include "ggml-backend.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit non-global allocation domain. All provider callbacks must be
 * nonthrowing; allocation and destruction externally serialized. The provider
 * and type must outlive ALL buffers/tensors, including ggml zero-size dummies.
 * On success allocate owns bytes aligned to alignment; release accepts precisely
 * that original pointer+size. No ordinary free is used on provider memory. */
struct k7_buft_provider {
    void *context;
    void *(*allocate)(void *context,size_t bytes,size_t alignment);
    void (*release)(void *context,void *memory,size_t bytes);
};
struct k7_buft_config {
    struct k7_buft_provider provider;
    size_t alignment;        /* power of two, CPU minimum .. 4096 */
    size_t max_buffer_bytes; /* nonzero alignment multiple */
    size_t total_budget;    /* aggregate padded live payload bytes only */
};
struct k7_buft_stats {
    size_t live_buffers,live_bytes,peak_bytes;
    int last_error;
};
/* *out must be NULL initially; no provider allocation during type creation. */
int k7_buft_create(const struct k7_buft_config *,ggml_backend_buffer_type_t *out);
/* EBUSY while nonzero buffers live. Owner must independently release all dummy
 * buffers and scheduler/model references first (ggml bypasses us for size=0). */
int k7_buft_destroy(ggml_backend_buffer_type_t *);
int k7_buft_get_stats(ggml_backend_buffer_type_t,struct k7_buft_stats *);
int k7_buft_round_bytes(size_t bytes,size_t alignment,size_t *rounded);
#ifdef K7_BUFT_TESTING
/* Inject ONE std::bad_alloc immediately before real wrapper construction.
 * Tests failure cleanup after real provider acquire; not actual system OOM. */
int k7_buft_test_fail_next_wrapper(ggml_backend_buffer_type_t);
#endif
#ifdef __cplusplus
}
#endif
#endif
