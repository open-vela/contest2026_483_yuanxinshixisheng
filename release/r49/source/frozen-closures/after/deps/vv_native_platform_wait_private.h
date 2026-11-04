#ifndef VV_NATIVE_PLATFORM_WAIT_PRIVATE_H
#define VV_NATIVE_PLATFORM_WAIT_PRIVATE_H
struct vv_native_runtime;
struct vv_backend_task;
struct vv_backend_brief;
struct vv_https_cancel;
int vv_native_platform_wait_task(void *,const char *,struct vv_backend_task *,
 struct vv_backend_brief *,const struct vv_https_cancel *);
int vv_native_platform_wait_task_progress(void *,const char *,struct vv_backend_task *,
 struct vv_backend_brief *,const struct vv_https_cancel *,
 int (*started)(void *,const struct vv_https_cancel *),void *);
/* Original service owner, runtime.busy held. Control-only ASR, finalized STOP
 * permitted; never query/upload, dispatch chat, capture or run generic listen. */
int vv_native_runtime_wait_control(struct vv_native_runtime *,unsigned,
 const struct vv_https_cancel *);
#endif
