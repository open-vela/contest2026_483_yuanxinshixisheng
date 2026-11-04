#ifndef MIMO_CLOUD_CLIENT_H
#define MIMO_CLOUD_CLIENT_H

#include "vv_https_client.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mimo_cloud_error {
  MIMO_CLOUD_OK = 0,
  MIMO_CLOUD_EINVAL = -2000,
  MIMO_CLOUD_ELIMIT,
  MIMO_CLOUD_ENOMEM,
  MIMO_CLOUD_EHTTP,
  MIMO_CLOUD_ERESPONSE,
  MIMO_CLOUD_EOUTPUT
};

typedef void (*mimo_cloud_log_fn)(void *arg, const char *event, int value);

struct mimo_cloud_protocol;
struct mimo_asr_request;
struct mimo_tts_request;

typedef int (*mimo_cloud_encode_asr_fn)(
  const struct mimo_cloud_protocol *protocol,
  const struct mimo_asr_request *request, unsigned char *body,
  size_t body_capacity, size_t *body_length);
typedef int (*mimo_cloud_encode_tts_fn)(
  const struct mimo_cloud_protocol *protocol,
  const struct mimo_tts_request *request, unsigned char *body,
  size_t body_capacity, size_t *body_length);

/* A decoder must validate the complete response before copying any output. */
typedef int (*mimo_cloud_decode_fn)(void *arg, const unsigned char *body,
                                    size_t body_len, void *output,
                                    size_t output_capacity,
                                    size_t *output_length);

struct mimo_cloud_protocol {
  const char *host;
  uint16_t port; /* 0 means 443. */
  const char *asr_path;
  const char *tts_path;
  const char *authorization_header;
  const char *authorization_scheme;
  const char *asr_model;
  const char *tts_model;
  const char *model_field;
  const char *asr_audio_field;
  const char *asr_format_field;
  const char *tts_text_field;
  const char *tts_voice_field; /* May be NULL when the API does not use it. */
  mimo_cloud_encode_asr_fn encode_asr;
  mimo_cloud_encode_tts_fn encode_tts;
  mimo_cloud_decode_fn decode_asr;
  void *decode_asr_arg;
  mimo_cloud_decode_fn decode_tts;
  void *decode_tts_arg;
};

struct mimo_cloud_limits {
  size_t max_token_bytes;
  size_t max_audio_input_bytes;
  size_t max_text_input_bytes;
  size_t max_request_body_bytes;
  size_t max_response_body_bytes;
};

struct mimo_cloud_client {
  struct vv_https_client *https;
  const struct mimo_cloud_protocol *protocol;
  struct mimo_cloud_limits limits;
  mimo_cloud_log_fn log; /* Receives fixed event names and numeric values only. */
  void *log_arg;
};

struct mimo_asr_request {
  const char *token;
  const unsigned char *audio;
  size_t audio_bytes;
  const char *audio_format;
};

struct mimo_tts_request {
  const char *token;
  const char *text;
  const char *voice; /* Required only when tts_voice_field is configured. */
};

int mimo_cloud_transcribe(struct mimo_cloud_client *client,
                          const struct mimo_asr_request *request,
                          char *transcript, size_t transcript_capacity,
                          size_t *transcript_bytes,
                          const struct vv_https_cancel *cancel);

int mimo_cloud_synthesize(struct mimo_cloud_client *client,
                          const struct mimo_tts_request *request,
                          unsigned char *audio, size_t audio_capacity,
                          size_t *audio_bytes,
                          const struct vv_https_cancel *cancel);

/* Flat JSON encoders exist for tests and private gateways. They are not
 * claimed to match Xiaomi's chat-completions speech schema. */
int mimo_encode_flat_asr(const struct mimo_cloud_protocol *protocol,
                         const struct mimo_asr_request *request,
                         unsigned char *body, size_t body_capacity,
                         size_t *body_length);
int mimo_encode_flat_tts(const struct mimo_cloud_protocol *protocol,
                         const struct mimo_tts_request *request,
                         unsigned char *body, size_t body_capacity,
                         size_t *body_length);

/* Strict helpers for protocol profiles. The flat JSON helper accepts exactly
 * one top-level string member and rejects duplicate/trailing fields. */
int mimo_decode_flat_json_string(void *field_name,
                                 const unsigned char *body, size_t body_len,
                                 void *output, size_t output_capacity,
                                 size_t *output_length);
int mimo_decode_wav(void *unused, const unsigned char *body, size_t body_len,
                    void *output, size_t output_capacity,
                    size_t *output_length);

const char *mimo_cloud_strerror(int error);

#ifdef __cplusplus
}
#endif
#endif
