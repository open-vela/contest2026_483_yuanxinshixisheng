#ifndef VV_ROUGHTIME_K7_H
#define VV_ROUGHTIME_K7_H
#include "vv_cloud_platform_diag.h"
#include "vv_https_client.h"
/* Single caller, before platform factory. No RTC or clock writes. */
int vv_roughtime_k7_fetch(const struct vv_platform_diag_authority *,
 const struct vv_https_cancel *,struct vv_trusted_time_sample *);
struct vv_cloud_tcp;
/* Runtime-only authenticated refresh. Borrow existing initialized resolver;
 * no singleton DNS reinit, RTC write, TLS request or service creation. Caller
 * owns serial whole-cloud idle exclusion until this synchronous call returns. */
int vv_roughtime_k7_fetch_runtime(const struct vv_platform_diag_authority *,
 struct vv_cloud_tcp *,const struct vv_https_cancel *,struct vv_trusted_time_sample *);
/* Bounded injectable datagram transport. resolve returns IPv4 network order;
 * receive/send return byte counts or negative errno; wait returns readiness,
 * zero for timeout, or negative errno. No operation may exceed deadline. */
struct vv_roughtime_io {
 void *arg;
 uint64_t (*now_ms)(void *);
 int (*nonce)(void *,uint8_t [64],uint64_t);
 int (*resolve)(void *,uint32_t *,uint64_t);
 int (*open_peer)(void *,uint32_t,uint16_t);
 int (*wait)(void *,int,bool,unsigned);
 ssize_t (*send)(void *,int,const uint8_t *,size_t);
 ssize_t (*receive)(void *,int,uint8_t *,size_t);
 void (*close)(void *,int);
};
int vv_roughtime_fetch_io(const struct vv_roughtime_io *,const struct vv_https_cancel *,
 struct vv_trusted_time_sample *);
#endif
