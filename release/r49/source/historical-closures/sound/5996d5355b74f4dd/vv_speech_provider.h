#ifndef VV_SPEECH_PROVIDER_H
#define VV_SPEECH_PROVIDER_H
#include "vv_asr_batch.h"
/* Native-client injection boundary, independent of business auth.
 * Borrowed lifetime, one serial owner. ASR returns only fully validated final
 * text; TTS private PCM is 16k mono S16LE and cleared on failure. No retries,
 * motion or playback here. Provider owns API auth; never forwards it to URLs.
 * Native client copies this descriptor; arg and its dependencies are borrowed.
 * Failure never falls back automatically to a different speech endpoint. */
struct vv_reply_scope_token;
struct vv_speech_provider {
 int (*asr)(void *,const unsigned char *,size_t,const struct vv_https_cancel *,struct vv_asr_batch_result *);
 int (*tts)(void *,const char *,unsigned char *,size_t,size_t *,const struct vv_https_cancel *);
 void *arg;
 /* Optional synchronous live capture+streaming ASR. Returns only after capture
  * cleanup/join and final task completion. No batch fallback on failure. */
 int (*listen)(void *,unsigned,const struct vv_https_cancel *,struct vv_asr_batch_result *);
 /* Optional synchronous streaming synthesis AND playback; joins all workers
  * before returning. Cancellation may stop already-started audio. */
 int (*speak)(void *,const char *,const struct vv_https_cancel *);
 /* Optional text-only dialogue; appended for source compatibility. Only a
  * fully validated complete answer is returned, never tool/device commands. */
 int (*chat)(void *,const char *,char *,size_t,const struct vv_https_cancel *);
 /* Optional incremental chat -> synthesis -> playback. Synchronous owner:
  * joins both transports and playback before returning. Audio may start
  * before the authoritative answer is complete. Nonzero clears answer;
  * callers commit conversation history only after complete success. */
 int (*chat_speak)(void *,const char *,char *,size_t,const struct vv_https_cancel *);
 /* Optional explicit diagnostic scope route. The immutable token is borrowed
  * for this synchronous call; all callback users must retire before return.
  * It is never inferred from global last-ASR/TX counters. No KWS/AEC action. */
 int (*chat_speak_scoped)(void *,const char *,char *,size_t,
   const struct vv_https_cancel *,const struct vv_reply_scope_token *);
 /* Optional exact authoritative text -> existing streaming TTS/playback.
  * NULL preserves legacy behavior. A scope token is nullable only for the
  * unscoped path. No LLM, fallback, retry or new hardware owner is introduced. */
 int (*speak_literal)(void *,const char *,char *,size_t,
   const struct vv_https_cancel *,const struct vv_reply_scope_token *);
};
#endif
