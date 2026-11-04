#include "vv_https_client.h"
#ifdef __NuttX__
#include <nuttx/config.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VV_IO_BUFFER 1024
#define VV_BODY_BUFFER 1024

struct reader {
  struct vv_https_client *client;
  const struct vv_https_cancel *cancel;
  uint64_t deadline;
  unsigned char data[VV_IO_BUFFER];
  size_t at;
  size_t used;
  /* Numeric framing diagnostics only; never log response headers/body. */
  unsigned phase; /* 1=status,2=headers,3=size,4=chunk,5=CRLF,6=trailers,7=fixed,8=EOF */
  size_t expected;
  bool premature_eof;
};

void vv_https_secure_zero(void *data, size_t len) {
  volatile unsigned char *p = (volatile unsigned char *)data;
  while (len--) *p++ = 0;
}

static bool cancelled(const struct vv_https_cancel *cancel) {
  return cancel && cancel->is_cancelled && cancel->is_cancelled(cancel->arg);
}

static int gate(struct vv_https_client *c,
                const struct vv_https_cancel *cancel, uint64_t deadline) {
  if (cancelled(cancel)) return VV_HTTPS_ECANCELLED;
  if (c->now_ms(c->now_arg) >= deadline) return VV_HTTPS_ETIMEOUT;
  return 0;
}

static int map_io(ssize_t rc) {
  if (rc == -ECANCELED) return VV_HTTPS_ECANCELLED;
  if (rc == -ETIMEDOUT) return VV_HTTPS_ETIMEOUT;
  return VV_HTTPS_EIO;
}

