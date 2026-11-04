#include "mimo_cloud_client.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct bytes_reader {
  const unsigned char *data;
  size_t length;
};

struct response_buffer {
  unsigned char *data;
  size_t capacity;
  size_t length;
};

static void event(struct mimo_cloud_client *c, const char *name, int value) {
  if (c->log) c->log(c->log_arg, name, value);
}

static int plain_value_valid(const char *s, size_t max, bool allow_empty) {
  size_t n = 0;
  if (!s || (!allow_empty && !*s)) return 0;
  for (; *s; ++s, ++n) {
    unsigned char ch = (unsigned char)*s;
    if (ch < 0x20 || ch == 0x7f || n >= max) return 0;
  }
  return n <= max;
}

static int json_key_valid(const char *s) {
  if (!s || !*s) return 0;
  for (; *s; ++s) {
    unsigned char ch = (unsigned char)*s;
    if (!(isalnum(ch) || ch == '_' || ch == '-')) return 0;
  }
  return 1;
}

static size_t escaped_size(const char *s) {
  size_t n = 0;
  for (; *s; ++s) {
    unsigned char ch = (unsigned char)*s;
    size_t add = (ch == '"' || ch == '\\') ? 2 : (ch < 0x20 ? 6 : 1);
    if (n > SIZE_MAX - add) return SIZE_MAX;
    n += add;
  }
  return n;
}

static char *put_json_string(char *p, const char *s) {
  static const char hex[] = "0123456789abcdef";
  *p++ = '"';
  for (; *s; ++s) {
    unsigned char ch = (unsigned char)*s;
    if (ch == '"' || ch == '\\') {
      *p++ = '\\'; *p++ = (char)ch;
    } else if (ch < 0x20) {
      *p++ = '\\'; *p++ = 'u'; *p++ = '0'; *p++ = '0';
      *p++ = hex[ch >> 4]; *p++ = hex[ch & 15];
    } else {
      *p++ = (char)ch;
    }
  }
  *p++ = '"';
  return p;
}

static char *put_pair(char *p, const char *key, const char *value,
                      bool comma) {
  if (comma) *p++ = ',';
  p = put_json_string(p, key); *p++ = ':';
  return put_json_string(p, value);
}

static const char b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *put_base64(char *p, const unsigned char *src, size_t n) {
  *p++ = '"';
  while (n >= 3) {
    *p++ = b64[src[0] >> 2];
    *p++ = b64[((src[0] & 3) << 4) | (src[1] >> 4)];
    *p++ = b64[((src[1] & 15) << 2) | (src[2] >> 6)];
    *p++ = b64[src[2] & 63];
    src += 3; n -= 3;
  }
  if (n) {
    *p++ = b64[src[0] >> 2];
    *p++ = b64[(src[0] & 3) << 4 | (n == 2 ? src[1] >> 4 : 0)];
    *p++ = n == 2 ? b64[(src[1] & 15) << 2] : '=';
    *p++ = '=';
  }
  *p++ = '"';
  return p;
}

static ssize_t read_bytes(void *arg, size_t offset, void *dst, size_t cap) {
  struct bytes_reader *r = arg;
  if (offset >= r->length) return 0;
  size_t n = r->length - offset;
  if (n > cap) n = cap;
  memcpy(dst, r->data + offset, n);
  return (ssize_t)n;
}

static int collect(void *arg, const void *data, size_t len) {
  struct response_buffer *b = arg;
  if (len > b->capacity - b->length) return -1;
  memcpy(b->data + b->length, data, len);
  b->length += len;
  return 0;
}

