#ifndef K7BOOT_NATIVE_H
#define K7BOOT_NATIVE_H
#include "../k7agent/cloud/include/vv_k7_platform_factory.h"
/* Register before worker start; config/loader args borrowed for boot lifetime.
 * No implicit trust authority or embedded credentials. */
int k7boot_native_configure(const struct vv_k7_platform_boot_config *,vv_k7_secure_load_fn,void *);
/* Explicit independent provider mode: no business login. Assessment stays
 * blocked by platform BUSINESS_AUTH until real business credentials exist. */
int k7boot_native_configure_speech_only(const struct vv_k7_platform_boot_config *);
int k7boot_native_worker(int,char **);
void k7boot_native_cancel(void);
#endif
