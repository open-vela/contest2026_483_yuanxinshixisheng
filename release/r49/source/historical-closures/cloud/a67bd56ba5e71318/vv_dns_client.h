#ifndef VV_DNS_CLIENT_H
#define VV_DNS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VV_DNS_HOSTNAME_MAX 128u
#define VV_DNS_ATTEMPTS_MAX 3u

enum vv_dns_status
{
  VV_DNS_OK = 0,
  VV_DNS_INVALID_ARGUMENT = -1,
  VV_DNS_DEADLINE_EXCEEDED = -2,
  VV_DNS_TEMPORARY_FAILURE = -3,
  VV_DNS_NOT_FOUND = -4,
  VV_DNS_NO_IPV4_ADDRESS = -5,
  VV_DNS_SYSTEM_FAILURE = -6,
  VV_DNS_CANCELLED = -7
};

struct vv_dns_policy
{
  uint32_t deadline_ms;
  uint32_t retry_backoff_ms;
  unsigned int max_attempts;
};

struct vv_dns_result
{
  uint32_t ipv4_be;
  unsigned int attempts;
  int backend_code;
};

struct vv_dns_backend
{
  void *context;
  enum vv_dns_status (*resolve_ipv4)(void *context,
                                     const char *hostname,
                                     uint32_t timeout_ms,
                                     uint32_t *ipv4_be,
                                     int *backend_code);
  uint64_t (*now_ms)(void *context);
  void (*sleep_ms)(void *context, uint32_t delay_ms);
  bool (*cancelled)(void *context);
};

enum vv_dns_status vv_dns_resolve_ipv4(const char *hostname,
                                       const struct vv_dns_policy *policy,
                                       const struct vv_dns_backend *backend,
                                       struct vv_dns_result *result);

const char *vv_dns_status_name(enum vv_dns_status status);

#ifdef __cplusplus
}
#endif

#endif