static int config_valid(const struct mimo_cloud_client *c, bool asr) {
  const struct mimo_cloud_protocol *p;
  if (!c || !c->https || !(p = c->protocol) || !p->host ||
      !(asr ? p->asr_path : p->tts_path) || !p->authorization_header ||
      !p->authorization_scheme || !p->model_field ||
      !(asr ? p->asr_model : p->tts_model) ||
      !(asr ? p->decode_asr : p->decode_tts) ||
      !c->limits.max_token_bytes || !c->limits.max_request_body_bytes ||
      !c->limits.max_response_body_bytes) return 0;
  if ((asr && !p->encode_asr) || (!asr && !p->encode_tts)) return 0;
  if (!plain_value_valid(p->authorization_header, 64, false) ||
      !plain_value_valid(p->authorization_scheme, 64, false)) return 0;
  if (!json_key_valid(p->model_field)) return 0;
  if (asr) return json_key_valid(p->asr_audio_field) &&
                  json_key_valid(p->asr_format_field) &&
                  c->limits.max_audio_input_bytes;
  return json_key_valid(p->tts_text_field) &&
         (!p->tts_voice_field || json_key_valid(p->tts_voice_field)) &&
         c->limits.max_text_input_bytes;
}

static int do_request(struct mimo_cloud_client *c, const char *path,
                      const char *token, unsigned char *body, size_t body_len,
                      mimo_cloud_decode_fn decode, void *decode_arg,
                      void *output, size_t output_capacity,
                      size_t *output_length,
                      const struct vv_https_cancel *cancel) {
  int rc = MIMO_CLOUD_ENOMEM;
  char *authorization = NULL;
  unsigned char *response_body = NULL;
  size_t scheme_len = strlen(c->protocol->authorization_scheme);
  size_t token_len = strlen(token);
  if (scheme_len > SIZE_MAX - token_len - 2) return MIMO_CLOUD_ELIMIT;
  size_t auth_len = scheme_len + 1 + token_len;
  authorization = malloc(auth_len + 1);
  response_body = malloc(c->limits.max_response_body_bytes);
  if (!authorization || !response_body) goto done;
  memcpy(authorization, c->protocol->authorization_scheme, scheme_len);
  authorization[scheme_len] = ' ';
  memcpy(authorization + scheme_len + 1, token, token_len + 1);
  struct vv_https_header headers[] = {
    {c->protocol->authorization_header, authorization, true},
    {"Content-Type", "application/json", false},
    {"Accept", "application/json, audio/wav", false}
  };
  struct bytes_reader source = {body, body_len};
  struct vv_https_request request = {
    .method = "POST", .host = c->protocol->host,
    .port = c->protocol->port, .path = path,
    .headers = headers, .header_count = sizeof(headers) / sizeof(headers[0]),
    .body_length = body_len, .body_read = read_bytes, .body_arg = &source
  };
  struct response_buffer sink = {
    response_body, c->limits.max_response_body_bytes, 0
  };
  struct vv_https_response response;
  int hrc = vv_https_perform(c->https, &request, collect, &sink, cancel,
                             &response);
  if (hrc) { rc = hrc; goto done; }
  if (response.status_code != 200) { rc = MIMO_CLOUD_EHTTP; goto done; }
  rc = decode(decode_arg, response_body, sink.length, output,
              output_capacity, output_length);
done:
  if (authorization) {
    vv_https_secure_zero(authorization, auth_len + 1); free(authorization);
  }
  if (response_body) {
    vv_https_secure_zero(response_body, c->limits.max_response_body_bytes);
    free(response_body);
  }
  return rc;
}

static int checked_total(size_t fixed, const char *const *values,
                         size_t count, size_t extra, size_t limit,
                         size_t *total) {
  size_t n = fixed;
  for (size_t i = 0; i < count; ++i) {
    size_t e = escaped_size(values[i]);
    if (e == SIZE_MAX || n > SIZE_MAX - e) return MIMO_CLOUD_ELIMIT;
    n += e;
  }
  if (n > SIZE_MAX - extra) return MIMO_CLOUD_ELIMIT;
  n += extra;
  if (n > limit) return MIMO_CLOUD_ELIMIT;
  *total = n;
  return 0;
}

