#ifndef K7BOOT_ALIYUN_PRODUCTION_AUTHORITY_H
#define K7BOOT_ALIYUN_PRODUCTION_AUTHORITY_H
#include "../k7agent/cloud/include/vv_cloud_platform_diag.h"
/* Public board policy, separate from private credentials. A successful return
 * means the authority structure was assembled, NOT that all trust gates pass.
 * RTC source approval remains explicit; factory must freshly read/decode RTC.
 * Retention certification is not claimed by RTC source approval. */
int k7boot_aliyun_production_authority(struct vv_platform_diag_authority *,bool *ca_approved);
/* Fixed public audited CA, independent of the private credential include. */
const unsigned char *k7boot_aliyun_production_ca(size_t *length);
/* TRNG-only attachment after explicit probe; does not approve RTC or CA. */
int k7boot_aliyun_development_trng_authority(struct vv_platform_diag_authority *);
#endif
