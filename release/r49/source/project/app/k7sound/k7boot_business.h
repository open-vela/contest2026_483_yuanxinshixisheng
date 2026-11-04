#ifndef K7BOOT_BUSINESS_H
#define K7BOOT_BUSINESS_H
#include "../k7agent/cloud/include/vv_k7_platform_factory.h"
/* Caller owns these refs through retirement. No default consent or endpoint.
 * random_bytes must provide fresh approved random bytes or fail closed. */
struct k7boot_business_config {
 vv_k7_secure_load_fn load;void *load_arg;
 const char *consent_ref;
 int (*random_bytes)(void *,unsigned char *,size_t);void *random_arg;
 unsigned poll_interval_ms,max_polls,total_timeout_ms;
};
struct k7boot_business_callbacks {
 struct vv_native_platform_config base;
 struct k7boot_business_config config;
 bool active;
 char session[33],boundary[33],key[33];
};
int k7boot_business_apply(struct vv_native_platform_config *,
 struct k7boot_business_callbacks *,const struct k7boot_business_config *);
/* Register before starting the production worker. Registration does not log in.
 * Enabled only by explicit Kconfig; no implicit private credential loading. */
int k7boot_aliyun_business_configure(const struct k7boot_business_config *);
/* Explicit private development initializer, available only with BUSINESS.
 * Reads static private arrays via public validated loader. Policy (dev-consent,
 * bounded polling) is public. Production binds approved factory entropy after
 * setup, overriding random_bytes/random_arg. Never logs secrets. */
int k7boot_aliyun_business_private_init(struct k7boot_business_config *);
#endif
