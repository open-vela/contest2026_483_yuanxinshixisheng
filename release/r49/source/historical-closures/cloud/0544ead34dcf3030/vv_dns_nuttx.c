#include "vv_dns_nuttx.h"
#include "../../../k7radio/skw_netdev.h"

#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>

#ifndef CONFIG_NETDB_DNSCLIENT_SEND_TIMEOUT
#  error "CONFIG_NETDB_DNSCLIENT_SEND_TIMEOUT is required"
#endif
#ifndef CONFIG_NETDB_DNSCLIENT_RECV_TIMEOUT
#  error "CONFIG_NETDB_DNSCLIENT_RECV_TIMEOUT is required"
#endif
#ifndef CONFIG_NETDB_DNSCLIENT_RETRIES
#  error "CONFIG_NETDB_DNSCLIENT_RETRIES is required"
#endif

#define VV_DNS_NUTTX_CALL_BOUND_MS \
  (1000u * (CONFIG_NETDB_DNSCLIENT_SEND_TIMEOUT + \
            CONFIG_NETDB_DNSCLIENT_RECV_TIMEOUT) * \
   CONFIG_NETDB_DNSCLIENT_RETRIES)

struct vv_dns_nuttx_context
{
  volatile const bool *cancel_flag;
};

static struct vv_dns_nuttx_context g_vv_dns_nuttx;

static uint64_t vv_dns_nuttx_now_ms(void *context)
{
  struct timespec value;
  (void)context;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
      return UINT64_MAX;
    }

  return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void vv_dns_nuttx_sleep_ms(void *context, uint32_t delay_ms)
{
  (void)context;
  usleep((useconds_t)delay_ms * 1000u);
}

static bool vv_dns_nuttx_cancelled(void *context)
{
  struct vv_dns_nuttx_context *state = context;
  return state->cancel_flag != NULL && *state->cancel_flag;
}

static enum vv_dns_status vv_dns_nuttx_resolve(void *context,
                                                const char *hostname,
                                                uint32_t timeout_ms,
                                                uint32_t *ipv4_be,
                                                int *backend_code)
{
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *cursor;
  int rc;
  struct in_addr literal;
  uint32_t dns_before=0,dns_after=0;
  (void)context;

  /* Numeric IPv4 needs no resolver call or DHCP DNS server. Keep the
   * synchronous getaddrinfo bound for names; caller deadline/cancel gates
   * still surround this backend, including the literal path. */
  if (inet_pton(AF_INET, hostname, &literal) == 1)
    {
      *ipv4_be = literal.s_addr;
      *backend_code = 0;
      return *ipv4_be != 0 ? VV_DNS_OK : VV_DNS_NO_IPV4_ADDRESS;
    }

  if (timeout_ms < VV_DNS_NUTTX_CALL_BOUND_MS)
    {
      return VV_DNS_DEADLINE_EXCEEDED;
    }

  rc=skw_netdev_dns_snapshot(&dns_before);
  if(rc || !dns_before)
    {
      *backend_code=rc?rc:-ENODATA;
      return VV_DNS_SYSTEM_FAILURE;
    }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  rc = getaddrinfo(hostname, NULL, &hints, &addresses);
  *backend_code = rc;
  if(skw_netdev_dns_snapshot(&dns_after) || dns_after!=dns_before)
    {
      if(addresses)freeaddrinfo(addresses);
      *backend_code=-ENETDOWN;
      return VV_DNS_SYSTEM_FAILURE;
    }
  if (rc != 0)
    {
      if (rc == EAI_AGAIN)
        {
          return VV_DNS_TEMPORARY_FAILURE;
        }

#ifdef EAI_NONAME
      if (rc == EAI_NONAME)
        {
          return VV_DNS_NOT_FOUND;
        }
#endif
#ifdef EAI_NODATA
      if (rc == EAI_NODATA)
        {
          return VV_DNS_NO_IPV4_ADDRESS;
        }
#endif
      return VV_DNS_SYSTEM_FAILURE;
    }

  for (cursor = addresses; cursor != NULL; cursor = cursor->ai_next)
    {
      if (cursor->ai_family == AF_INET &&
          cursor->ai_addrlen >= sizeof(struct sockaddr_in))
        {
          const struct sockaddr_in *address =
            (const struct sockaddr_in *)cursor->ai_addr;
          *ipv4_be = address->sin_addr.s_addr;
          freeaddrinfo(addresses);
          return *ipv4_be != 0 ? VV_DNS_OK : VV_DNS_NO_IPV4_ADDRESS;
        }
    }

  freeaddrinfo(addresses);
  return VV_DNS_NO_IPV4_ADDRESS;
}

int vv_dns_nuttx_backend_init(struct vv_dns_backend *backend,
                              volatile const bool *cancel_flag)
{
  if (backend == NULL)
    {
      return -EINVAL;
    }

  g_vv_dns_nuttx.cancel_flag = cancel_flag;
  memset(backend, 0, sizeof(*backend));
  backend->context = &g_vv_dns_nuttx;
  backend->resolve_ipv4 = vv_dns_nuttx_resolve;
  backend->now_ms = vv_dns_nuttx_now_ms;
  backend->sleep_ms = vv_dns_nuttx_sleep_ms;
  backend->cancelled = vv_dns_nuttx_cancelled;
  return 0;
}
