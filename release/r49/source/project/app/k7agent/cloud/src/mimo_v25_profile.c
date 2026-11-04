#include "mimo_v25_profile.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 32

struct writer {
  unsigned char *data;
  size_t capacity;
  size_t used;
  int error;
};

struct json_cursor {
  const unsigned char *at;
  const unsigned char *end;
  unsigned depth;
};

struct json_span {
  const unsigned char *begin;
  const unsigned char *end;
};

static const char base64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void write_bytes(struct writer *w, const void *data, size_t length) {
  if (w->error) return;
  if (length > w->capacity - w->used) {
    w->error = MIMO_CLOUD_ELIMIT;
    return;
  }
  memcpy(w->data + w->used, data, length);
  w->used += length;
}

static void write_literal(struct writer *w, const char *value) {
  write_bytes(w, value, strlen(value));
}

static int utf8_sequence(const unsigned char *p, const unsigned char *end,
                         size_t *length) {
  unsigned char a;
  if (p == end) return 0;
  a = *p;
  if (a < 0x80) { *length = 1; return a >= 0x20; }
  if (a >= 0xc2 && a <= 0xdf && end - p >= 2 &&
      p[1] >= 0x80 && p[1] <= 0xbf) { *length = 2; return 1; }
  if (a >= 0xe0 && a <= 0xef && end - p >= 3 &&
      p[1] >= (a == 0xe0 ? 0xa0 : 0x80) &&
      p[1] <= (a == 0xed ? 0x9f : 0xbf) &&
      p[2] >= 0x80 && p[2] <= 0xbf) { *length = 3; return 1; }
  if (a >= 0xf0 && a <= 0xf4 && end - p >= 4 &&
      p[1] >= (a == 0xf0 ? 0x90 : 0x80) &&
      p[1] <= (a == 0xf4 ? 0x8f : 0xbf) &&
      p[2] >= 0x80 && p[2] <= 0xbf &&
      p[3] >= 0x80 && p[3] <= 0xbf) { *length = 4; return 1; }
  return 0;
}

static int json_text_valid(const char *value) {
  const unsigned char *p = (const unsigned char *)value;
  const unsigned char *end;
  if (!value) return 0;
  end = p + strlen(value);
  while (p < end) {
    size_t n;
    if (!utf8_sequence(p, end, &n)) return 0;
    p += n;
  }
  return 1;
}

static void write_json_string(struct writer *w, const char *value) {
  static const char hex[] = "0123456789abcdef";
  const unsigned char *p = (const unsigned char *)value;
  write_literal(w, "\"");
  while (*p) {
    unsigned char ch = *p++;
    if (ch == '"' || ch == '\\') {
      unsigned char pair[2] = {'\\', ch};
      write_bytes(w, pair, sizeof(pair));
    } else if (ch < 0x20) {
      char escape[6] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15]};
      write_bytes(w, escape, sizeof(escape));
    } else {
      write_bytes(w, &ch, 1);
    }
  }
  write_literal(w, "\"");
}

static void write_base64(struct writer *w, const unsigned char *source,
                         size_t length) {
  write_literal(w, "\"");
  while (length >= 3) {
    char out[4];
    out[0] = base64_table[source[0] >> 2];
    out[1] = base64_table[((source[0] & 3) << 4) | (source[1] >> 4)];
    out[2] = base64_table[((source[1] & 15) << 2) | (source[2] >> 6)];
    out[3] = base64_table[source[2] & 63];
    write_bytes(w, out, sizeof(out));
    source += 3;
    length -= 3;
  }
  if (length) {
    char out[4];
    out[0] = base64_table[source[0] >> 2];
    out[1] = base64_table[((source[0] & 3) << 4) |
                          (length == 2 ? source[1] >> 4 : 0)];
    out[2] = length == 2 ? base64_table[(source[1] & 15) << 2] : '=';
    out[3] = '=';
    write_bytes(w, out, sizeof(out));
  }
  write_literal(w, "\"");
}