int mimo_cloud_transcribe(struct mimo_cloud_client *c,
                          const struct mimo_asr_request *q,
                          char *transcript, size_t transcript_capacity,
                          size_t *transcript_bytes,
                          const struct vv_https_cancel *cancel) {
  int rc = MIMO_CLOUD_EINVAL;
  unsigned char *body = NULL;
  size_t body_len = 0;
  if (transcript_bytes) *transcript_bytes = 0;
  if (!config_valid(c, true) || !q || !transcript || !transcript_capacity ||
      !transcript_bytes || !q->audio || !q->audio_bytes ||
      q->audio_bytes > c->limits.max_audio_input_bytes ||
      !plain_value_valid(q->token, c->limits.max_token_bytes, false) ||
      !plain_value_valid(q->audio_format, 32, false)) goto done;
  body = malloc(c->limits.max_request_body_bytes);
  if (!body) { rc = MIMO_CLOUD_ENOMEM; goto done; }
  rc = c->protocol->encode_asr(c->protocol, q, body,
      c->limits.max_request_body_bytes, &body_len);
  if (rc || !body_len || body_len > c->limits.max_request_body_bytes) goto done;
  event(c, "asr_request", (int)q->audio_bytes);
  rc = do_request(c, c->protocol->asr_path, q->token, body, body_len,
                  c->protocol->decode_asr, c->protocol->decode_asr_arg,
                  transcript, transcript_capacity, transcript_bytes, cancel);
done:
  if (body) {
    vv_https_secure_zero(body, c->limits.max_request_body_bytes); free(body);
  }
  event(c, "asr_done", rc);
  return rc;
}

int mimo_cloud_synthesize(struct mimo_cloud_client *c,
                          const struct mimo_tts_request *q,
                          unsigned char *audio, size_t audio_capacity,
                          size_t *audio_bytes,
                          const struct vv_https_cancel *cancel) {
  int rc = MIMO_CLOUD_EINVAL;
  unsigned char *body = NULL;
  size_t body_len = 0;
  if (audio_bytes) *audio_bytes = 0;
  if (!config_valid(c, false) || !q || !audio || !audio_capacity ||
      !audio_bytes || !plain_value_valid(q->token, c->limits.max_token_bytes,
      false) || !plain_value_valid(q->text, c->limits.max_text_input_bytes,
      false) || (c->protocol->tts_voice_field &&
      !plain_value_valid(q->voice, 128, false))) goto done;
  body = malloc(c->limits.max_request_body_bytes);
  if (!body) { rc = MIMO_CLOUD_ENOMEM; goto done; }
  rc = c->protocol->encode_tts(c->protocol, q, body,
      c->limits.max_request_body_bytes, &body_len);
  if (rc || !body_len || body_len > c->limits.max_request_body_bytes) goto done;
  event(c, "tts_request", (int)strlen(q->text));
  rc = do_request(c, c->protocol->tts_path, q->token, body, body_len,
                  c->protocol->decode_tts, c->protocol->decode_tts_arg,
                  audio, audio_capacity, audio_bytes, cancel);
done:
  if (body) {
    vv_https_secure_zero(body, c->limits.max_request_body_bytes); free(body);
  }
  event(c, "tts_done", rc);
  return rc;
}

int mimo_encode_flat_asr(const struct mimo_cloud_protocol *protocol,
                         const struct mimo_asr_request *request,
                         unsigned char *body, size_t body_capacity,
                         size_t *body_length) {
  if (!protocol || !request || !body || !body_length || !request->audio)
    return MIMO_CLOUD_EINVAL;
  if (!json_key_valid(protocol->model_field) || !protocol->asr_model ||
      !json_key_valid(protocol->asr_audio_field) ||
      !json_key_valid(protocol->asr_format_field) || !request->audio_format)
    return MIMO_CLOUD_EINVAL;
  if (request->audio_bytes > (SIZE_MAX / 4) * 3 - 2)
    return MIMO_CLOUD_ELIMIT;
  size_t encoded = ((request->audio_bytes + 2) / 3) * 4;
  const char *values[] = {protocol->model_field, protocol->asr_model,
    protocol->asr_audio_field, protocol->asr_format_field,
    request->audio_format};
  size_t length;
  int rc = checked_total(19, values, sizeof(values) / sizeof(values[0]),
                         encoded, body_capacity, &length);
  if (rc) return rc;
  char *p = (char *)body; *p++ = '{';
  p = put_pair(p, protocol->model_field, protocol->asr_model, false);
  *p++ = ','; p = put_json_string(p, protocol->asr_audio_field); *p++ = ':';
  p = put_base64(p, request->audio, request->audio_bytes);
  p = put_pair(p, protocol->asr_format_field, request->audio_format, true);
  *p++ = '}';
  if ((size_t)(p - (char *)body) != length) return MIMO_CLOUD_EINVAL;
  *body_length = length;
  return 0;
}

