#include "vv_https_mbedtls.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#ifdef __NuttX__
#include <sys/ioctl.h>
#include <nuttx/fs/ioctl.h>
#include <unistd.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/sha1.h>
#include <mbedtls/x509_crt.h>

struct vv_https_mbedtls {
  struct vv_https_mbedtls_config cfg;
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config ssl_cfg;
  mbedtls_x509_crt ca;
  mbedtls_ctr_drbg_context drbg;
  int last_error;
  bool initialized;
  bool connected;
  const void *pending_write;
  size_t pending_length;
  /* Single TLS owner; numeric connection-local observations only. */
  struct {
    uint64_t calls, bytes, want_read, zero_returns, zero_length_calls;
    size_t last_length;
    int last_rc, last_errno;
  } rx;
};

static bool is_cancelled(const struct vv_https_cancel *c) {
  return c && c->is_cancelled && c->is_cancelled(c->arg);
}

/* errno may be stale for mbedTLS errors; raw rc is the primary evidence. */
static void tls_diag(int stage, int rc, int os_error, int detail) {
  int saved = errno;
  printf("TLS_IO stage=%d rc=%d os_errno=%d detail=%d\n",
         stage, rc, os_error, detail);
  fflush(stdout);
  errno = saved;
}

/* Keep both BIO callbacks on the same context, forwarding the original net
 * context and arguments exactly. No retries, socket queries or logging here. */
static int bio_send(void *ctx, const unsigned char *data, size_t len) {
  struct vv_https_mbedtls *t = ctx;
  return mbedtls_net_send(&t->net, data, len);
}

static int bio_recv(void *ctx, unsigned char *data, size_t len) {
  struct vv_https_mbedtls *t = ctx;
  int rc = mbedtls_net_recv(&t->net, data, len);
  int saved = errno;
  ++t->rx.calls;
  t->rx.last_length = len;
  t->rx.last_rc = rc;
  t->rx.last_errno = saved;
  if (rc > 0) t->rx.bytes += (unsigned)rc;
  if (rc == MBEDTLS_ERR_SSL_WANT_READ) ++t->rx.want_read;
  if (!rc) ++t->rx.zero_returns;
  if (!len) ++t->rx.zero_length_calls;
  errno = saved;
  return rc;
}

/* A TLS zero is not by itself proof that net_recv returned zero: an empty
 * application record can also produce it. Report before reset destroys that
 * distinction. errno is observational and can be stale even on success. */
static void bio_read_diag(const struct vv_https_mbedtls *t, int ssl_rc,
                          size_t len, uint64_t calls_before) {
  int saved = errno;
  printf("TLS_BIO ssl_rc=%d outer_len=%zu call_delta=%llu recv_calls=%llu "
         "recv_bytes=%llu last_len=%zu last_rc=%d last_errno=%d "
         "want_read=%llu zero_returns=%llu zero_len_calls=%llu\n",
         ssl_rc, len, (unsigned long long)(t->rx.calls - calls_before),
         (unsigned long long)t->rx.calls, (unsigned long long)t->rx.bytes,
         t->rx.last_length, t->rx.last_rc, t->rx.last_errno,
         (unsigned long long)t->rx.want_read,
         (unsigned long long)t->rx.zero_returns,
         (unsigned long long)t->rx.zero_length_calls);
  fflush(stdout);
  errno = saved;
}

#ifdef __NuttX__
/* Parse only bounded numeric fields from this SDK's FIOC_FILEPATH suffix.
 * Never log the path: its prefix contains local/remote addresses. */
static bool diag_number(const char **cursor, unsigned base, unsigned limit,
                        unsigned *value) {
  const char *p = *cursor;
  unsigned n = 0, digits = 0;
  for (;;) {
    unsigned d;
    if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
    else if (base == 16 && *p >= 'a' && *p <= 'f')
      d = (unsigned)(*p - 'a') + 10;
    else if (base == 16 && *p >= 'A' && *p <= 'F')
      d = (unsigned)(*p - 'A') + 10;
    else break;
    if (d >= base || n > (limit - d) / base) return false;
    n = n * base + d;
    ++digits;
    ++p;
  }
  if (!digits) return false;
  *cursor = p;
  *value = n;
  return true;
}

