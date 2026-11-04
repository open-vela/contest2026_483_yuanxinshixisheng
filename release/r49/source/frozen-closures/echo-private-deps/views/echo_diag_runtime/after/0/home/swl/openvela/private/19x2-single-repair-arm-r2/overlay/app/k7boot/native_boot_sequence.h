#ifndef VV_NATIVE_BOOT_SEQUENCE_H
#define VV_NATIVE_BOOT_SEQUENCE_H
#include <stdbool.h>
#include <stdint.h>
struct vv_native_boot_ops {
 int (*preview_start)(void *); /* one-shot start, no microphone/motion */
 int (*preview_ready)(void *); /* 0 only after initial hold/mode0 applied */
 int (*network_ready)(void *); /* fresh radio + IPv4, EAGAIN while offline */
 int (*prepare_login_bind)(void *); /* one bounded attempt; no recording */
 int (*service_run)(void *); /* synchronous, cancellation-aware */
 void (*stop_hold)(void *); /* stop voice + request mode0; keep preview */
 bool (*cancelled)(void *);
 uint64_t (*now_ms)(void *);
 void (*sleep_ms)(void *,unsigned);
 void *arg;
};
/* One serial owner. No automatic retry on error; caller owns callback deadlines.
 * Before WiFi, only preview runs. Cancellation/failure keeps screen preview.
 * The supplied callbacks must not invoke legacy TCP speech gateways. */
int vv_native_boot_run(const struct vv_native_boot_ops *);
/* Real K7 one-shot camera adapter; worker uses voicepanel (max 3h currently). */
int vv_native_boot_preview_start(void *);
int vv_native_boot_preview_ready(void *);
int vv_native_boot_network_ready(void *);
void vv_native_boot_stop_hold(void *);
#endif