int mimo_encode_flat_tts(const struct mimo_cloud_protocol *protocol,
                         const struct mimo_tts_request *request,
                         unsigned char *body, size_t body_capacity,
                         size_t *body_length) {
  if (!protocol || !request || !body || !body_length || !request->text ||
      !json_key_valid(protocol->model_field) || !protocol->tts_model ||
      !json_key_valid(protocol->tts_text_field) ||
      (protocol->tts_voice_field && (!json_key_valid(protocol->tts_voice_field)
      || !request->voice))) return MIMO_CLOUD_EINVAL;
  const char *values[6] = {protocol->model_field, protocol->tts_model,
    protocol->tts_text_field, request->text, "", ""};
  size_t count = 4, fixed = 13;
  if (protocol->tts_voice_field) {
    values[count++] = protocol->tts_voice_field;
    values[count++] = request->voice;
    fixed += 6;
  }
  size_t length;
  int rc = checked_total(fixed, values, count, 0, body_capacity, &length);
  if (rc) return rc;
  char *p = (char *)body; *p++ = '{';
  p = put_pair(p, protocol->model_field, protocol->tts_model, false);
  p = put_pair(p, protocol->tts_text_field, request->text, true);
  if (protocol->tts_voice_field)
    p = put_pair(p, protocol->tts_voice_field, request->voice, true);
  *p++ = '}';
  if ((size_t)(p - (char *)body) != length) return MIMO_CLOUD_EINVAL;
  *body_length = length;
  return 0;
}

static const unsigned char *ws(const unsigned char *p,
                               const unsigned char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  return p;
}

static int json_string(const unsigned char **at, const unsigned char *end,
                       char *out, size_t cap, size_t *n) {
  const unsigned char *p = *at;
  size_t used = 0;
  unsigned utf8_need = 0;
  unsigned char utf8_min = 0x80, utf8_max = 0xbf;
  if (p == end || *p++ != '"') return MIMO_CLOUD_ERESPONSE;
  while (p < end && *p != '"') {
    unsigned char ch = *p++;
    if (ch == '\\') {
      if (utf8_need) return MIMO_CLOUD_ERESPONSE;
      if (p == end) return MIMO_CLOUD_ERESPONSE;
      ch = *p++;
      if (ch == '"' || ch == '\\' || ch == '/') { }
      else if (ch == 'b') ch = '\b'; else if (ch == 'f') ch = '\f';
      else if (ch == 'n') ch = '\n'; else if (ch == 'r') ch = '\r';
      else if (ch == 't') ch = '\t'; else return MIMO_CLOUD_ERESPONSE;
    } else if (ch < 0x20) {
      return MIMO_CLOUD_ERESPONSE;
    } else if (utf8_need) {
      if (ch < utf8_min || ch > utf8_max) return MIMO_CLOUD_ERESPONSE;
      --utf8_need; utf8_min = 0x80; utf8_max = 0xbf;
    } else if (ch >= 0x80) {
      if (ch >= 0xc2 && ch <= 0xdf) utf8_need = 1;
      else if (ch >= 0xe0 && ch <= 0xef) {
        utf8_need = 2;
        if (ch == 0xe0) utf8_min = 0xa0;
        if (ch == 0xed) utf8_max = 0x9f;
      } else if (ch >= 0xf0 && ch <= 0xf4) {
        utf8_need = 3;
        if (ch == 0xf0) utf8_min = 0x90;
        if (ch == 0xf4) utf8_max = 0x8f;
      } else return MIMO_CLOUD_ERESPONSE;
    }
    if (out) {
      if (used + 1 >= cap) return MIMO_CLOUD_EOUTPUT;
      out[used] = (char)ch;
    }
    ++used;
  }
  if (p == end || *p++ != '"' || utf8_need) return MIMO_CLOUD_ERESPONSE;
  if (out) out[used] = 0;
  *n = used; *at = p;
  return 0;
}

