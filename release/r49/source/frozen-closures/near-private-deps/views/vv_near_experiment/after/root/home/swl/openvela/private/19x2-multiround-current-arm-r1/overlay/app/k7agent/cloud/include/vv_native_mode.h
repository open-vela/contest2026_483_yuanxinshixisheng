#ifndef VV_NATIVE_MODE_H
#define VV_NATIVE_MODE_H
#include "device_voice_intent.h"
#include <stdatomic.h>
#include <stdint.h>
struct vv_native_mode_ops {
 int (*request)(void *,unsigned);int (*status)(void *,unsigned);
 void (*halt)(void *);void (*cancel_work)(void *);void (*quiet)(void *);
 void (*sleep_ms)(void *,unsigned);uint64_t (*now_ms)(void *);
 void *arg;
};
struct vv_native_mode {
 atomic_bool busy;atomic_uint epoch;struct vv_native_mode_ops ops;
 unsigned assessment_epoch;bool assessment_active;
};
/* Init does not touch hardware or request tracking; caller starts preview held.
 * Ops bounded, serialized, non-reentrant; cancel_work invalidates pending cloud
 * work/TTS but must not erase RAM photos or automatically retry uploads.
 * quiet cancels only narration. now_ms is monotonic; all callbacks must be
 * bounded, since this module cannot preempt a blocked pipeline callback.
 * Exclusive pipeline ownership is required: never run legacy bridge in parallel.
 * Except invalidate/epoch, calls and assessment fields have one serial owner. */
int vv_native_mode_init(struct vv_native_mode *,const struct vv_native_mode_ops *);
unsigned vv_native_mode_epoch(const struct vv_native_mode *);
/* Thread-safe invalidation only; does not issue hardware requests. Active apply
 * detects it within its bounded poll; idle caller then dispatches STOP. */
void vv_native_mode_invalidate(struct vv_native_mode *);
int vv_native_mode_apply(struct vv_native_mode *,enum k7_voice_intent,unsigned expected_epoch);
/* Release local assessment occupancy only; no motion/report/erase operation.
 * Report completion must separately validate pipeline hold revision before resume. */
int vv_native_mode_assessment_done(struct vv_native_mode *,unsigned expected_epoch);
int vv_native_mode_text(struct vv_native_mode *,const char *,size_t,bool verified,bool completed,unsigned expected_epoch);
/* Actual pipeline wrapper, explicit init only; no motion on init. */
int vv_native_mode_k7_init(struct vv_native_mode *,void (*cancel_work)(void *),void (*quiet)(void *),void *);
#endif
