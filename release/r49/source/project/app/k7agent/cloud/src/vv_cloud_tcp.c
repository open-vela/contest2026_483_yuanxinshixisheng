#include "vv_cloud_tcp.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
#include <pthread.h>
/* The NuttX DNS backend has shared resolver state. Connection/TLS ownership
 * stays per caller; only the synchronous backend resolve is serialized. */
static pthread_mutex_t dns_owner = PTHREAD_MUTEX_INITIALIZER;
#endif

struct call { struct vv_cloud_tcp *tcp; uint64_t deadline, last;
  const struct vv_https_cancel *cancel; int failure; };
static int check(struct call *c)
{
  uint64_t now;
  if (c->failure) return c->failure;
  if ((c->cancel && c->cancel->is_cancelled &&
       c->cancel->is_cancelled(c->cancel->arg)) ||
      (c->tcp->dns.cancelled && c->tcp->dns.cancelled(c->tcp->dns.context)))
    return c->failure = -ECANCELED;
  now = c->tcp->dns.now_ms(c->tcp->dns.context);
  if (now == UINT64_MAX || now < c->last) return c->failure = -EIO;
  c->last = now;
  return now >= c->deadline ? (c->failure = -ETIMEDOUT) : 0;
}
static uint64_t dns_now(void *arg)
{ struct call *c = arg; (void)check(c); return c->last; }
static bool dns_cancel(void *arg) { return check(arg) != 0; }
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
static int acquire_dns(struct call *c, uint64_t deadline)
{
  for (;;) {
    int rc = check(c);
    if (rc) return rc;
    if (c->last >= deadline) return c->failure = -ETIMEDOUT;
    rc = pthread_mutex_trylock(&dns_owner);
    if (!rc) return 0;
    if (rc != EBUSY) return c->failure = -rc;
    /* Never spin indefinitely when a custom backend cannot yield. */
    if (!c->tcp->dns.sleep_ms) return c->failure = -ENOSYS;
    c->tcp->dns.sleep_ms(c->tcp->dns.context, 1);
  }
}
#endif
static enum vv_dns_status resolve(void *arg, const char *host, uint32_t ms,
                                  uint32_t *ip, int *code)
{
  struct call *c = arg; uint64_t left;
  if (check(c)) return VV_DNS_CANCELLED;
  left = c->deadline - c->last;
  if (left < ms) ms = (uint32_t)left;
#ifdef CONFIG_EXAMPLES_K7CLOUD_REPLY_STREAM
  /* ms is the remaining DNS policy budget, which can be shorter than the
   * TCP deadline. Waiting for another resolver consumes that same budget. */
  uint64_t resolve_deadline = c->last + ms;
  int rc = acquire_dns(c, resolve_deadline);
  if (rc) { *code = rc; return VV_DNS_SYSTEM_FAILURE; }
  enum vv_dns_status status = VV_DNS_SYSTEM_FAILURE;
  rc = check(c);
  if (!rc && c->last >= resolve_deadline) rc = c->failure = -ETIMEDOUT;
  if (!rc) {
    ms = (uint32_t)(resolve_deadline - c->last);
    status = c->tcp->dns.resolve_ipv4(c->tcp->dns.context, host, ms, ip, code);
  } else *code = rc;
  rc = pthread_mutex_unlock(&dns_owner);
  if (rc) {
    if (!c->failure) c->failure = -rc;
    *code = -rc;
    status = VV_DNS_SYSTEM_FAILURE;
  }
  return status;
#else
  return c->tcp->dns.resolve_ipv4(c->tcp->dns.context, host, ms, ip, code);
#endif
}
static void sleep_dns(void *arg, uint32_t ms)
{
  struct call *c = arg;
  while (ms && !check(c)) {
    uint32_t slice = ms > 50 ? 50 : ms;
    if (c->deadline - c->last < slice) slice = (uint32_t)(c->deadline-c->last);
    if (!c->tcp->dns.sleep_ms) return;
    c->tcp->dns.sleep_ms(c->tcp->dns.context, slice); ms -= slice;
  }
}
int vv_cloud_tcp_resolve_ipv4(struct vv_cloud_tcp *t,const char *host,
 uint64_t deadline,const struct vv_https_cancel *cancel,uint32_t *ip){
 if(ip)*ip=0;
 if(!t||!host||!ip||!t->dns.now_ms||!t->dns.resolve_ipv4)return -EINVAL;
 struct call c={t,deadline,0,cancel,0};int code=0,rc=check(&c);
 if(rc)return rc;
 uint64_t left=deadline-c.last;
 uint32_t ms=left>UINT32_MAX?UINT32_MAX:(uint32_t)left;
 enum vv_dns_status s=resolve(&c,host,ms,ip,&code);
 if((rc=check(&c))){*ip=0;return rc;}
 if(s==VV_DNS_OK&&*ip)return 0;
 *ip=0;
 return s==VV_DNS_DEADLINE_EXCEEDED?-ETIMEDOUT:
   s==VV_DNS_CANCELLED?-ECANCELED:s==VV_DNS_TEMPORARY_FAILURE?-EAGAIN:
   s==VV_DNS_NOT_FOUND||s==VV_DNS_NO_IPV4_ADDRESS?-EHOSTUNREACH:-EIO;
}
int vv_cloud_tcp_connect(void *arg, const char *host, uint16_t port,
                         uint64_t deadline, const struct vv_https_cancel *cancel)
{
  struct vv_cloud_tcp *t = arg; struct call c = {t, deadline, 0, cancel, 0};
  struct vv_dns_backend dns = {&c, resolve, dns_now, sleep_dns, dns_cancel};
  struct vv_dns_policy policy; struct vv_dns_result result;
  enum vv_dns_status status; int rc, fd; uint64_t left;
  const char *stage="socket";
  if (!t || !host || !port || !t->dns.now_ms || !t->dns.resolve_ipv4 ||
      !t->ops || !t->ops->open_socket || !t->ops->nonblocking ||
      !t->ops->connect_ipv4 || !t->ops->wait_connected ||
      !t->ops->socket_error || !t->ops->close_socket) return -EINVAL;
  if ((rc=check(&c))) return rc;
  policy=t->dns_policy; left=deadline-c.last;
  if (left < policy.deadline_ms) policy.deadline_ms=(uint32_t)left;
  status=vv_dns_resolve_ipv4(host,&policy,&dns,&result);
  if ((rc=check(&c))) return rc;
  if (status != VV_DNS_OK) {
    printf("CLOUD_TCP stage=dns status=%d attempts=%u backend_code=%d\n",
           (int)status,result.attempts,result.backend_code);
    switch (status) {
      case VV_DNS_INVALID_ARGUMENT: return -EINVAL;
      case VV_DNS_DEADLINE_EXCEEDED: return -ETIMEDOUT;
      case VV_DNS_CANCELLED: return -ECANCELED;
      case VV_DNS_TEMPORARY_FAILURE: return -EAGAIN;
      case VV_DNS_NOT_FOUND: case VV_DNS_NO_IPV4_ADDRESS: return -EHOSTUNREACH;
      default: return -EIO;
    }
  }
  fd=t->ops->open_socket(t->ops_arg);
  if (fd < 0) {printf("CLOUD_TCP stage=socket rc=%d\n",fd);return fd;}
  stage="nonblock";
  if ((rc=check(&c))) goto fail;
  if ((rc=t->ops->nonblocking(t->ops_arg,fd))) goto fail;
  if ((rc=check(&c))) goto fail;
  stage="connect";
  rc=t->ops->connect_ipv4(t->ops_arg,fd,result.ipv4_be,port);
  if (rc && rc != -EINPROGRESS && rc != -EWOULDBLOCK && rc != -EALREADY)
    goto fail;
  if (!rc) { if ((rc=check(&c))) goto fail; return fd; }
  stage="wait_connect";
  for (;;) {
    unsigned slice;
    if ((rc=check(&c))) goto fail;
    left=deadline-c.last; slice=left>50 ? 50 : (unsigned)left;
    rc=t->ops->wait_connected(t->ops_arg,fd,slice);
    if (rc == -EINTR || rc == 0) continue;
    if (rc < 0) goto fail;
    if ((rc=check(&c))) goto fail;
    stage="socket_error";
    rc=t->ops->socket_error(t->ops_arg,fd);
    if (rc) goto fail;
    if ((rc=check(&c))) goto fail;
    return fd;
  }
fail:
  printf("CLOUD_TCP stage=%s rc=%d\n",stage,rc);
  t->ops->close_socket(t->ops_arg,fd); return rc < 0 ? rc : -EIO;
}
