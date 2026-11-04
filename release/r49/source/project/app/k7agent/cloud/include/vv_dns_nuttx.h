#ifndef VV_DNS_NUTTX_H
#define VV_DNS_NUTTX_H

#include "vv_dns_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create the production backend.  getaddrinfo() has no per-call timeout
 * argument: timeout_ms is therefore checked against the build-time resolver
 * bound before starting a call.  See config/dnsclient.fragment.
 */
int vv_dns_nuttx_backend_init(struct vv_dns_backend *backend,
                              volatile const bool *cancel_flag);

#ifdef __cplusplus
}
#endif

#endif
