#ifndef VV_ALIYUN_ENDPOINT_H
#define VV_ALIYUN_ENDPOINT_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define VV_ALIYUN_LEGACY_HOST "dashscope.aliyuncs.com"
/* Endpoints only, never URLs/paths/ports, redirects or credentials. Paraformer
 * remains on its existing Beijing model. A selected hostname is immutable for
 * an operation; callers copy it before spawning work or retaining config. */
static inline bool vv_aliyun_speech_host_valid(const char *host)
{
  static const char suffix[] = ".cn-beijing.maas.aliyuncs.com";
  size_t n = 0, suffix_n = sizeof(suffix) - 1;
  if (!host) return false;
  while (n <= 128 && host[n]) ++n;
  if (!n || n > 128) return false;
  if (!strcmp(host, VV_ALIYUN_LEGACY_HOST)) return true;
  if (n <= suffix_n || n - suffix_n > 63 ||
      memcmp(host + n - suffix_n, suffix, suffix_n)) return false;
  size_t label_n = n - suffix_n;
  if (host[0] == '-' || host[label_n - 1] == '-') return false;
  /* One lower-case ASCII DNS tenant label, not only the ws- namespace. */
  for (size_t i = 0; i < label_n; ++i)
    if (!((host[i] >= 'a' && host[i] <= 'z') ||
          (host[i] >= '0' && host[i] <= '9') || host[i] == '-')) return false;
  return true;
}
#endif
