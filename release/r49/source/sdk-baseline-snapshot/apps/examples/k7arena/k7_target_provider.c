#include "k7_target_provider.h"
#include <nuttx/mm/k7_model_arena.h>
#include <errno.h>
#include <limits.h>
int kap_range_valid(const void *memory,size_t bytes) {
    uint64_t address=(uint64_t)(uintptr_t)memory;
    if(!memory || !bytes || (address&63u) || address<K7_MODEL_ARENA_BASE ||
       (uint64_t)bytes>K7_MODEL_ARENA_SIZE)return 0;
    /* Subtraction avoids overflow in address+bytes and base+size. */
    return address-K7_MODEL_ARENA_BASE<=K7_MODEL_ARENA_SIZE-(uint64_t)bytes;
}
unsigned kap_live_slots(const struct kap_state *s) {
    unsigned n=0;if(!s)return 0;
    for(unsigned i=0;i<KAP_SLOTS;++i)if(s->slots[i].pointer)++n;
    return n;
}
int kap_initialize(struct kap_state *s) {
    int rc;if(!s)return -EINVAL;
    if(s->quarantined || s->errors || s->retained_type || kap_live_slots(s))return -EBUSY;
    if(s->ready)return 0;
    rc=k7_model_arena_initialize();s->initialize_status=rc;
    if(rc==0)s->ready=1;
    return rc;
}
static void *allocate(void *opaque,size_t bytes,size_t alignment) {
    struct kap_state *s=opaque;unsigned slot;void *p;
    if(!s || !s->ready || s->quarantined || alignment!=64 || !bytes || (bytes&63u) ||
       bytes>KAP_LIMIT || s->live_bytes>KAP_LIMIT || bytes>KAP_LIMIT-s->live_bytes) {
        if(s)++s->errors;
        return NULL;
    }
    for(slot=0;slot<KAP_SLOTS && s->slots[slot].pointer;++slot){}
    if(slot==KAP_SLOTS){++s->errors;return NULL;}
    p=k7_model_alloc(bytes);if(!p)return NULL;
    s->slots[slot]=(struct kap_slot){p,bytes,0};
    ++s->allocations;s->live_bytes+=bytes;
    if(s->live_bytes>s->peak_bytes)s->peak_bytes=s->live_bytes;
    if(!kap_range_valid(p,bytes)) {
        /* Real free API does NOT validate provenance. Never feed an out-of-range
         * or misaligned pointer into mm_free. Quarantine for target diagnosis. */
        ++s->errors;++s->quarantined;return NULL;
    }
    s->slots[slot].valid=1;return p;
}
static void release(void *opaque,void *p,size_t bytes) {
    struct kap_state *s=opaque;if(!s)return;
    for(unsigned i=0;i<KAP_SLOTS;++i) {
        if(s->slots[i].pointer==p && s->slots[i].bytes==bytes && s->slots[i].valid) {
            k7_model_free(p); /* void: actual reclaim additionally checked by available delta */
            s->live_bytes-=bytes;s->slots[i]=(struct kap_slot){0};++s->releases;return;
        }
    }
    ++s->errors; /* foreign, size-mismatched or repeated release: never call mm_free */
}
struct k7_buft_provider kap_provider(struct kap_state *s) {
    return (struct k7_buft_provider){s,allocate,release};
}