static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static int wav_valid(const unsigned char *data, size_t length) {
  size_t at = 12;
  bool have_fmt = false, have_data = false;
  if (!data || length < 44 || memcmp(data, "RIFF", 4) ||
      memcmp(data + 8, "WAVE", 4) ||
      (uint64_t)le32(data + 4) + 8 != length) return 0;
  while (at <= length - 8) {
    uint32_t chunk = le32(data + at + 4);
    size_t padded = (size_t)chunk + (chunk & 1u);
    if (padded > length - at - 8) return 0;
    if (!memcmp(data + at, "fmt ", 4)) {
      if (have_fmt || chunk < 16) return 0;
      have_fmt = true;
    } else if (!memcmp(data + at, "data", 4)) {
      if (have_data) return 0;
      have_data = true;
    }
    at += 8 + padded;
  }
  return at == length && have_fmt && have_data;
}

static int encoded_audio_length(size_t raw, size_t *encoded) {
  if (raw > (SIZE_MAX / 4) * 3 - 2) return MIMO_CLOUD_ELIMIT;
  *encoded = ((raw + 2) / 3) * 4;
  return *encoded <= MIMO_V25_MAX_ASR_BASE64_BYTES ? 0 : MIMO_CLOUD_ELIMIT;
}

int mimo_v25_encode_asr(const struct mimo_cloud_protocol *protocol,
                        const struct mimo_asr_request *request,
                        unsigned char *body, size_t body_capacity,
                        size_t *body_length) {
  struct writer w = {body, body_capacity, 0, 0};
  size_t encoded;
  if (body_length) *body_length = 0;
  if (!protocol || !request || !body || !body_length || !request->audio ||
      !request->audio_bytes || !protocol->asr_model ||
      strcmp(protocol->asr_model, "mimo-v2.5-asr") ||
      !request->audio_format ||
      (strcmp(request->audio_format, "wav") &&
       strcmp(request->audio_format, "mp3"))) return MIMO_CLOUD_EINVAL;
  if (!strcmp(request->audio_format, "wav") &&
      !wav_valid(request->audio, request->audio_bytes))
    return MIMO_CLOUD_EINVAL;
  if (encoded_audio_length(request->audio_bytes, &encoded))
    return MIMO_CLOUD_ELIMIT;
  (void)encoded;
  write_literal(&w, "{\"model\":");
  write_json_string(&w, protocol->asr_model);
  write_literal(&w, ",\"messages\":[{\"role\":\"user\",\"content\":[{"
                    "\"type\":\"input_audio\",\"input_audio\":{\"data\":");
  write_base64(&w, request->audio, request->audio_bytes);
  write_literal(&w, ",\"format\":");
  write_json_string(&w, request->audio_format);
  write_literal(&w, "}}]}],\"asr_options\":{\"language\":\"zh\"},"
                    "\"stream\":false}");
  if (w.error) return w.error;
  *body_length = w.used;
  return 0;
}

int mimo_v25_encode_tts(const struct mimo_cloud_protocol *protocol,
                        const struct mimo_tts_request *request,
                        unsigned char *body, size_t body_capacity,
                        size_t *body_length) {
  struct writer w = {body, body_capacity, 0, 0};
  if (body_length) *body_length = 0;
  if (!protocol || !request || !body || !body_length || !request->text ||
      !*request->text || !request->voice || !*request->voice ||
      !protocol->tts_model ||
      strcmp(protocol->tts_model, "mimo-v2.5-tts") ||
      !json_text_valid(request->text) || !json_text_valid(request->voice))
    return MIMO_CLOUD_EINVAL;
  write_literal(&w, "{\"model\":");
  write_json_string(&w, protocol->tts_model);
  write_literal(&w, ",\"messages\":[{\"role\":\"assistant\",\"content\":");
  write_json_string(&w, request->text);
  write_literal(&w, "}],\"audio\":{\"format\":\"wav\",\"voice\":");
  write_json_string(&w, request->voice);
  write_literal(&w, "},\"stream\":false}");
  if (w.error) return w.error;
  *body_length = w.used;
  return 0;
}

