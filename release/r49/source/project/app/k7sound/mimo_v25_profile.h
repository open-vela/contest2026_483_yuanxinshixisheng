#ifndef MIMO_V25_PROFILE_H
#define MIMO_V25_PROFILE_H

#include "mimo_cloud_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The official service describes the encoded audio limit as 10 MB without
 * defining a binary-megabyte interpretation. This profile uses the smaller,
 * decimal interpretation so it cannot exceed either interpretation. */
#define MIMO_V25_MAX_ASR_BASE64_BYTES ((size_t)10000000)

/* Initialize the official, non-streaming MiMo v2.5 speech profile. `host` is
 * retained by pointer and must outlive the protocol. Token Plan callers must
 * pass the exact regional host shown in their console. Authentication uses
 * the officially supported Authorization: Bearer form. */
int mimo_v25_profile_init(struct mimo_cloud_protocol *protocol,
                          const char *host);

int mimo_v25_encode_asr(const struct mimo_cloud_protocol *protocol,
                        const struct mimo_asr_request *request,
                        unsigned char *body, size_t body_capacity,
                        size_t *body_length);
int mimo_v25_encode_tts(const struct mimo_cloud_protocol *protocol,
                        const struct mimo_tts_request *request,
                        unsigned char *body, size_t body_capacity,
                        size_t *body_length);
int mimo_v25_decode_asr(void *unused, const unsigned char *body,
                        size_t body_len, void *output,
                        size_t output_capacity, size_t *output_length);
int mimo_v25_decode_tts(void *unused, const unsigned char *body,
                        size_t body_len, void *output,
                        size_t output_capacity, size_t *output_length);

#ifdef __cplusplus
}
#endif
#endif