static int write_all(struct vv_https_client *c, const void *data, size_t len,
                     uint64_t deadline,
                     const struct vv_https_cancel *cancel,size_t *sent) {
  const unsigned char *p = data;
  while (len) {
    int rc = gate(c, cancel, deadline);
    if (rc) return rc;
    ssize_t n = c->transport->write(c->transport_ctx, p, len, deadline, cancel);
    if (n <= 0) return map_io(n);
    if ((size_t)n > len) return VV_HTTPS_EIO;
    if (sent) *sent += (size_t)n;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static int refill(struct reader *r) {
  int rc = gate(r->client, r->cancel, r->deadline);
  if (rc) return rc;
  ssize_t n = r->client->transport->read(r->client->transport_ctx, r->data,
                                         sizeof(r->data), r->deadline,
                                         r->cancel);
  if (n < 0) return map_io(n);
  r->at = 0;
  r->used = (size_t)n;
  if (!n && r->phase != 8) {
    r->premature_eof = true;
    printf("HTTPS_EOF phase=%u expected=%zu\n", r->phase, r->expected);
    fflush(stdout);
  }
  return n == 0 ? 1 : 0;
}

static int read_some(struct reader *r, void *dst, size_t cap, size_t *actual) {
  *actual = 0;
  if (!r->used) {
    int rc = refill(r);
    if (rc) return rc;
  }
  size_t n = r->used < cap ? r->used : cap;
  memcpy(dst, r->data + r->at, n);
  r->at += n;
  r->used -= n;
  *actual = n;
  return 0;
}

static int read_exact(struct reader *r, void *dst, size_t len) {
  unsigned char *p = dst;
  while (len) {
    size_t n;
    r->expected = len;
    int rc = read_some(r, p, len, &n);
    if (rc) return rc == 1 ? VV_HTTPS_EPROTO : rc;
    p += n;
    len -= n;
  }
  return 0;
}

/* Returns a line without CRLF. */
static int read_line(struct reader *r, char *line, size_t capacity,
                     size_t *wire_total) {
  size_t n = 0;
  bool cr = false;
  for (;;) {
    unsigned char ch;
    int rc = read_exact(r, &ch, 1);
    if (rc) return rc;
    if (++*wire_total > r->client->limits.max_response_header_bytes)
      return VV_HTTPS_ELIMIT;
    if (cr) {
      if (ch != '\n') return VV_HTTPS_EPROTO;
      line[n] = 0;
      return 0;
    }
    if (ch == '\r') {
      cr = true;
    } else {
      if (ch == '\n' || ch == 0 || n + 1 >= capacity) return VV_HTTPS_EPROTO;
      line[n++] = (char)ch;
    }
  }
}

static int ascii_casecmp(const char *a, const char *b) {
  while (*a && *b) {
    int x = tolower((unsigned char)*a++);
    int y = tolower((unsigned char)*b++);
    if (x != y) return x - y;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

static int parse_size(const char *s, size_t *out) {
  size_t v = 0;
  if (!*s) return VV_HTTPS_EPROTO;
  while (*s) {
    if (!isdigit((unsigned char)*s)) return VV_HTTPS_EPROTO;
    unsigned d = (unsigned)(*s++ - '0');
    if (v > (SIZE_MAX - d) / 10) return VV_HTTPS_ELIMIT;
    v = v * 10 + d;
  }
  *out = v;
  return 0;
}

static int valid_token(const char *s) {
  if (!s || !*s) return 0;
  for (; *s; s++) {
    unsigned char ch = (unsigned char)*s;
    if (!(isalnum(ch) || strchr("!#$%&'*+-.^_`|~", ch))) return 0;
  }
  return 1;
}

static int valid_value(const char *s) {
  if (!s) return 0;
  for (; *s; s++) {
    unsigned char ch = (unsigned char)*s;
    if ((ch < 0x20 && ch != '\t') || ch == 0x7f) return 0;
  }
  return 1;
}

static int valid_path(const char *s) {
  if (!s || s[0] != '/') return 0;
  for (; *s; s++) {
    unsigned char ch = (unsigned char)*s;
    if (ch <= 0x20 || ch == 0x7f) return 0;
  }
  return 1;
}

static int valid_host(const char *s) {
  if (!s || !*s) return 0;
  size_t n = 0;
  for (; *s; s++, n++) {
    unsigned char ch = (unsigned char)*s;
    if (!(isalnum(ch) || ch == '.' || ch == '-')) return 0;
  }
  return n <= 253;
}

static int append(char *buf, size_t cap, size_t *used, const char *text) {
  size_t n = strlen(text);
  if (n > cap - *used) return VV_HTTPS_ELIMIT;
  memcpy(buf + *used, text, n);
  *used += n;
  return 0;
}

static int make_headers(const struct vv_https_request *q, char *buf,
                        size_t cap, size_t *used) {
  char number[32];
  int rc;
  *used = 0;
  if (!valid_token(q->method) || !valid_host(q->host) || !valid_path(q->path))
    return VV_HTTPS_EINVAL;
  if ((rc = append(buf, cap, used, q->method)) ||
      (rc = append(buf, cap, used, " ")) ||
      (rc = append(buf, cap, used, q->path)) ||
      (rc = append(buf, cap, used, " HTTP/1.1\r\nHost: ")) ||
      (rc = append(buf, cap, used, q->host))) return rc;
  if (q->port && q->port != 443) {
    snprintf(number, sizeof(number), ":%u", (unsigned)q->port);
    if ((rc = append(buf, cap, used, number))) return rc;
  }
  if ((rc = append(buf, cap, used, "\r\nConnection: close\r\n")))
    return rc;
  for (size_t i = 0; i < q->header_count; i++) {
    const struct vv_https_header *h = &q->headers[i];
    if (!valid_token(h->name) || !valid_value(h->value))
      return VV_HTTPS_EINVAL;
    if (!ascii_casecmp(h->name, "Host") ||
        !ascii_casecmp(h->name, "Content-Length") ||
        !ascii_casecmp(h->name, "Transfer-Encoding") ||
        !ascii_casecmp(h->name, "Connection")) return VV_HTTPS_EINVAL;
    if ((rc = append(buf, cap, used, h->name)) ||
        (rc = append(buf, cap, used, ": ")) ||
        (rc = append(buf, cap, used, h->value)) ||
        (rc = append(buf, cap, used, "\r\n"))) return rc;
  }
  snprintf(number, sizeof(number), "%zu", q->body_length);
  if ((rc = append(buf, cap, used, "Content-Length: ")) ||
      (rc = append(buf, cap, used, number)) ||
      (rc = append(buf, cap, used, "\r\n\r\n"))) return rc;
  return 0;
}

static int deliver(struct vv_https_client *c, vv_https_sink_fn sink,
                   void *sink_arg, const void *data, size_t len,
                   struct vv_https_response *response) {
  if (len > c->limits.max_response_body_bytes - response->body_bytes)
    return VV_HTTPS_ELIMIT;
  if (len && sink && sink(sink_arg, data, len)) return VV_HTTPS_ESINK;
  response->body_bytes += len;
  return 0;
}

static int fixed_body(struct reader *r, size_t length, vv_https_sink_fn sink,
                      void *sink_arg, struct vv_https_response *response) {
  unsigned char buf[VV_BODY_BUFFER];
  while (length) {
    size_t cap = length < sizeof(buf) ? length : sizeof(buf), n;
    /* Payload can be consumed incrementally. Waiting for a full local buffer
     * would hold an already complete SSE event behind a later network read.
     * Keep the exact remaining length and validate framing separately. */
    r->expected = cap;
    int rc = read_some(r, buf, cap, &n);
    if (rc) return rc == 1 ? VV_HTTPS_EPROTO : rc;
    rc = deliver(r->client, sink, sink_arg, buf, n, response);
    if (rc) return rc;
    length -= n;
  }
  return 0;
}

static int chunked_body(struct reader *r, vv_https_sink_fn sink,
                        void *sink_arg, struct vv_https_response *response,
                        size_t *header_wire) {
  char line[256];
  for (;;) {
    r->phase = 3;
    int rc = read_line(r, line, sizeof(line), header_wire);
    if (rc) return rc;
    char *ext = strchr(line, ';');
    if (ext) *ext = 0;
    if (!*line) return VV_HTTPS_EPROTO;
    size_t n = 0;
    for (const char *p = line; *p; p++) {
      unsigned d;
      if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
      else if (*p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a' + 10);
      else if (*p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A' + 10);
      else return VV_HTTPS_EPROTO;
      if (n > (SIZE_MAX - d) / 16) return VV_HTTPS_ELIMIT;
      n = n * 16 + d;
    }
    if (n > r->client->limits.max_chunk_bytes) return VV_HTTPS_ELIMIT;
    if (!n) {
      r->phase = 6;
      do {
        rc = read_line(r, line, sizeof(line), header_wire);
        if (rc) return rc;
      } while (*line);
      return 0;
    }
    r->phase = 4;
    rc = fixed_body(r, n, sink, sink_arg, response);
    if (rc) return rc;
    unsigned char crlf[2];
    r->phase = 5;
    rc = read_exact(r, crlf, 2);
    if (rc) return rc; /* Preserve timeout/cancel/I/O instead of calling it syntax. */
    if (crlf[0] != '\r' || crlf[1] != '\n') return VV_HTTPS_EPROTO;
  }
}

static int eof_body(struct reader *r, vv_https_sink_fn sink, void *sink_arg,
                    struct vv_https_response *response) {
  unsigned char buf[VV_BODY_BUFFER];
  for (;;) {
    size_t n;
    int rc = read_some(r, buf, sizeof(buf), &n);
    if (rc == 1) return 0;
    if (rc) return rc;
    rc = deliver(r->client, sink, sink_arg, buf, n, response);
    if (rc) return rc;
  }
}

static int receive_response(struct reader *r, vv_https_sink_fn sink,
                            void *sink_arg,
                            struct vv_https_response *response) {
  char line[1024];
  size_t wire = 0;
  r->phase = 1;
  int rc = read_line(r, line, sizeof(line), &wire);
  if (rc) return rc;
  int status = 0;
  char minor = 0;
  if (sscanf(line, "HTTP/1.%c %d", &minor, &status) != 2 ||
      (minor != '0' && minor != '1') || status < 100 || status > 599)
    return VV_HTTPS_EPROTO;
  response->status_code = status;
  bool have_length = false, chunked = false;
  size_t length = 0;
  r->phase = 2;
  for (;;) {
    rc = read_line(r, line, sizeof(line), &wire);
    if (rc || !*line) break;
    char *colon = strchr(line, ':');
    if (!colon || colon == line) return VV_HTTPS_EPROTO;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t') colon++;
    if (!ascii_casecmp(line, "Content-Length")) {
      size_t parsed;
      rc = parse_size(colon, &parsed);
      if (rc || (have_length && parsed != length)) return VV_HTTPS_EPROTO;
      have_length = true;
      length = parsed;
    } else if (!ascii_casecmp(line, "Content-Type")) {
      char *end = colon + strlen(colon);
      while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      size_t size = (size_t)(end - colon);
      if (!size) return VV_HTTPS_EPROTO;
      if (size >= sizeof(response->content_type)) return VV_HTTPS_ELIMIT;
      for (const unsigned char *p = (const unsigned char *)colon; *p; ++p)
        if ((*p < 32 && *p != '\t') || *p == 127) return VV_HTTPS_EPROTO;
      /* Conservative duplicate handling: parameter values can be case
       * sensitive, so only byte-identical trimmed values are accepted. */
      if (response->content_type[0] && strcmp(response->content_type, colon))
        return VV_HTTPS_EPROTO;
      memcpy(response->content_type, colon, size + 1);
    } else if (!ascii_casecmp(line, "Transfer-Encoding")) {
      while (*colon == ' ' || *colon == '\t') colon++;
      char *end = colon + strlen(colon);
      while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      if (ascii_casecmp(colon, "chunked")) return VV_HTTPS_EPROTO;
      chunked = true;
    }
  }
  if (rc) return rc;
  if (chunked && have_length) return VV_HTTPS_EPROTO;
  response->chunked = chunked;
  if ((status >= 100 && status < 200) || status == 204 || status == 304)
    return (chunked || (have_length && length)) ? VV_HTTPS_EPROTO : 0;
  if (chunked) return chunked_body(r, sink, sink_arg, response, &wire);
  if (have_length) {
    if (length > r->client->limits.max_response_body_bytes)
      return VV_HTTPS_ELIMIT;
    r->phase = 7;
    return fixed_body(r, length, sink, sink_arg, response);
  }
  r->phase = 8;
  return eof_body(r, sink, sink_arg, response);
}

static int perform(struct vv_https_client *c,
                     const struct vv_https_request *q,
                     vv_https_sink_fn sink, void *sink_arg,
                     const struct vv_https_cancel *cancel,
                     struct vv_https_response *response, bool development_http) {
  int rc = VV_HTTPS_EINVAL;
  int stage = 1;
  size_t body_sent = 0;
  char *headers = NULL;
  unsigned char body[VV_BODY_BUFFER];
  bool connected = false;
  if (response) response->premature_eof = false;
  if (!c || !q || !response || !c->transport || !c->now_ms ||
      !c->transport->connect || !c->transport->write ||
      !c->transport->read || !c->transport->close ||
      !c->limits.max_request_header_bytes ||
      !c->limits.max_response_header_bytes ||
      !c->limits.max_response_body_bytes || !c->limits.max_chunk_bytes ||
      !c->limits.total_timeout_ms || (!q->body_read && q->body_length))
    return rc;
  memset(response, 0, sizeof(*response));
  uint64_t start = c->now_ms(c->now_arg);
  uint64_t deadline = start + c->limits.total_timeout_ms;
  if (deadline < start) deadline = UINT64_MAX;
  headers = malloc(c->limits.max_request_header_bytes);
  if (!headers) {
    printf("HTTPS_IO stage=1 rc=%d http=0\n", VV_HTTPS_EIO); fflush(stdout);
    return VV_HTTPS_EIO;
  }
  stage = 2;
  size_t header_len;
  rc = make_headers(q, headers, c->limits.max_request_header_bytes,
                    &header_len);
  if (rc) goto done;
  struct vv_https_peer_security sec = {0};
  rc = gate(c, cancel, deadline);
  if (rc) goto done;
  stage = 3;
  rc = c->transport->connect(c->transport_ctx, q->host,
                             q->port ? q->port : 443, deadline,
                             cancel, &sec);
  if (rc) {
    rc = rc == -ECANCELED ? VV_HTTPS_ECANCELLED :
         rc == -ETIMEDOUT ? VV_HTTPS_ETIMEOUT :
         rc == VV_HTTPS_ETLS_VERIFY ? VV_HTTPS_ETLS_VERIFY : VV_HTTPS_EIO;
    goto done;
  }
  connected = true;
  if (!development_http && (!sec.sni_set || !sec.peer_verified || sec.verify_flags))
    { rc = VV_HTTPS_ETLS_VERIFY; goto done; }
  if (c->log) c->log(c->log_arg, development_http ? "development_http" : "tls_verified", 1);
  stage = 4;
  rc = write_all(c, headers, header_len, deadline, cancel,NULL);
  if (rc) goto done;
  stage = 5;
  for (size_t offset = 0; offset < q->body_length;) {
    size_t want = q->body_length - offset;
    if (want > sizeof(body)) want = sizeof(body);
    ssize_t n = q->body_read(q->body_arg, offset, body, want);
    if (n <= 0 || (size_t)n > want) { rc = VV_HTTPS_EBODY; goto done; }
    rc = write_all(c, body, (size_t)n, deadline, cancel,&body_sent);
    vv_https_secure_zero(body, sizeof(body));
    if (rc) goto done;
    offset += (size_t)n;
  }
  {
    stage = 6;
    struct reader reader = {.client = c, .cancel = cancel,
                            .deadline = deadline};
    rc = receive_response(&reader, sink, sink_arg, response);
    response->premature_eof = reader.premature_eof;
  }
done:
  if (rc && stage == 5) { printf("HTTPS_BODY sent=%zu total=%zu\n", body_sent,q->body_length); fflush(stdout); }
  if (rc) { printf("HTTPS_IO stage=%d rc=%d http=%d\n", stage, rc, response->status_code); fflush(stdout); }
  vv_https_secure_zero(body, sizeof(body));
  if (headers) {
    vv_https_secure_zero(headers, c->limits.max_request_header_bytes);
    free(headers);
  }
  if (connected) c->transport->close(c->transport_ctx);
  if (c->log) c->log(c->log_arg, "request_done", rc ? rc : response->status_code);
  return rc;
}

const char *vv_https_strerror(int error) {
  switch (error) {
    case 0: return "ok";
    case VV_HTTPS_EINVAL: return "invalid argument";
    case VV_HTTPS_ECANCELLED: return "cancelled";
    case VV_HTTPS_ETIMEOUT: return "timeout";
    case VV_HTTPS_ETLS_VERIFY: return "TLS verification failed";
    case VV_HTTPS_EIO: return "transport I/O failed";
    case VV_HTTPS_EPROTO: return "HTTP protocol error";
    case VV_HTTPS_ELIMIT: return "configured limit exceeded";
    case VV_HTTPS_ESINK: return "response sink rejected data";
    case VV_HTTPS_EBODY: return "request body source failed";
    default: return "unknown error";
  }
}

int vv_https_perform(struct vv_https_client *c,const struct vv_https_request *q,
 vv_https_sink_fn sink,void *arg,const struct vv_https_cancel *cancel,
 struct vv_https_response *response) {
 return perform(c,q,sink,arg,cancel,response,false);
}
int vv_http_development_perform(struct vv_https_client *c,const struct vv_https_request *q,
 vv_https_sink_fn sink,void *arg,const struct vv_https_cancel *cancel,
 struct vv_https_response *response) {
 if(response)memset(response,0,sizeof(*response));
#ifdef CONFIG_EXAMPLES_K7CLOUD_DEV_LAN_HTTP
 if(!q||!q->host||strcmp(q->host,"10.3.3.170")||q->port!=18085)
   return VV_HTTPS_EINVAL;
 return perform(c,q,sink,arg,cancel,response,true);
#else
 (void)c;(void)q;(void)sink;(void)arg;(void)cancel;
 return VV_HTTPS_EINVAL;
#endif
}