static int tcp_path_numbers(const char *path, size_t size,
                            int *state, int *flags, int *rx) {
  const char *end = memchr(path, 0, size);
  if (!end || (size_t)(end - path) < 5 || memcmp(path, "tcp:[", 5))
    return -EPROTO;
  for (const char *p = path; p != end; ++p)
    if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return -EPROTO;
  const char *mark = strstr(path, ", st ");
  if (!mark) return -EPROTO;
  const char *p = mark + 5;
  unsigned st, flg, count = 0, ignored;
  if (!diag_number(&p, 16, 255, &st) || strncmp(p, ", flg ", 6))
    return -EPROTO;
  p += 6;
  if (!diag_number(&p, 16, 255, &flg) || strcmp(p, "]")) return -EPROTO;
  const char *received = strstr(path, ", rx ");
  if (received) {
    if (received >= mark) return -EPROTO;
    p = received + 5;
    if (!diag_number(&p, 10, INT_MAX, &count) || *p++ != '/' ||
        !diag_number(&p, 10, INT_MAX, &ignored)) return -EPROTO;
    if (!strncmp(p, " + ofo ", 7)) {
      p += 7;
      if (!diag_number(&p, 10, INT_MAX, &ignored)) return -EPROTO;
    }
    if (p != mark) return -EPROTO;
  }
  *state = (int)st;
  *flags = (int)flg;
  *rx = received ? (int)count : -1;
  return 0;
}
#endif

/* EOF only, before reset loses the connection. These are observations, not
 * recovery decisions. In this SDK both peer FIN and TCP_ABORT can surface as
 * read()==0. SO_ERROR alone cannot distinguish them. No normal-path queries. */
static void socket_eof_diag(const struct vv_https_mbedtls *t) {
  int saved = errno;
  int path_rc = -ENOTSUP, read_rc = -ENOTSUP, state = -1, flags = -1;
  int path_rx = -1, available = -1, socket_error = -1, socket_rc;
#ifdef __NuttX__
  /* The ioctl contract writes up to PATH_MAX, so a smaller buffer is unsafe. */
  char path[PATH_MAX];
  memset(path, 0xff, sizeof(path));
  errno = 0;
  path_rc = ioctl(t->net.fd, FIOC_FILEPATH, (uintptr_t)path);
  if (path_rc < 0) path_rc = errno ? -errno : -EIO;
  else if (path_rc) path_rc = -EIO;
  else path_rc = tcp_path_numbers(path, sizeof(path), &state, &flags, &path_rx);
  vv_https_secure_zero(path, sizeof(path));
  errno = 0;
  read_rc = ioctl(t->net.fd, FIONREAD, (uintptr_t)&available);
  if (read_rc < 0) read_rc = errno ? -errno : -EIO;
  else if (read_rc || available < 0) read_rc = -EIO;
  if (read_rc) available = -1;
#endif
  socklen_t length = sizeof(socket_error);
  errno = 0;
  socket_rc = getsockopt(t->net.fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
  if (socket_rc < 0) socket_rc = errno ? -errno : -EIO;
  else if (socket_rc || length != sizeof(socket_error) || socket_error < 0)
    socket_rc = -EIO;
  if (socket_rc) socket_error = -1;
  printf("TLS_EOF_SOCKET path_rc=%d state=%d flags=%d path_rx=%d "
         "read_rc=%d available=%d socket_rc=%d so_error=%d\n",
         path_rc, state, flags, path_rx, read_rc, available,
         socket_rc, socket_error);
  fflush(stdout);
  errno = saved;
}

static int wait_fd(struct vv_https_mbedtls *t, short events,
                   uint64_t deadline, const struct vv_https_cancel *cancel) {
  for (;;) {
    if (is_cancelled(cancel)) return -ECANCELED;
    uint64_t now = t->cfg.now_ms(t->cfg.now_arg);
    if (now >= deadline) return -ETIMEDOUT;
    uint64_t remain = deadline - now;
    int slice = remain > 50 ? 50 : (int)remain;
    struct pollfd pfd = {.fd = t->net.fd, .events = events};
    int rc = poll(&pfd, 1, slice);
    if (rc > 0) {
      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        /* Read SO_ERROR only on the terminal path; reading may consume it.
         * A zero socket error does not exclude poll-setup resource failure. */
        int saved_errno = errno, socket_error = -1;
        socklen_t size = sizeof(socket_error);
        int socket_rc = getsockopt(t->net.fd, SOL_SOCKET, SO_ERROR,
                                   &socket_error, &size);
        int socket_errno = socket_rc < 0 ? errno : 0;
        printf("TLS_POLL rc=%d wanted=%d revents=%d\n",
               rc, (int)events, (int)pfd.revents);
        printf("TLS_SOCKET rc=%d os_errno=%d so_error=%d\n",
               socket_rc, socket_errno, socket_error);
        fflush(stdout);
        errno = saved_errno;
        tls_diag(20, -EIO, 0, pfd.revents); return -EIO;
      }
      if (pfd.revents & events) return 0;
    } else if (rc < 0 && errno != EINTR) {
      tls_diag(21, rc, errno, events);
      return -errno;
    }
  }
}

