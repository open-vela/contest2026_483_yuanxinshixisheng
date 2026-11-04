#ifndef VV_CLOUD_TCP_H
#define VV_CLOUD_TCP_H
#include "vv_dns_client.h"
#include "vv_https_client.h"

/* All operations return negative errno on failure. wait_connected returns
 * 0 for timeout, 1 for readiness (including POLLERR/HUP); socket_error must
 * then query SO_ERROR. open_socket transfers ownership to this adapter. */
struct vv_cloud_tcp_ops {
  int (*open_socket)(void *arg);
  int (*nonblocking)(void *arg, int fd);
  int (*connect_ipv4)(void *arg, int fd, uint32_t address_be, uint16_t port);
  int (*wait_connected)(void *arg, int fd, unsigned timeout_ms);
  int (*socket_error)(void *arg, int fd);
  void (*close_socket)(void *arg, int fd);
};
struct vv_cloud_tcp {
  struct vv_dns_backend dns;
  struct vv_dns_policy dns_policy;
  const struct vv_cloud_tcp_ops *ops;
  void *ops_arg;
};
/* Compatible with vv_https_tcp_connect_fn. Success transfers a connected,
 * still nonblocking fd to caller. All failures close any acquired fd once.
 * DNS remains synchronous: cancellation cannot interrupt getaddrinfo; the
 * supplied DNS backend must enforce its documented maximum call bound.
 * Without CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM, use one owner per DNS backend
 * (the NuttX backend has global state). With it, resolves through this adapter
 * share a mutex; its cancellable wait consumes the DNS/TCP deadline and needs
 * the backend sleep_ms callback when contended. TCP waits never hold the lock.
 * Initialize backend state before workers start and keep it unchanged; callers
 * outside this adapter still need their own DNS ownership coordination.
 * now_ms must be monotonic and return UINT64_MAX on clock failure. */
int vv_cloud_tcp_connect(void *arg, const char *host, uint16_t port,
                         uint64_t deadline_ms,
                         const struct vv_https_cancel *cancel);
/* Reuse the exact DNS owner/deadline/cancel machinery without opening TCP.
 * Used by authenticated runtime Roughtime UDP. Does not reinitialize DNS. */
int vv_cloud_tcp_resolve_ipv4(struct vv_cloud_tcp *,const char *,uint64_t,
 const struct vv_https_cancel *,uint32_t *);
/* Real socket operations; separate translation unit, requires POSIX sockets. */
const struct vv_cloud_tcp_ops *vv_cloud_tcp_posix_ops(void);
#endif
