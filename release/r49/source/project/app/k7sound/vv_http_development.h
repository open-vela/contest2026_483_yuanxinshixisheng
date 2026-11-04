#ifndef VV_HTTP_DEVELOPMENT_H
#define VV_HTTP_DEVELOPMENT_H
#include "vv_https_mbedtls.h"
struct vv_http_development {
 struct vv_https_mbedtls_config config;
 int fd;
};
/* Caller initializes fd=-1. No TLS or certificate/security assertions. */
const struct vv_https_transport_ops *vv_http_development_ops(void);
void vv_http_development_close(struct vv_http_development *);
#endif