int mimo_decode_flat_json_string(void *field_name,
                                 const unsigned char *body, size_t body_len,
                                 void *output, size_t output_capacity,
                                 size_t *output_length) {
  if (!field_name || !body || !output || !output_capacity || !output_length)
    return MIMO_CLOUD_EINVAL;
  const unsigned char *p = ws(body, body + body_len), *end = body + body_len;
  char key[64]; size_t key_len = 0;
  if (p == end || *p++ != '{') return MIMO_CLOUD_ERESPONSE;
  p = ws(p, end);
  int rc = json_string(&p, end, key, sizeof(key), &key_len);
  if (rc) return rc;
  (void)key_len;
  if (strcmp(key, (const char *)field_name)) return MIMO_CLOUD_ERESPONSE;
  p = ws(p, end);
  if (p == end || *p++ != ':') return MIMO_CLOUD_ERESPONSE;
  p = ws(p, end);
  size_t value_len = 0;
  rc = json_string(&p, end, NULL, 0, &value_len);
  if (rc) return rc;
  p = ws(p, end);
  if (p == end || *p++ != '}') return MIMO_CLOUD_ERESPONSE;
  if (ws(p, end) != end) return MIMO_CLOUD_ERESPONSE;
  if (value_len + 1 > output_capacity) return MIMO_CLOUD_EOUTPUT;

  /* Parse again only after the complete envelope has passed validation, so a
   * rejected response cannot partially replace caller-visible output. */
  p = ws(body, end); ++p;
  p = ws(p, end);
  rc = json_string(&p, end, key, sizeof(key), &key_len);
  if (rc) return rc;
  p = ws(p, end); ++p;
  p = ws(p, end);
  rc = json_string(&p, end, output, output_capacity, &value_len);
  if (rc) return rc;
  *output_length = value_len;
  return 0;
}

int mimo_decode_wav(void *unused, const unsigned char *body, size_t body_len,
                    void *output, size_t output_capacity,
                    size_t *output_length) {
  (void)unused;
  if (!body || !output || !output_length) return MIMO_CLOUD_EINVAL;
  if (body_len < 44 || memcmp(body, "RIFF", 4) ||
      memcmp(body + 8, "WAVE", 4)) return MIMO_CLOUD_ERESPONSE;
  uint32_t riff_size = (uint32_t)body[4] | (uint32_t)body[5] << 8 |
                       (uint32_t)body[6] << 16 | (uint32_t)body[7] << 24;
  if ((uint64_t)riff_size + 8 != body_len) return MIMO_CLOUD_ERESPONSE;
  if (body_len > output_capacity) return MIMO_CLOUD_EOUTPUT;
  memcpy(output, body, body_len); *output_length = body_len;
  return 0;
}

const char *mimo_cloud_strerror(int error) {
  switch (error) {
    case 0: return "ok";
    case MIMO_CLOUD_EINVAL: return "invalid argument";
    case MIMO_CLOUD_ELIMIT: return "configured limit exceeded";
    case MIMO_CLOUD_ENOMEM: return "allocation failed";
    case MIMO_CLOUD_EHTTP: return "unexpected HTTP status";
    case MIMO_CLOUD_ERESPONSE: return "invalid API response";
    case MIMO_CLOUD_EOUTPUT: return "output buffer too small";
    default: return vv_https_strerror(error);
  }
}
