#ifndef VV_REAL_UPLOAD_PRIVATE_H
#define VV_REAL_UPLOAD_PRIVATE_H
#include "vv_native_assessment.h"
typedef int (*vv_assessment_upload_fn)(void *,const struct vv_assessment_request *,
 char [37],const struct vv_https_cancel *);
enum vv_assessment_outcome vv_native_assessment_prepare_wait_upload_progress(
 struct vv_native_assessment *,const struct vv_assessment_request *,char *,size_t,size_t *,
 const struct vv_https_cancel *,struct vv_assessment_result *,
 int (*)(void *,const struct vv_https_cancel *),void *,vv_assessment_wait_fn,void *,
 vv_assessment_upload_fn,void *);
/* Caller holds runtime.busy and immutable admitted same-round JPEG/metadata.
 * One POST, no auth retry/replay. Main callback completes even if HTTP is done;
 * its own independent guard MUST also retain photo epoch/mask/HOLD identity.
 * A validated accepted UUID is retained diagnostically even on prompt failure;
 * only return 0 means successful callback and accepted upload together. */
int vv_native_platform_upload_task_progress(void *,const struct vv_assessment_request *,
 char [37],const struct vv_https_cancel *,
 int (*)(void *,const struct vv_https_cancel *),void *);
#endif
