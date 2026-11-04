#ifndef VV_SPEECH_ONLY_CALLBACKS_H
#define VV_SPEECH_ONLY_CALLBACKS_H
#include "vv_native_platform.h"
struct vv_speech_only_callbacks {
 /* Snapshot of an active voice pipeline, independent of hold/track/photo mode.
  * Bounded, thread-safe and no hardware mutation. */
 bool (*preview_alive)(void *);void *preview_arg;
};
/* Config stays alive through successful platform retirement. Does not set TLS,
 * limits, audio capacity, authority or credentials; caller/factory owns those.
 * Business requests always fail closed. */
int vv_speech_only_callbacks_apply(struct vv_native_platform_config *,
 struct vv_speech_only_callbacks *);
/* Real K7 pipeline snapshot; host builds return false. */
bool vv_speech_only_k7_preview_alive(void *);
#endif
