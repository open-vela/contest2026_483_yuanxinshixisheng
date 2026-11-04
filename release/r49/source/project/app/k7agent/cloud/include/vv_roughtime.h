#ifndef VV_ROUGHTIME_H
#define VV_ROUGHTIME_H
#include <stddef.h>
#include <stdint.h>
#define VV_ROUGHTIME_NONCE_BYTES 64u
#define VV_ROUGHTIME_REQUEST_BYTES 1024u
#define VV_ROUGHTIME_RESPONSE_MAX 4096u
struct vv_roughtime_result { uint64_t midpoint_us; uint32_t radius_us; };
struct vv_roughtime_crypto {
 int (*verify)(const uint8_t signature[64],const uint8_t *message,size_t bytes,const uint8_t key[32]);
 int (*sha512)(uint8_t hash[64],const uint8_t *message,size_t bytes);
};
/* Legacy Cloudflare Roughtime only. The caller owns nonce freshness, bounded
 * monotonic RTT, one-shot consumption and source-policy approval. No clock writes. */
int vv_roughtime_request(const uint8_t nonce[64],uint8_t request[1024]);
int vv_roughtime_verify_with_crypto(const uint8_t *response,size_t bytes,
 const uint8_t nonce[64],struct vv_roughtime_result *,const struct vv_roughtime_crypto *);
int vv_roughtime_verify(const uint8_t *response,size_t bytes,const uint8_t nonce[64],struct vv_roughtime_result *);
#endif