static void skip_ws(struct json_cursor *c) {
  while (c->at < c->end && (*c->at == ' ' || *c->at == '\t' ||
         *c->at == '\r' || *c->at == '\n')) ++c->at;
}

static int hex_value(unsigned char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static int read_hex4(const unsigned char **at, const unsigned char *end,
                     uint32_t *value) {
  uint32_t out = 0;
  if ((size_t)(end - *at) < 4) return MIMO_CLOUD_ERESPONSE;
  for (unsigned i = 0; i < 4; ++i) {
    int v = hex_value((*at)[i]);
    if (v < 0) return MIMO_CLOUD_ERESPONSE;
    out = (out << 4) | (uint32_t)v;
  }
  *at += 4;
  *value = out;
  return 0;
}

static int emit_codepoint(uint32_t cp, unsigned char *output, size_t capacity,
                          size_t *used) {
  unsigned char bytes[4];
  size_t count;
  if (cp <= 0x7f) { bytes[0] = (unsigned char)cp; count = 1; }
  else if (cp <= 0x7ff) {
    bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
    bytes[1] = (unsigned char)(0x80 | (cp & 0x3f)); count = 2;
  } else if (cp <= 0xffff) {
    bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
    bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | (cp & 0x3f)); count = 3;
  } else {
    bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
    bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    bytes[3] = (unsigned char)(0x80 | (cp & 0x3f)); count = 4;
  }
  if (output) {
    if (count > capacity - *used) return MIMO_CLOUD_EOUTPUT;
    memcpy(output + *used, bytes, count);
  }
  *used += count;
  return 0;
}

static int read_json_string(struct json_cursor *c, unsigned char *output,
                            size_t capacity, size_t *length,
                            struct json_span *span) {
  const unsigned char *start;
  size_t used = 0;
  if (c->at == c->end || *c->at++ != '"') return MIMO_CLOUD_ERESPONSE;
  start = c->at - 1;
  while (c->at < c->end && *c->at != '"') {
    unsigned char ch = *c->at++;
    if (ch == '\\') {
      uint32_t cp;
      if (c->at == c->end) return MIMO_CLOUD_ERESPONSE;
      ch = *c->at++;
      if (ch == '"' || ch == '\\' || ch == '/') cp = ch;
      else if (ch == 'b') cp = '\b';
      else if (ch == 'f') cp = '\f';
      else if (ch == 'n') cp = '\n';
      else if (ch == 'r') cp = '\r';
      else if (ch == 't') cp = '\t';
      else if (ch == 'u') {
        int rc = read_hex4(&c->at, c->end, &cp);
        if (rc) return rc;
        if (cp >= 0xd800 && cp <= 0xdbff) {
          uint32_t low;
          if ((size_t)(c->end - c->at) < 6 || c->at[0] != '\\' ||
              c->at[1] != 'u') return MIMO_CLOUD_ERESPONSE;
          c->at += 2;
          rc = read_hex4(&c->at, c->end, &low);
          if (rc || low < 0xdc00 || low > 0xdfff)
            return MIMO_CLOUD_ERESPONSE;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
          return MIMO_CLOUD_ERESPONSE;
        }
      } else return MIMO_CLOUD_ERESPONSE;
      {
        int rc = emit_codepoint(cp, output, capacity, &used);
        if (rc) return rc;
      }
    } else {
      size_t count;
      const unsigned char *sequence = c->at - 1;
      if (!utf8_sequence(sequence, c->end, &count))
        return MIMO_CLOUD_ERESPONSE;
      if (output) {
        if (count > capacity - used) return MIMO_CLOUD_EOUTPUT;
        memcpy(output + used, sequence, count);
      }
      used += count;
      c->at = sequence + count;
    }
  }
  if (c->at == c->end) return MIMO_CLOUD_ERESPONSE;
  ++c->at;
  if (span) { span->begin = start; span->end = c->at; }
  if (length) *length = used;
  return 0;
}

