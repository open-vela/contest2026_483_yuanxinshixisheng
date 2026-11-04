#include "vv_dns_client.h"

#include <string.h>

static bool vv_dns_hostname_valid(const char *hostname)
{
  size_t length = 0;
  size_t label_length = 0;
  size_t i;

  if (hostname == NULL)
    {
      return false;
    }

  while (length <= VV_DNS_HOSTNAME_MAX && hostname[length] != '\0')
    {
      ++length;
    }

  if (length == 0 || length > VV_DNS_HOSTNAME_MAX)
    {
      return false;
    }

  for (i = 0; i < length; ++i)
    {
      unsigned char c = (unsigned char)hostname[i];
      if (c == '.')
        {
          if (label_length == 0 || hostname[i - 1] == '-')
            {
              return false;
            }

          label_length = 0;
          continue;
        }

      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-'))
        {
          return false;
        }

      if (label_length == 0 && c == '-')
        {
          return false;
        }

      if (++label_length > 63u)
        {
          return false;
        }
    }

  return label_length != 0 && hostname[length - 1] != '-';
}

static bool vv_dns_is_cancelled(const struct vv_dns_backend *backend)
{
  return backend->cancelled != NULL && backend->cancelled(backend->context);
}

enum vv_dns_status vv_dns_resolve_ipv4(const char *hostname,
                                       const struct vv_dns_policy *policy,
                                       const struct vv_dns_backend *backend,
                                       struct vv_dns_result *result)
{
  uint64_t started;
  unsigned int attempt;
  enum vv_dns_status status = VV_DNS_SYSTEM_FAILURE;

  if (result != NULL)
    {
      memset(result, 0, sizeof(*result));
    }

  if (!vv_dns_hostname_valid(hostname) || policy == NULL || backend == NULL ||
      result == NULL || backend->resolve_ipv4 == NULL || backend->now_ms == NULL ||
      policy->deadline_ms == 0 || policy->max_attempts == 0 ||
      policy->max_attempts > VV_DNS_ATTEMPTS_MAX)
    {
      return VV_DNS_INVALID_ARGUMENT;
    }

  started = backend->now_ms(backend->context);
  for (attempt = 0; attempt < policy->max_attempts; ++attempt)
    {
      uint64_t now;
      uint64_t elapsed;
      uint32_t remaining;

      if (vv_dns_is_cancelled(backend))
        {
          return VV_DNS_CANCELLED;
        }

      now = backend->now_ms(backend->context);
      elapsed = now >= started ? now - started : policy->deadline_ms;
      if (elapsed >= policy->deadline_ms)
        {
          return VV_DNS_DEADLINE_EXCEEDED;
        }

      remaining = policy->deadline_ms - (uint32_t)elapsed;
      result->attempts = attempt + 1u;
      result->backend_code = 0;
      status = backend->resolve_ipv4(backend->context, hostname, remaining,
                                     &result->ipv4_be,
                                     &result->backend_code);

      now = backend->now_ms(backend->context);
      elapsed = now >= started ? now - started : policy->deadline_ms;
      if (elapsed >= policy->deadline_ms)
        {
          result->ipv4_be = 0;
          return VV_DNS_DEADLINE_EXCEEDED;
        }

      if (vv_dns_is_cancelled(backend))
        {
          result->ipv4_be = 0;
          return VV_DNS_CANCELLED;
        }

      if (status == VV_DNS_OK)
        {
          if (result->ipv4_be == 0)
            {
              return VV_DNS_NO_IPV4_ADDRESS;
            }

          return VV_DNS_OK;
        }

      result->ipv4_be = 0;
      if (status != VV_DNS_TEMPORARY_FAILURE ||
          attempt + 1u == policy->max_attempts)
        {
          return status;
        }

      if (policy->retry_backoff_ms > 0)
        {
          uint32_t sleep_ms;
          now = backend->now_ms(backend->context);
          elapsed = now >= started ? now - started : policy->deadline_ms;
          if (elapsed >= policy->deadline_ms)
            {
              return VV_DNS_DEADLINE_EXCEEDED;
            }

          remaining = policy->deadline_ms - (uint32_t)elapsed;
          sleep_ms = policy->retry_backoff_ms < remaining
                     ? policy->retry_backoff_ms : remaining;
          if (backend->sleep_ms != NULL)
            {
              backend->sleep_ms(backend->context, sleep_ms);
            }
        }
    }

  return status;
}

const char *vv_dns_status_name(enum vv_dns_status status)
{
  switch (status)
    {
      case VV_DNS_OK: return "ok";
      case VV_DNS_INVALID_ARGUMENT: return "invalid_argument";
      case VV_DNS_DEADLINE_EXCEEDED: return "deadline_exceeded";
      case VV_DNS_TEMPORARY_FAILURE: return "temporary_failure";
      case VV_DNS_NOT_FOUND: return "not_found";
      case VV_DNS_NO_IPV4_ADDRESS: return "no_ipv4_address";
      case VV_DNS_SYSTEM_FAILURE: return "system_failure";
      case VV_DNS_CANCELLED: return "cancelled";
      default: return "unknown";
    }
}
