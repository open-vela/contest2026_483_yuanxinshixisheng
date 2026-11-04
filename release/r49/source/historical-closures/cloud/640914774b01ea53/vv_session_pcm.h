#ifndef VV_SESSION_PCM_H
#define VV_SESSION_PCM_H
#include "vv_https_client.h"
/* Immutable mono signed-16 little-endian PCM at 16 kHz, <= 30 seconds. Caller
 * retains the buffer/cancel context until this synchronous operation returns.
 * Session playback always uses a fresh TX generation and preserves shared RX;
 * an absent/failed session is an error, never an old-owner fallback. */
int vv_session_pcm_play(const unsigned char *,size_t,const struct vv_https_cancel *);
/* Explicit pre-service bootstrap only, for the cached network-ready prompt.
 * Reject an already published session. A single CPU1 worker acquires/releases
 * the established PIO owner and is joined before returning. External guards
 * run only on the caller; the PIO worker sees an atomic cancellation latch. */
int vv_bootstrap_pcm_play(const unsigned char *,size_t,const struct vv_https_cancel *);
#endif
