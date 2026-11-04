#ifndef VV_ASR_BATCH_H
#define VV_ASR_BATCH_H
#include "vv_https_client.h"
#define VV_ASR_BATCH_PCM_MAX 96000
#define VV_ASR_BATCH_TEXT_CAP 2049
#ifdef CONFIG_EXAMPLES_K7CLOUD_PARAFORMER_NATURAL_WINDOW
#define VV_ASR_LISTEN_SAMPLES_MAX 160000u
#else
#define VV_ASR_LISTEN_SAMPLES_MAX 48000u
#endif
enum vv_asr_batch_error { VV_ASR_BATCH_INVALID=-3000, VV_ASR_BATCH_MEMORY,
  VV_ASR_BATCH_HTTP, VV_ASR_BATCH_PROTOCOL, VV_ASR_BATCH_MOCK,
  /* Resident capture only: this ASR epoch lost PCM to its own queue overflow.
   * Discard the entire utterance; a new epoch may be admitted. This does not
   * classify generic storage/provider ENOSPC as recoverable. */
  VV_ASR_WINDOW_OVERRUN,
  /* Resident capture observed its physical audio owner become unavailable.
   * Only a failed, undispatched ASR window may request one owner replacement;
   * this never classifies a transport/storage errno as an audio failure. */
  VV_ASR_OWNER_UNAVAILABLE };
struct vv_asr_batch_request {
  const char *host,*path,*boundary_hex; uint16_t port;
  const unsigned char *pcm; size_t pcm_bytes;
  unsigned sample_rate,channels,bits_per_sample;
};
/* Diagnostic identity of the actual successful resident capture. All sample
 * offsets/counts are mono16k input samples, [first_sample,next_sample). A zero
 * valid flag means every field is zero: batch/nonresident, no final command
 * policy, failed/cancelled transport/capture/cleanup or changed audio owner.
 * owner_epoch identifies the pinned physical manager incarnation; asr_epoch
 * is the final capture window, never a speculative/latest manager counter.
 * This is evidence only, not permission to dispatch or retain an audio owner. */
struct vv_asr_capture_identity {
 bool valid;uint64_t owner_epoch;uint32_t asr_epoch;
 uint64_t first_sample,next_sample,received_samples;
};
struct vv_asr_batch_result { char text[VV_ASR_BATCH_TEXT_CAP]; int http_status;
 /* Natural streaming only: resolved final-sentence policy, after task-finished.
  * Never populated by partial events or interpreted as executable text. */
 bool command_finalized;unsigned command_intent;
 struct vv_asr_capture_identity capture_identity;
};
/* Explicit single HTTPS POST; no recording/playback/retry/Bearer or default
 * endpoint. Caller supplies immutable 16kHz mono S16LE PCM, 2..96000 bytes,
 * and collision-free 32 lowercase hex boundary. One multipart audio field.
 * Failure clears result. Empty successful text means silence, not a command.
 * Explicit mock:true anywhere is rejected; absence does not prove real ASR.
 * Default disabled: no startup/live/CMake wiring in this module. */
int vv_asr_batch_perform(struct vv_https_client *,const struct vv_asr_batch_request *,
  const struct vv_https_cancel *,struct vv_asr_batch_result *);
#endif