static int key_is(struct json_cursor *c, const char *wanted, bool *match) {
  struct json_cursor copy = *c;
  unsigned char key[32];
  size_t length = 0, wanted_length = strlen(wanted);
  int rc = read_json_string(&copy, key, sizeof(key), &length, NULL);
  if (rc == MIMO_CLOUD_EOUTPUT) {
    rc = read_json_string(c, NULL, 0, NULL, NULL);
    if (!rc) *match = false;
    return rc;
  }
  if (rc) return rc;
  *c = copy;
  *match = length == wanted_length && !memcmp(key, wanted, length);
  return 0;
}

static int skip_value(struct json_cursor *c);

static int skip_array(struct json_cursor *c) {
  if (++c->depth > JSON_MAX_DEPTH) return MIMO_CLOUD_ERESPONSE;
  ++c->at;
  skip_ws(c);
  if (c->at < c->end && *c->at == ']') { ++c->at; --c->depth; return 0; }
  for (;;) {
    int rc = skip_value(c);
    if (rc) return rc;
    skip_ws(c);
    if (c->at == c->end) return MIMO_CLOUD_ERESPONSE;
    if (*c->at == ']') { ++c->at; --c->depth; return 0; }
    if (*c->at++ != ',') return MIMO_CLOUD_ERESPONSE;
    skip_ws(c);
  }
}

static int skip_object(struct json_cursor *c) {
  if (++c->depth > JSON_MAX_DEPTH) return MIMO_CLOUD_ERESPONSE;
  ++c->at;
  skip_ws(c);
  if (c->at < c->end && *c->at == '}') { ++c->at; --c->depth; return 0; }
  for (;;) {
    int rc = read_json_string(c, NULL, 0, NULL, NULL);
    if (rc) return rc;
    skip_ws(c);
    if (c->at == c->end || *c->at++ != ':') return MIMO_CLOUD_ERESPONSE;
    skip_ws(c);
    rc = skip_value(c);
    if (rc) return rc;
    skip_ws(c);
    if (c->at == c->end) return MIMO_CLOUD_ERESPONSE;
    if (*c->at == '}') { ++c->at; --c->depth; return 0; }
    if (*c->at++ != ',') return MIMO_CLOUD_ERESPONSE;
    skip_ws(c);
  }
}

static int skip_number(struct json_cursor *c) {
  const unsigned char *p = c->at;
  if (p < c->end && *p == '-') ++p;
  if (p == c->end) return MIMO_CLOUD_ERESPONSE;
  if (*p == '0') ++p;
  else if (*p >= '1' && *p <= '9') {
    do { ++p; } while (p < c->end && *p >= '0' && *p <= '9');
  } else return MIMO_CLOUD_ERESPONSE;
  if (p < c->end && *p == '.') {
    ++p;
    if (p == c->end || *p < '0' || *p > '9') return MIMO_CLOUD_ERESPONSE;
    do { ++p; } while (p < c->end && *p >= '0' && *p <= '9');
  }
  if (p < c->end && (*p == 'e' || *p == 'E')) {
    ++p;
    if (p < c->end && (*p == '+' || *p == '-')) ++p;
    if (p == c->end || *p < '0' || *p > '9') return MIMO_CLOUD_ERESPONSE;
    do { ++p; } while (p < c->end && *p >= '0' && *p <= '9');
  }
  c->at = p;
  return 0;
}

