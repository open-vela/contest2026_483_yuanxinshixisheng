#include "k7_arena_diagnostic.h"
#include <nuttx/mm/k7_model_arena.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
/* Single diagnostic owner only; metadata BSS is not placed on NuttX task stack. */
static _Alignas(64) unsigned char metadata[32768];
int kap_diagnostic(struct kap_state *s,struct kap_report *r) {
    ggml_backend_buffer_type_t type=NULL;ggml_backend_buffer_t buffer=NULL;
    struct ggml_context *ctx=NULL;struct ggml_tensor *t[2]={NULL,NULL};
    struct k7_buft_config config;int rc=0;float input[64],output[64];
    size_t sizes[2],padded[2],alignment,total=0,offset=0;unsigned char *base;
    if(!s || !r)return EINVAL;
    memset(r,0,sizeof(*r));
    r->initialize_status=kap_initialize(s);
    if(r->initialize_status) {r->operation_error=EIO;return EIO;}
    r->available_before=k7_model_available();
    /* Make fixed metadata first. Upstream ggml_init metadata malloc can abort;
     * this candidate does not claim to turn that into a recoverable error. */
    ctx=ggml_init((struct ggml_init_params){sizeof(metadata),metadata,true});
    if(!ctx){rc=ENOMEM;goto done;}
    for(unsigned i=0;i<2;++i){t[i]=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,64,64);if(!t[i]){rc=ENOMEM;goto done;}}
    config=(struct k7_buft_config){kap_provider(s),64,KAP_LIMIT,KAP_LIMIT};
    rc=k7_buft_create(&config,&s->retained_type);type=s->retained_type;if(rc)goto done;
    alignment=ggml_backend_buft_get_alignment(type);
    if(alignment!=64){rc=EINVAL;goto done;}
    for(unsigned i=0;i<2;++i) {
        sizes[i]=ggml_backend_buft_get_alloc_size(type,t[i]);
        if(sizes[i]!=64u*64u*sizeof(float)){rc=EIO;goto done;}
        rc=k7_buft_round_bytes(sizes[i],alignment,&padded[i]);if(rc)goto done;
        if(padded[i]>KAP_LIMIT-total){rc=ENOMEM;goto done;}
        total+=padded[i];
    }
    if(total>ggml_backend_buft_get_max_size(type)){rc=ENOMEM;goto done;}
    /* Explicit one-buffer allocation: never enter alloc_ctx_tensors_from_buft
     * or its unchecked realloc/multi-buffer helper path. */
    buffer=ggml_backend_buft_alloc_buffer(type,total);
    if(!buffer){rc=ENOMEM;goto done;}
    base=ggml_backend_buffer_get_base(buffer);
    if(ggml_backend_buffer_get_type(buffer)!=type || ggml_backend_buffer_get_size(buffer)<total ||
       !kap_range_valid(base,total) || (uintptr_t)base>UINTPTR_MAX-total){rc=EFAULT;goto done;}
    for(unsigned i=0;i<2;++i) {
        /* Validate all preconditions of fixed b5046 tensor_alloc before call. */
        if(t[i]->buffer || t[i]->data || t[i]->view_src || offset>total || sizes[i]>total-offset ||
           ((uintptr_t)(base+offset)%alignment)){rc=EINVAL;goto done;}
        if(ggml_backend_tensor_alloc(buffer,t[i],base+offset)!=GGML_STATUS_SUCCESS){rc=EIO;goto done;}
        offset+=padded[i];
    }
    r->addresses_valid=1;
    for(unsigned i=0;i<2;++i) {
        if(!kap_range_valid(t[i]->data,ggml_nbytes(t[i]))){r->addresses_valid=0;rc=EFAULT;goto done;}
        for(unsigned row=0;row<64;++row) {
            for(unsigned x=0;x<64;++x)input[x]=(float)((int)(row*64+x)-2048+(int)i);
            ggml_backend_tensor_set(t[i],input,row*sizeof(input),sizeof(input));
            ggml_backend_tensor_get(t[i],output,row*sizeof(output),sizeof(output));
            for(unsigned x=0;x<64;++x){++r->values_checked;if(input[x]!=output[x])++r->mismatches;}
        }
    }
    if(r->mismatches)rc=EILSEQ;
done:
    if(buffer)ggml_backend_buffer_free(buffer);
    if(ctx)ggml_free(ctx);
    if(type){int destroy=k7_buft_destroy(&s->retained_type);if(destroy)rc=destroy;}
    r->available_after=k7_model_available();
    r->peak_bytes=s->peak_bytes;r->allocations=s->allocations;r->releases=s->releases;
    r->live_slots=kap_live_slots(s);r->errors=s->errors;r->quarantined=s->quarantined;
    r->returned_to_baseline=r->available_after==r->available_before && !r->live_slots && !s->live_bytes;
    if(!r->returned_to_baseline){++s->errors;r->errors=s->errors;}
    if(!r->returned_to_baseline || r->errors || r->quarantined)rc=EIO;
    r->operation_error=rc;return rc;
}
