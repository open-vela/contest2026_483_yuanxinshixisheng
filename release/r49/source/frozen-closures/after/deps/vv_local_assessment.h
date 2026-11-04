#ifndef VV_LOCAL_ASSESSMENT_H
#define VV_LOCAL_ASSESSMENT_H
#include "vv_native_assessment.h"
#include <errno.h>
/* Explicit source44 policy; disabled for all existing build profiles. */
#ifndef K7_LOCAL_ASSESSMENT
#define K7_LOCAL_ASSESSMENT 0
#endif
#define VV_LOCAL_CAPTURE_COMPLETE "三视图采集成功"
#define VV_LOCAL_UPLOAD_BUDGET_MS 5000u
/* Strong board binding; no weak success fallback and no runtime ABI change. */
int vv_native_runtime_k7_discard(uint32_t epoch);
static inline bool vv_local_business_unreachable(int rc) {
 return rc==VV_HTTPS_EIO || rc==VV_HTTPS_ETIMEOUT;
}
static inline bool vv_local_business_unavailable(int rc) {
 return vv_local_business_unreachable(rc) ||
        rc==VV_BACKEND_CLIENT_HTTP || rc==VV_BACKEND_CLIENT_AUTH;
}
/* A local care suggestion is not a report. Preserve the result and backend
 * error; return zero only for accepted upload, bounded transport loss, or
 * business HTTP/auth failure. Trust, clock, memory and cancellation stay errors. */
static inline int vv_local_upload_result(const struct vv_assessment_result *r) {
 if(r->outcome==VV_ASSESSMENT_UPLOADED)return 0;
 if(r->outcome==VV_ASSESSMENT_CANCELLED)return -ECANCELED;
 if(r->outcome==VV_ASSESSMENT_MEMORY)return -ENOMEM;
 if(r->outcome==VV_ASSESSMENT_TIMEOUT &&
    (!r->backend_error || vv_local_business_unreachable(r->backend_error) ||
     r->backend_error==VV_HTTPS_ECANCELLED))return 0;
 if(r->outcome==VV_ASSESSMENT_BACKEND_ERROR &&
    vv_local_business_unavailable(r->backend_error))return 0;
 return r->backend_error?r->backend_error:-EPROTO;
}
#endif
