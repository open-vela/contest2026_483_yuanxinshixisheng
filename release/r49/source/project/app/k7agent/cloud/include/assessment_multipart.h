#ifndef K7_ASSESSMENT_MULTIPART_H
#define K7_ASSESSMENT_MULTIPART_H
#include <stddef.h>
#include <sys/types.h>
struct k7_upload_image {const unsigned char *data;size_t bytes;};
struct k7_assessment_body {
  char headers[4][256],metadata[512],trailer[80],content_type[96];
  const void *segments[13];size_t lengths[13],total;
};
/* Caller supplies real session/consent refs and a 32-hex boundary token.
 * Three image copies must remain immutable/alive through all body reads.
 * This encodes a request body only; no authentication, network or upload claim. */
int k7_assessment_body_init(struct k7_assessment_body *,const char *capture_session,
                           const char *consent_ref,const char *boundary_hex,
                           const struct k7_upload_image images[3]);
/* Compatible with vv_https_body_read_fn; supports short/random-offset reads. */
ssize_t k7_assessment_body_read(void *,size_t offset,void *,size_t capacity);
#endif
