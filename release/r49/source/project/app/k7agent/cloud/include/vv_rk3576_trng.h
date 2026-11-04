#ifndef VV_RK3576_TRNG_H
#define VV_RK3576_TRNG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
/* Candidate only: upstream Linux rockchip-rng.c rk3576_rng_read, with
 * fail-closed deadline/ownership guards. No real MMIO, CRU, reset or security
 * configuration exists here. Offsets are relative to an externally verified
 * RKRNG_NS mapping; raw addresses are never dereferenced by this module.
 * A zero-initialized instance is DISABLED. Single owner; no reentrancy. */
#define VV_RK3576_TRNG_CTRL 0x10u
#define VV_RK3576_TRNG_STATE 0x14u
#define VV_RK3576_TRNG_DATA 0x50u
#define VV_RK3576_TRNG_READY (1u << 4)
#define VV_RK3576_TRNG_MAX_BYTES 1024u
struct vv_rk3576_trng_ops {
  int (*read32)(void *,uint32_t offset,uint32_t *value);
  int (*write32)(void *,uint32_t offset,uint32_t value);
  uint64_t (*now_us)(void *); /* monotonic, UINT64_MAX on failure */
  int (*wait_us)(void *,uint32_t); /* bounded wait, negative on failure */
  bool (*authorized_and_healthy)(void *);
  void *arg;
};
struct vv_rk3576_trng {
  struct vv_rk3576_trng_ops ops;
  uint64_t last_us;
  uint8_t previous[32];
  bool enabled,failed,entered,have_previous;
};
/* Must only be called under an explicit platform source policy: either the
 * verified production clock/reset/access/owner contract, or the separately
 * identified bounded NS development probe policy. The latter makes no claim
 * of cross-world ownership/entropy certification. No admission is implicit.
 * Passing a callback is not certification.
 * Reconfigure is explicit recovery and resets the repetition history. */
int vv_rk3576_trng_configure(struct vv_rk3576_trng *,const struct vv_rk3576_trng_ops *);
void vv_rk3576_trng_disable(struct vv_rk3576_trng *);
/* Suitable for vv_cloud_entropy_config.read_exact, deadline in absolute ms
 * on the SAME monotonic epoch as now_us. Always reads complete 32-byte HW
 * blocks, copies only requested bytes; errors wipe ALL n output bytes and
 * latch disabled, never return a positive short result. Poll <=100us and
 * <=10ms per block, also bounded by the total deadline. Callbacks themselves
 * must be bounded. Ready bit is NOT a hardware health indication.
 * Consecutive identical full blocks are rejected across calls (including
 * partial-output calls). This is only a stuck-output tripwire, not SP800-90B
 * entropy estimation, a complete health test, or approval of a source. */
ssize_t vv_rk3576_trng_read_exact(void *,uint8_t *,size_t,uint64_t deadline_ms);
#endif
