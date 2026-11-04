#ifndef VELAVISION_KWS_EMBEDDED_H
#define VELAVISION_KWS_EMBEDDED_H
#include "voicelink/kws_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Available only with a SHA-verified, separately linked model bundle. The
 * caller still supplies the real SHA implementation for runtime verification. */
int k7_kws_embedded_config(struct k7_kws_config *, k7_kws_sha256_fn, void *);
#ifdef __cplusplus
}
#endif
#endif