static int wait_mbed(struct vv_https_mbedtls *t, int rc, uint64_t deadline,
                     const struct vv_https_cancel *cancel) {
  if (rc == MBEDTLS_ERR_SSL_WANT_READ)
    return wait_fd(t, POLLIN, deadline, cancel);
  if (rc == MBEDTLS_ERR_SSL_WANT_WRITE)
    return wait_fd(t, POLLOUT, deadline, cancel);
  return rc;
}

static void reset_connection(struct vv_https_mbedtls *t) {
  t->connected = false;
  t->pending_write = NULL;
  t->pending_length = 0;
  if (t->net.fd >= 0) {
#ifdef __NuttX__
    /* This SDK's shutdown callback disables the buffered-send callback before
     * tcp_close can drain/release it. mbedtls_net_free calls shutdown(fd, 2)
     * first (SHUT_WR on NuttX). Let tcp_close own FIN and buffer cleanup. */
    int fd = t->net.fd;
    t->net.fd = -1;
    (void)close(fd);
#else
    mbedtls_net_free(&t->net);
#endif
  }
  mbedtls_ssl_free(&t->ssl);
  mbedtls_ssl_init(&t->ssl);
}

static int tls_connect(void *ctx, const char *host, uint16_t port,
                       uint64_t deadline,
                       const struct vv_https_cancel *cancel,
                       struct vv_https_peer_security *security) {
  struct vv_https_mbedtls *t = ctx;
  int rc;
  int handshake_error = 0;
  int stage = 1;
  memset(security, 0, sizeof(*security));
  reset_connection(t);
  memset(&t->rx, 0, sizeof(t->rx));
  if (!t->cfg.security_ready(t->cfg.security_arg))
    return VV_HTTPS_ETLS_VERIFY;
  rc = t->cfg.tcp_connect(t->cfg.tcp_arg, host, port, deadline, cancel);
  if (rc < 0) { tls_diag(stage, rc, errno, 0); return rc; }
  t->net.fd = rc;
  stage = 2;
  rc = mbedtls_ssl_setup(&t->ssl, &t->ssl_cfg);
  if (rc) goto fail;
  stage = 3;
  rc = mbedtls_ssl_set_hostname(&t->ssl, host);
  if (rc) goto fail;
  security->sni_set = true;
  mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
  stage = 4;
  for (;;) {
    if (is_cancelled(cancel)) { rc = -ECANCELED; goto fail; }
    if (t->cfg.now_ms(t->cfg.now_arg) >= deadline)
      { rc = -ETIMEDOUT; goto fail; }
    rc = mbedtls_ssl_handshake(&t->ssl);
    handshake_error = rc;
    if (!rc) break;
    int os_error = errno;
    int waited = wait_mbed(t, rc, deadline, cancel);
    if (waited < 0 && waited != MBEDTLS_ERR_SSL_WANT_READ &&
        waited != MBEDTLS_ERR_SSL_WANT_WRITE) {
      tls_diag(stage, rc, os_error, waited);
      rc = waited;
      goto fail;
    }
  }
  stage = 5;
  security->verify_flags = mbedtls_ssl_get_verify_result(&t->ssl);
  security->peer_verified = security->verify_flags == 0 &&
                            mbedtls_ssl_get_peer_cert(&t->ssl) != NULL;
  if (!security->peer_verified) { rc = VV_HTTPS_ETLS_VERIFY; goto fail; }
  if (!t->cfg.security_ready(t->cfg.security_arg))
    { rc = VV_HTTPS_ETLS_VERIFY; goto fail; }
  t->connected = true;
  t->last_error = 0;
  return 0;
fail:
  tls_diag(stage, rc, errno, (int)security->verify_flags);
  t->last_error = rc;
  reset_connection(t);
  /* Preserve the native diagnostic above. Only the SDK's definite socket
   * send failure during handshake is a retryable transport loss. Do not
   * classify certificate, entropy, time, allocation or other TLS failures
   * by errno (which may be stale), nor replay an established request. */
  if (stage == 4 && handshake_error == MBEDTLS_ERR_NET_SEND_FAILED &&
      rc == MBEDTLS_ERR_NET_SEND_FAILED)
    return -ECONNRESET;
  return rc;
}

