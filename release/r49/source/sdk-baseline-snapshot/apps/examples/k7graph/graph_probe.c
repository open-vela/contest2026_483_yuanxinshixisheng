#include "graph_probe.h"
#include <errno.h>
#include <string.h>
static void note(void *opaque,enum ggml_pool_resource resource,int delta,size_t value) {
    struct gp_state *s=opaque;struct gp_witness *w=&s->witness;
    switch(resource) {
    case POOL_BYTES:atomic_fetch_add(&w->bytes,(long)value*delta);break;
    case POOL_MUTEX:atomic_fetch_add(&w->mutexes,delta);break;
    case POOL_COND:atomic_fetch_add(&w->conds,delta);break;
    case POOL_ATTR:atomic_fetch_add(&w->attrs,delta);break;
    case POOL_THREAD:atomic_fetch_add(&w->handles,delta);break;
    case POOL_RUNNING:
        atomic_fetch_add(&w->running,delta);
        if(delta>0)atomic_fetch_add(&w->starts,1);else atomic_fetch_add(&w->exits,1);break;
    case POOL_COMPUTE:if(value<32)atomic_fetch_or(&w->compute_mask,1u<<(unsigned)value);break;
    }
}
#ifdef GP_TESTING
static int reject(void *opaque,enum ggml_pool_site site,unsigned ordinal) {
    struct gp_state *s=opaque;
    return (s->fail_site==site && s->fail_ordinal==ordinal) || (site==THREAD_JOIN && s->fail_join);
}
#define STOP_STAGE(n) do{if(s->fail_stage==(n)){error=ECANCELED;goto finish;}}while(0)
#else
#define STOP_STAGE(n) ((void)0)
#endif
void gp_initialize(struct gp_state *s) {
    memset(s,0,sizeof(*s));
    atomic_init(&s->witness.bytes,0);atomic_init(&s->witness.mutexes,0);
    atomic_init(&s->witness.conds,0);atomic_init(&s->witness.attrs,0);
    atomic_init(&s->witness.handles,0);atomic_init(&s->witness.running,0);
    atomic_init(&s->witness.starts,0);atomic_init(&s->witness.exits,0);
    atomic_init(&s->witness.compute_mask,0);
}
void gp_snapshot(struct gp_state *s,struct gp_report *out) {
    struct gp_witness *w=&s->witness;
    s->report.worker_mask=atomic_load(&w->compute_mask);
    s->report.starts=atomic_load(&w->starts);s->report.exits=atomic_load(&w->exits);
    s->report.pool_bytes=atomic_load(&w->bytes);s->report.mutexes=atomic_load(&w->mutexes);
    s->report.conds=atomic_load(&w->conds);s->report.attrs=atomic_load(&w->attrs);
    s->report.handles=atomic_load(&w->handles);s->report.running=atomic_load(&w->running);
    s->report.retained_pool=s->pool!=NULL;s->report.retained_context=s->ctx!=NULL;
    *out=s->report;
}
int gp_cleanup(struct gp_state *s) {
    int error;
    if(!s)return EINVAL;
    error=ggml_pool_destroy_checked(&s->pool);
    s->report.cleanup_error=error;
    if(error)return error; /* Keep plan, graph arena, context, hooks alive. */
    if(s->ctx){ggml_free(s->ctx);s->ctx=NULL;}
    memset(&s->plan,0,sizeof(s->plan));
    return 0;
}
int gp_run(struct gp_state *s,unsigned threads) {
    struct ggml_threadpool_params params;
    struct ggml_pool_options options;
    struct ggml_tensor *a,*b,*product;struct ggml_cgraph *graph;
    struct ggml_init_params ip;size_t required;int error=0,cleanup;
    if(!s)return EINVAL;
    if(s->pool || s->ctx)return EBUSY;
    memset(&s->report,0,sizeof(s->report));
    s->report.compute_status=-1;s->report.configured_threads=threads;
    if(threads!=2 && threads!=4){s->report.operation_error=EINVAL;return EINVAL;}
    /* Static fixed graph admission; no user-controlled shape/arithmetic. */
    required=3*ggml_tensor_overhead()+3*64*64*sizeof(float)+ggml_graph_overhead_custom(16,false);
    if(required>sizeof(s->arena)){s->report.operation_error=ENOMEM;return ENOMEM;}
    atomic_store(&s->witness.compute_mask,0);atomic_store(&s->witness.starts,0);atomic_store(&s->witness.exits,0);
    ggml_cpu_init();
    params=ggml_threadpool_params_default((int)threads);params.poll=0;
    options=ggml_pool_options_default();options.stack_bytes=GP_STACK_BYTES;
    options.max_threads=4;options.max_reserved_bytes=1024u*1024u;
    options.hooks.ctx=s;options.hooks.note=note;
#ifdef GP_TESTING
    options.hooks.fail=reject;
#endif
    error=ggml_pool_required_bytes((int)threads,options.stack_bytes,&s->report.requested_pool_budget);
    if(error)goto finish;
    error=ggml_pool_create_checked(&params,&options,&s->pool);
    if(error)goto finish;
    STOP_STAGE(1);
    ip=(struct ggml_init_params){sizeof(s->arena),s->arena,false};
    /* Upstream allocates its small context through aborting GGML_MALLOC.
       This NULL check does NOT convert upstream OOM abort to recoverable error. */
    s->ctx=ggml_init(ip);if(!s->ctx){error=ENOMEM;goto finish;}
    STOP_STAGE(2);
    a=ggml_new_tensor_2d(s->ctx,GGML_TYPE_F32,64,64);
    b=ggml_new_tensor_2d(s->ctx,GGML_TYPE_F32,64,64);
    if(!a || !b){error=ENOMEM;goto finish;}
    product=ggml_mul(s->ctx,a,b);
    if(!a || !b || !product){error=ENOMEM;goto finish;}
    for(int i=0;i<4096;++i){((float*)a->data)[i]=(float)(i%17-8);((float*)b->data)[i]=(float)(i%7+1);}
    STOP_STAGE(3);
    graph=ggml_new_graph_custom(s->ctx,16,false);
    if(!graph){error=ENOMEM;goto finish;}
    ggml_build_forward_expand(graph,product);
    s->report.context_used=ggml_used_mem(s->ctx);
    s->plan=ggml_graph_plan(graph,(int)threads,s->pool);
    s->report.plan_work=s->plan.work_size;
    if(s->plan.work_size>sizeof(s->work)){error=ENOMEM;goto finish;}
    s->plan.work_data=s->plan.work_size?s->work:NULL;
    STOP_STAGE(4);
    s->report.compute_status=ggml_graph_compute(graph,&s->plan);
    if(s->report.compute_status!=GGML_STATUS_SUCCESS){error=EIO;goto finish;}
#ifdef GP_TESTING
    if(s->fail_stage==5)((float*)product->data)[11]+=1.0f;
#endif
    for(int i=0;i<4096;++i) {
        float expected=(float)((i%17-8)*(i%7+1));float actual=((float*)product->data)[i];
        ++s->report.checked_values;
        if(actual!=expected)++s->report.mismatches;
        else s->report.checksum+=(int64_t)actual;
    }
    if(s->report.mismatches)error=EILSEQ;
    else if(atomic_load(&s->witness.compute_mask)!=((1u<<threads)-2u))error=EIO;
finish:
    s->report.operation_error=error;
    cleanup=gp_cleanup(s);
    return cleanup?cleanup:error;
}


