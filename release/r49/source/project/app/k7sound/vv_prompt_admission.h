#ifndef VV_PROMPT_ADMISSION_H
#define VV_PROMPT_ADMISSION_H
#if defined(K7_PROMPT_ROUND_ADMISSION) && K7_PROMPT_ROUND_ADMISSION
struct vv_native_runtime;
struct vv_https_cancel;
/* Serial runtime owner only, immediately before an actual unscoped prompt. */
int vv_native_platform_prompt_admit(struct vv_native_runtime *,const struct vv_https_cancel *);
#endif
#endif