static ssize_t tls_write(void *ctx, const void *data, size_t len,
                         uint64_t deadline,
                         const struct vv_https_cancel *cancel) {
  struct vv_https_mbedtls *t = ctx;
  if (!t->connected) return -ENOTCONN;
  if (t->pending_write) return -EBUSY;
  for (;;) {
    if (!t->cfg.security_ready(t->cfg.security_arg))
      { reset_connection(t); return VV_HTTPS_ETLS_VERIFY; }
    if (is_cancelled(cancel)) { reset_connection(t); return -ECANCELED; }
    if (t->cfg.now_ms(t->cfg.now_arg) >= deadline)
      { reset_connection(t); return -ETIMEDOUT; }
    int rc = mbedtls_ssl_write(&t->ssl, data, len);
    int os_error = errno;
    if (rc >= 0) { if (!rc) { tls_diag(6, rc, os_error, 0); reset_connection(t); } return rc; }
    int waited = wait_mbed(t, rc, deadline, cancel);
    if (waited < 0 && waited != MBEDTLS_ERR_SSL_WANT_READ &&
        waited != MBEDTLS_ERR_SSL_WANT_WRITE) {
      tls_diag(6, rc, os_error, waited);
      t->last_error = rc;
      reset_connection(t);
      return waited == rc ? -EIO : waited;
    }
  }
}

static ssize_t tls_read(void *ctx, void *data, size_t len,
                        uint64_t deadline,
                        const struct vv_https_cancel *cancel) {
  struct vv_https_mbedtls *t = ctx;
  if (!t->connected) return -ENOTCONN;
  if (t->pending_write) return -EBUSY;
  for (;;) {
    if (!t->cfg.security_ready(t->cfg.security_arg))
      { reset_connection(t); return VV_HTTPS_ETLS_VERIFY; }
    if (is_cancelled(cancel)) { reset_connection(t); return -ECANCELED; }
    if (t->cfg.now_ms(t->cfg.now_arg) >= deadline)
      { reset_connection(t); return -ETIMEDOUT; }
    uint64_t calls_before = t->rx.calls;
    int rc = mbedtls_ssl_read(&t->ssl, data, len);
    int os_error = errno;
    if (rc >= 0) { if (!rc) { bio_read_diag(t, rc, len, calls_before); socket_eof_diag(t); tls_diag(7, rc, os_error, 0); reset_connection(t); } return rc; }
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { bio_read_diag(t, rc, len, calls_before); tls_diag(7, rc, os_error, 0); reset_connection(t); return 0; }
    int waited = wait_mbed(t, rc, deadline, cancel);
    if (waited < 0 && waited != MBEDTLS_ERR_SSL_WANT_READ &&
        waited != MBEDTLS_ERR_SSL_WANT_WRITE) {
      bio_read_diag(t, rc, len, calls_before);
      tls_diag(7, rc, os_error, waited);
      t->last_error = rc;
      reset_connection(t);
      return waited == rc ? -EIO : waited;
    }
  }
}

