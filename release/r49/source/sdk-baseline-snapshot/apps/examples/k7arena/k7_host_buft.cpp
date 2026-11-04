#include "k7_host_buft.h"
#include "ggml-backend-impl.h"
#include <cerrno>
#include <cstdint>
#include <new>
struct domain {
    ggml_backend_buffer_type type;
    k7_buft_config config;
    k7_buft_stats stats;
#ifdef K7_BUFT_TESTING
    bool fail_wrapper=false;
#endif
};
static ggml_backend_buffer_t allocate_buffer(ggml_backend_buffer_type_t,size_t);
static domain *get(ggml_backend_buffer_type_t type) {
    return type && type->iface.alloc_buffer==allocate_buffer?static_cast<domain*>(type->context):nullptr;
}
extern "C" int k7_buft_round_bytes(size_t bytes,size_t alignment,size_t *out) {
    if(!out || !alignment || alignment>4096 || (alignment&(alignment-1)))return EINVAL;
    if(bytes>SIZE_MAX-(alignment-1))return EOVERFLOW;
    *out=(bytes+alignment-1)&~(alignment-1);return 0;
}
static const char *name(ggml_backend_buffer_type_t){return "K7_ARENA_HOST";}
static size_t alignment(ggml_backend_buffer_type_t t){return get(t)->config.alignment;}
static size_t max_size(ggml_backend_buffer_type_t t){return get(t)->config.max_buffer_bytes;}
static bool host(ggml_backend_buffer_type_t){return true;}
static void release_buffer(ggml_backend_buffer_t buffer) {
    domain *d=get(buffer->buft);size_t padded=0;
    /* Fields are immutable owner-private values established by allocate_buffer. */
    (void)k7_buft_round_bytes(buffer->size,d->config.alignment,&padded);
    d->config.provider.release(d->config.provider.context,buffer->context,padded);
    d->stats.live_bytes-=padded;--d->stats.live_buffers;
}
static ggml_backend_buffer_t allocate_buffer(ggml_backend_buffer_type_t type,size_t bytes) {
    domain *d=get(type);size_t padded=0;
    int error=k7_buft_round_bytes(bytes,d->config.alignment,&padded);
    if(error){d->stats.last_error=error;return nullptr;}
    if(!bytes || padded>d->config.max_buffer_bytes || padded>d->config.total_budget-d->stats.live_bytes) {
        d->stats.last_error=ENOMEM;return nullptr;
    }
    void *p=d->config.provider.allocate(d->config.provider.context,padded,d->config.alignment);
    if(!p){d->stats.last_error=ENOMEM;return nullptr;}
    if(reinterpret_cast<uintptr_t>(p)%d->config.alignment) {
        d->config.provider.release(d->config.provider.context,p,padded);
        d->stats.last_error=EINVAL;return nullptr;
    }
    ggml_backend_buffer_t buffer=nullptr;
    try {
#ifdef K7_BUFT_TESTING
        if(d->fail_wrapper){d->fail_wrapper=false;throw std::bad_alloc();}
#endif
        /* Fixed b5046 wrapper borrows raw data; reuse its CPU get/set/clear/copy
         * callbacks, then install this explicit domain and owning free callback. */
        buffer=ggml_backend_cpu_buffer_from_ptr(p,bytes);
    } catch(const std::bad_alloc &) {error=ENOMEM;}
      catch(...) {error=EIO;}
    if(!buffer) {
        d->config.provider.release(d->config.provider.context,p,padded);
        d->stats.last_error=error?error:ENOMEM;return nullptr;
    }
    buffer->buft=type;buffer->iface.free_buffer=release_buffer;
    d->stats.live_bytes+=padded;++d->stats.live_buffers;
    if(d->stats.live_bytes>d->stats.peak_bytes)d->stats.peak_bytes=d->stats.live_bytes;
    d->stats.last_error=0;return buffer;
}
extern "C" int k7_buft_create(const k7_buft_config *c,ggml_backend_buffer_type_t *out) {
    size_t dummy;
    if(!out)return EINVAL;
    if(*out)return EBUSY;
    if(!c || !c->provider.allocate || !c->provider.release ||
       k7_buft_round_bytes(0,c->alignment,&dummy) ||
       c->alignment<ggml_backend_buft_get_alignment(ggml_backend_cpu_buffer_type()) ||
       !c->max_buffer_bytes || c->max_buffer_bytes%c->alignment ||
       c->max_buffer_bytes>c->total_budget)return EINVAL;
    domain *d=new(std::nothrow) domain{};if(!d)return ENOMEM;
    d->config=*c;
    d->type={{name,allocate_buffer,alignment,max_size,nullptr,host},nullptr,d};
    *out=&d->type;return 0;
}
extern "C" int k7_buft_destroy(ggml_backend_buffer_type_t *type) {
    if(!type)return EINVAL;
    if(!*type)return 0;
    domain *d=get(*type);if(!d)return EINVAL;
    if(d->stats.live_buffers)return EBUSY;
    delete d;*type=nullptr;return 0;
}
extern "C" int k7_buft_get_stats(ggml_backend_buffer_type_t type,k7_buft_stats *out) {
    domain *d=get(type);if(!d || !out)return EINVAL;*out=d->stats;return 0;
}
#ifdef K7_BUFT_TESTING
extern "C" int k7_buft_test_fail_next_wrapper(ggml_backend_buffer_type_t type) {
    domain *d=get(type);if(!d)return EINVAL;d->fail_wrapper=true;return 0;
}
#endif