static int skip_value(struct json_cursor *c) {
  skip_ws(c);
  if (c->at == c->end) return MIMO_CLOUD_ERESPONSE;
  if (*c->at == '"') return read_json_string(c, NULL, 0, NULL, NULL);
  if (*c->at == '{') return skip_object(c);
  if (*c->at == '[') return skip_array(c);
  if (*c->at == '-' || (*c->at >= '0' && *c->at <= '9'))
    return skip_number(c);
  if ((size_t)(c->end - c->at) >= 4 && !memcmp(c->at, "true", 4))
    { c->at += 4; return 0; }
  if ((size_t)(c->end - c->at) >= 5 && !memcmp(c->at, "false", 5))
    { c->at += 5; return 0; }
  if ((size_t)(c->end - c->at) >= 4 && !memcmp(c->at, "null", 4))
    { c->at += 4; return 0; }
  return MIMO_CLOUD_ERESPONSE;
}

static int parse_member_prefix(struct json_cursor *c, bool *first,
                               bool *done) {
  skip_ws(c);
  if (c->at == c->end) return MIMO_CLOUD_ERESPONSE;
  if (*c->at == '}') { ++c->at; *done = true; return 0; }
  if (!*first) {
    if (*c->at++ != ',') return MIMO_CLOUD_ERESPONSE;
    skip_ws(c);
  }
  *first = false;
  return 0;
}

static int parse_audio_object(struct json_cursor *c, struct json_span *data) {
  bool first = true, done = false, seen = false;
  if (c->at == c->end || *c->at++ != '{') return MIMO_CLOUD_ERESPONSE;
  while (!done) {
    bool match;
    int rc = parse_member_prefix(c, &first, &done);
    if (rc || done) break;
    rc = key_is(c, "data", &match); if (rc) return rc;
    skip_ws(c);
    if (c->at == c->end || *c->at++ != ':') return MIMO_CLOUD_ERESPONSE;
    skip_ws(c);
    if (match) {
      if (seen) return MIMO_CLOUD_ERESPONSE;
      seen = true;
      rc = read_json_string(c, NULL, 0, NULL, data);
    } else rc = skip_value(c);
    if (rc) return rc;
  }
  return done && seen ? 0 : MIMO_CLOUD_ERESPONSE;
}

static int parse_message(struct json_cursor *c, bool want_audio,
                         struct json_span *result) {
  bool first = true, done = false, seen = false;
  if (c->at == c->end || *c->at++ != '{') return MIMO_CLOUD_ERESPONSE;
  while (!done) {
    bool match;
    int rc = parse_member_prefix(c, &first, &done);
    if (rc || done) break;
    rc = key_is(c, want_audio ? "audio" : "content", &match);
    if (rc) return rc;
    skip_ws(c);
    if (c->at == c->end || *c->at++ != ':') return MIMO_CLOUD_ERESPONSE;
    skip_ws(c);
    if (match) {
      if (seen) return MIMO_CLOUD_ERESPONSE;
      seen = true;
      rc = want_audio ? parse_audio_object(c, result) :
                        read_json_string(c, NULL, 0, NULL, result);
    } else rc = skip_value(c);
    if (rc) return rc;
  }
  return done && seen ? 0 : MIMO_CLOUD_ERESPONSE;
}

static int parse_choice(struct json_cursor *c, bool want_audio,
                        struct json_span *result) {
  bool first = true, done = false, seen = false;
  if (c->at == c->end || *c->at++ != '{') return MIMO_CLOUD_ERESPONSE;
  while (!done) {
    bool match;
    int rc = parse_member_prefix(c, &first, &done);
    if (rc || done) break;
    rc = key_is(c, "message", &match); if (rc) return rc;
    skip_ws(c);
    if (c->at == c->end || *c->at++ != ':') return MIMO_CLOUD_ERESPONSE;
    skip_ws(c);
    if (match) {
      if (seen) return MIMO_CLOUD_ERESPONSE;
      seen = true; rc = parse_message(c, want_audio, result);
    } else rc = skip_value(c);
    if (rc) return rc;
  }
  return done && seen ? 0 : MIMO_CLOUD_ERESPONSE;
}

