#ifndef VV_WSS_CLIENT_H
#define VV_WSS_CLIENT_H
#include "vv_https_client.h"
/* Single-owner synchronous engine. All calls use absolute monotonic deadlines.
 * random MUST be a cryptographic RNG; sha1 is only RFC6455 Accept derivation.
 * No extensions/subprotocols offered. TLS transport must verify peer/SNI.
 * recv returns complete messages into caller storage, 1 for peer close, <0 error.
 * No live integration or board TLS acceptance is implied by host tests. */
struct vv_wss_client {
 const struct vv_https_transport_ops *io; void *io_arg;
 vv_https_now_ms_fn now; void *now_arg;
 int (*random)(void *,uint8_t *,size_t); void *random_arg;
 int (*sha1)(const uint8_t *,size_t,uint8_t digest[20]);
 size_t max_message; bool open;
};
int vv_wss_open(struct vv_wss_client *,const char *host,uint16_t port,
 const char *path,const char *authorization,uint64_t,const struct vv_https_cancel *);
int vv_wss_send(struct vv_wss_client *,unsigned opcode,const void *,size_t,
 uint64_t,const struct vv_https_cancel *);
int vv_wss_recv(struct vv_wss_client *,unsigned *opcode,void *,size_t,size_t *,
 uint64_t,const struct vv_https_cancel *);
void vv_wss_abort(struct vv_wss_client *);
#endif
