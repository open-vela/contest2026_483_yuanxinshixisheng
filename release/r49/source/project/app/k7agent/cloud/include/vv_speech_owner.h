#ifndef VV_SPEECH_OWNER_H
#define VV_SPEECH_OWNER_H
#include <stdbool.h>
/* Cloud-session exclusion; legacy internal audio locks remain authoritative.
 * Native token must stay alive until release and cannot be NULL. */
int vv_speech_owner_legacy_enter(void);
void vv_speech_owner_legacy_leave(void);
void vv_speech_owner_legacy_poison(void);
int vv_speech_owner_native_claim(const void *token);
void vv_speech_owner_native_release(const void *token);
bool vv_speech_owner_native_available(const void *token);
#endif