static int parse_choices(struct json_cursor *c, bool want_audio,
                         struct json_span *result) {
  int rc;
  if (c->at == c->end || *c->at++ != '[') return MIMO_CLOUD_ERESPONSE;
  skip_ws(c);
  if (c->at == c->end || *c->at == ']') return MIMO_CLOUD_ERESPONSE;
  rc = parse_choice(c, want_audio, result);
  if (rc) return rc;
  skip_ws(c);
  while (c->at < c->end && *c->at == ',') {
    ++c->at; skip_ws(c); rc = skip_value(c); if (rc) return rc; skip_ws(c);
  }
  if (c->at == c->end || *c->at++ != ']') return MIMO_CLOUD_ERESPONSE;
  return 0;
}

static int locate_result(const unsigned char *body, size_t body_len,
                         bool want_audio, struct json_span *result) {
  struct json_cursor c = {body, body + body_len, 0};
  bool first = true, done = false, seen = false;
  skip_ws(&c);
  if (c.at == c.end || *c.at++ != '{') return MIMO_CLOUD_ERESPONSE;
  while (!done) {
    bool match;
    int rc = parse_member_prefix(&c, &first, &done);
    if (rc || done) break;
    rc = key_is(&c, "choices", &match); if (rc) return rc;
    skip_ws(&c);
    if (c.at == c.end || *c.at++ != ':') return MIMO_CLOUD_ERESPONSE;
    skip_ws(&c);
    if (match) {
      if (seen) return MIMO_CLOUD_ERESPONSE;
      seen = true; rc = parse_choices(&c, want_audio, result);
    } else rc = skip_value(&c);
    if (rc) return rc;
  }
  skip_ws(&c);
  return done && seen && c.at == c.end ? 0 : MIMO_CLOUD_ERESPONSE;
}

