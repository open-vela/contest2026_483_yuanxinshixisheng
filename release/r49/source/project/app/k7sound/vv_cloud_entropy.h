#ifndef VV_CLOUD_ENTROPY_H
#define VV_CLOUD_ENTROPY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
/* Disabled until explicitly configured with an independently approved strong
 * source. This adapter does NOT certify a device, estimate entropy, or replace
 * hardware startup/continuous health tests. No fallback exists.
 * All calls serialized; callbacks must not reenter. read_exact must bound its
 * synchronous operation by the absolute monotonic deadline; partial output
 * is failure. A timer, PRNG, device ID, network time or unaudited HCI LE_Rand
 * is not an approved source merely because it can fill bytes. */
struct vv_cloud_entropy_config {
  ssize_t (*read_exact)(void *,uint8_t *,size_t,uint64_t deadline_ms);
  bool (*approved_and_healthy)(void *);
  uint64_t (*now_ms)(void *); /* UINT64_MAX on failure */
  void *arg;
  uint32_t max_wait_ms; /* 1..1000 */
};
struct vv_cloud_entropy {
  struct vv_cloud_entropy_config cfg;
  bool enabled,failed,entered;
  uint64_t last_ms;
};
#define VV_CLOUD_ENTROPY_MAX_REQUEST 1024u
/* Caller supplies zero-initialized state. Configure only from trusted board
 * provisioning, never a remote bool.
 * Any failure requires fresh explicit configure; no automatic recovery. */
int vv_cloud_entropy_configure(struct vv_cloud_entropy *,const struct vv_cloud_entropy_config *);
void vv_cloud_entropy_disable(struct vv_cloud_entropy *);
/* Signature compatible with mbedtls_ctr_drbg_seed f_entropy. Returns 0 only
 * for a complete healthy read, negative on failure, wipes output on failure.
 * Use as explicit DRBG seed callback; do not also call default entropy_func
 * that still requires nonexistent /dev/urandom. Source approval is external. */
int vv_cloud_entropy_read(void *,unsigned char *,size_t);
#endif