static void tls_close(void *ctx) {
  struct vv_https_mbedtls *t = ctx;
  if (t->net.fd >= 0 && !t->pending_write)
    (void)mbedtls_ssl_close_notify(&t->ssl);
  reset_connection(t);
}

static const struct vv_https_transport_ops g_ops = {
  .connect = tls_connect, .write = tls_write, .read = tls_read,
  .close = tls_close
};

struct vv_https_mbedtls *
vv_https_mbedtls_create(const struct vv_https_mbedtls_config *cfg) {
  static const unsigned char personalization[] = "velavision-https-client";
  if (!cfg || !cfg->ca_pem || !cfg->ca_pem_length || !cfg->tcp_connect ||
      !cfg->security_ready || !cfg->now_ms || !cfg->entropy_read) {
    printf("TLS_CREATE stage=1 rc=%d\n", -EINVAL); fflush(stdout);
    return NULL;
  }
  printf("TLS_CREATE stage=1 rc=0\n"); fflush(stdout);
  /* Do not even seed the DRBG until platform trust prerequisites pass. */
  if (!cfg->security_ready(cfg->security_arg)) {
    printf("TLS_CREATE stage=2 rc=%d\n", VV_HTTPS_ETLS_VERIFY); fflush(stdout);
    return NULL;
  }
  printf("TLS_CREATE stage=2 rc=0\n"); fflush(stdout);
  struct vv_https_mbedtls *t = calloc(1, sizeof(*t));
  printf("TLS_CREATE stage=3 rc=%d\n", t ? 0 : -ENOMEM); fflush(stdout);
  if (!t) return NULL;
  t->cfg = *cfg;
  mbedtls_net_init(&t->net);
  mbedtls_ssl_init(&t->ssl);
  mbedtls_ssl_config_init(&t->ssl_cfg);
  mbedtls_x509_crt_init(&t->ca);
  mbedtls_ctr_drbg_init(&t->drbg);
  int rc = mbedtls_ctr_drbg_seed(&t->drbg, cfg->entropy_read, cfg->entropy_arg,
                                 personalization,
                                 sizeof(personalization) - 1);
  printf("TLS_CREATE stage=4 rc=%d\n", rc); fflush(stdout);
  if (!rc) {
    rc = mbedtls_x509_crt_parse(&t->ca, cfg->ca_pem, cfg->ca_pem_length);
    printf("TLS_CREATE stage=5 rc=%d\n", rc); fflush(stdout);
  }
  if (!rc) {
    rc = mbedtls_ssl_config_defaults(&t->ssl_cfg,
      MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
      MBEDTLS_SSL_PRESET_DEFAULT);
    printf("TLS_CREATE stage=6 rc=%d\n", rc); fflush(stdout);
  }
  if (rc) { t->last_error = rc; vv_https_mbedtls_destroy(t); return NULL; }
  mbedtls_ssl_conf_authmode(&t->ssl_cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&t->ssl_cfg, &t->ca, NULL);
  mbedtls_ssl_conf_rng(&t->ssl_cfg, mbedtls_ctr_drbg_random, &t->drbg);
  t->initialized = true;
  printf("TLS_CREATE stage=7 rc=0\n"); fflush(stdout);
  return t;
}