static int base64_value(unsigned char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

static int decode_base64_span(struct json_span span, unsigned char *output,
                              size_t capacity, size_t *output_length) {
  struct json_cursor c = {span.begin, span.end, 0};
  size_t text_length = 0, used = 0;
  unsigned char *text;
  int rc = read_json_string(&c, NULL, 0, &text_length, NULL);
  if (rc || c.at != c.end || !text_length || (text_length & 3u))
    return MIMO_CLOUD_ERESPONSE;
  text = malloc(text_length);
  if (!text) return MIMO_CLOUD_ENOMEM;
  c.at = span.begin;
  rc = read_json_string(&c, text, text_length, &text_length, NULL);
  if (rc) goto done;
  for (size_t i = 0; i < text_length; i += 4) {
    int a = base64_value(text[i]), b = base64_value(text[i + 1]);
    int d2 = text[i + 2] == '=' ? -2 : base64_value(text[i + 2]);
    int d3 = text[i + 3] == '=' ? -2 : base64_value(text[i + 3]);
    unsigned char bytes[3]; size_t count = 3;
    if (a < 0 || b < 0 || d2 == -1 || d3 == -1 ||
        (d2 == -2 && d3 != -2) ||
        ((d2 == -2 || d3 == -2) && i + 4 != text_length)) {
      rc = MIMO_CLOUD_ERESPONSE; goto done;
    }
    if (d2 == -2) {
      if (b & 15) { rc = MIMO_CLOUD_ERESPONSE; goto done; }
      d2 = 0; d3 = 0; count = 1;
    } else if (d3 == -2) {
      if (d2 & 3) { rc = MIMO_CLOUD_ERESPONSE; goto done; }
      d3 = 0; count = 2;
    }
    bytes[0] = (unsigned char)((a << 2) | (b >> 4));
    bytes[1] = (unsigned char)((b << 4) | (d2 >> 2));
    bytes[2] = (unsigned char)((d2 << 6) | d3);
    if (count > capacity - used) { rc = MIMO_CLOUD_EOUTPUT; goto done; }
    if (output) memcpy(output + used, bytes, count);
    used += count;
  }
  *output_length = used;
  rc = 0;
done:
  memset(text, 0, text_length);
  free(text);
  return rc;
}

int mimo_v25_decode_asr(void *unused, const unsigned char *body,
                        size_t body_len, void *output,
                        size_t output_capacity, size_t *output_length) {
  struct json_span result;
  struct json_cursor c;
  unsigned char *decoded = NULL;
  size_t needed = 0;
  int rc;
  (void)unused;
  if (output_length) *output_length = 0;
  if (!body || !output || !output_capacity || !output_length)
    return MIMO_CLOUD_EINVAL;
  rc = locate_result(body, body_len, false, &result);
  if (rc) return rc;
  c = (struct json_cursor){result.begin, result.end, 0};
  rc = read_json_string(&c, NULL, 0, &needed, NULL);
  if (rc || c.at != c.end || needed + 1 > output_capacity)
    return rc ? rc : MIMO_CLOUD_EOUTPUT;
  decoded = malloc(needed + 1);
  if (!decoded) return MIMO_CLOUD_ENOMEM;
  c.at = result.begin;
  rc = read_json_string(&c, decoded, needed, &needed, NULL);
  if (!rc && memchr(decoded, 0, needed)) rc = MIMO_CLOUD_ERESPONSE;
  if (!rc) {
    memcpy(output, decoded, needed);
    ((unsigned char *)output)[needed] = 0;
    *output_length = needed;
  }
  memset(decoded, 0, needed + 1);
  free(decoded);
  return rc;
}

int mimo_v25_decode_tts(void *unused, const unsigned char *body,
                        size_t body_len, void *output,
                        size_t output_capacity, size_t *output_length) {
  struct json_span result;
  unsigned char *decoded = NULL;
  size_t needed = 0;
  int rc;
  (void)unused;
  if (output_length) *output_length = 0;
  if (!body || !output || !output_capacity || !output_length)
    return MIMO_CLOUD_EINVAL;
  rc = locate_result(body, body_len, true, &result);
  if (rc) return rc;
  rc = decode_base64_span(result, NULL, SIZE_MAX, &needed);
  if (rc) return rc;
  if (needed > output_capacity) return MIMO_CLOUD_EOUTPUT;
  decoded = malloc(needed ? needed : 1);
  if (!decoded) return MIMO_CLOUD_ENOMEM;
  rc = decode_base64_span(result, decoded, needed, &needed);
  if (!rc && !wav_valid(decoded, needed)) rc = MIMO_CLOUD_ERESPONSE;
  if (!rc) {
    memcpy(output, decoded, needed);
    *output_length = needed;
  }
  memset(decoded, 0, needed);
  free(decoded);
  return rc;
}

int mimo_v25_profile_init(struct mimo_cloud_protocol *protocol,
                          const char *host) {
  const unsigned char *at;
  size_t host_length;
  if (!protocol || !host || !(host_length = strlen(host)) ||
      host_length > 253 || host[0] == '.' || host[host_length - 1] == '.')
    return MIMO_CLOUD_EINVAL;
  for (at = (const unsigned char *)host; *at; ++at) {
    if (!(isalnum(*at) || *at == '.' || *at == '-'))
      return MIMO_CLOUD_EINVAL;
  }
  *protocol = (struct mimo_cloud_protocol){
    .host = host,
    .port = 443,
    .asr_path = "/v1/chat/completions",
    .tts_path = "/v1/chat/completions",
    .authorization_header = "Authorization",
    .authorization_scheme = "Bearer",
    .asr_model = "mimo-v2.5-asr",
    .tts_model = "mimo-v2.5-tts",
    .model_field = "model",
    .asr_audio_field = "input_audio",
    .asr_format_field = "format",
    .tts_text_field = "content",
    .tts_voice_field = "voice",
    .encode_asr = mimo_v25_encode_asr,
    .encode_tts = mimo_v25_encode_tts,
    .decode_asr = mimo_v25_decode_asr,
    .decode_tts = mimo_v25_decode_tts
  };
  return 0;
}