void vv_https_mbedtls_destroy(struct vv_https_mbedtls *t) {
  if (!t) return;
  reset_connection(t);
  mbedtls_ssl_config_free(&t->ssl_cfg);
  mbedtls_x509_crt_free(&t->ca);
  mbedtls_ctr_drbg_free(&t->drbg);
  vv_https_secure_zero(t, sizeof(*t));
  free(t);
}

const struct vv_https_transport_ops *vv_https_mbedtls_ops(void) {
  return &g_ops;
}

int vv_https_mbedtls_last_error(const struct vv_https_mbedtls *t) {
  return t ? t->last_error : 0;
}

static int try_gate(struct vv_https_mbedtls *t, short *wanted) {
  if (!wanted) return -EINVAL;
  *wanted = 0;
  if (!t || !t->initialized || !t->connected || t->net.fd < 0)
    return -ENOTCONN;
  if (!t->cfg.security_ready(t->cfg.security_arg)) {
    reset_connection(t);
    return VV_HTTPS_ETLS_VERIFY;
  }
  return 0;
}
static ssize_t try_result(struct vv_https_mbedtls *t, int rc, short *wanted) {
  if (rc == MBEDTLS_ERR_SSL_WANT_READ) { *wanted = POLLIN; return -EAGAIN; }
  if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) { *wanted = POLLOUT; return -EAGAIN; }
  if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
    reset_connection(t);
    return 0;
  }
  if (rc < 0) { t->last_error = rc; reset_connection(t); return -EIO; }
  return rc;
}
ssize_t vv_https_mbedtls_try_read(void *ctx, void *data, size_t len,
                                short *wanted) {
  struct vv_https_mbedtls *t = ctx;
  int rc = try_gate(t, wanted);
  if (rc) return rc;
  if (!data || !len) return -EINVAL;
  if (t->pending_write) return -EBUSY;
  uint64_t calls_before = t->rx.calls;
  rc = mbedtls_ssl_read(&t->ssl, data, len);
  if (rc <= 0 && rc != MBEDTLS_ERR_SSL_WANT_READ &&
      rc != MBEDTLS_ERR_SSL_WANT_WRITE)
    bio_read_diag(t, rc, len, calls_before);
  if (!rc) socket_eof_diag(t);
  return try_result(t, rc, wanted);
}
ssize_t vv_https_mbedtls_try_write(void *ctx, const void *data, size_t len,
                                 short *wanted) {
  struct vv_https_mbedtls *t = ctx;
  int rc = try_gate(t, wanted);
  if (rc) return rc;
  if (!data || !len) return -EINVAL;
  if (t->pending_write && (t->pending_write != data || t->pending_length != len))
    return -EBUSY;
  rc = mbedtls_ssl_write(&t->ssl, data, len);
  if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
    t->pending_write = data;
    t->pending_length = len;
  } else {
    t->pending_write = NULL;
    t->pending_length = 0;
  }
  return try_result(t, rc, wanted);
}
int vv_https_mbedtls_fd(const struct vv_https_mbedtls *t) {
  return t && t->connected ? t->net.fd : -1;
}
int vv_https_mbedtls_random(void *ctx, uint8_t *data, size_t len) {
  struct vv_https_mbedtls *t = ctx;
  if (!t || !t->initialized || !data || !len) return -EINVAL;
  if (!t->cfg.security_ready(t->cfg.security_arg)) return VV_HTTPS_ETLS_VERIFY;
  if (len > MBEDTLS_CTR_DRBG_MAX_REQUEST) return -EMSGSIZE;
  int rc = mbedtls_ctr_drbg_random(&t->drbg, data, len);
  if (rc) { vv_https_secure_zero(data, len); t->last_error = rc; return -EIO; }
  return 0;
}
int vv_https_mbedtls_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
  if (!data || !digest) return -EINVAL;
  return mbedtls_sha1(data, len, digest);
}
