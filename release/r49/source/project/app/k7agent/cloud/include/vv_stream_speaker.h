#ifndef VV_STREAM_SPEAKER_H
#define VV_STREAM_SPEAKER_H
#include "vv_https_client.h"
typedef int (*vv_stream_pcm_sink)(void *,const unsigned char *,size_t);
typedef int (*vv_stream_pcm_fetch)(void *,vv_stream_pcm_sink,void *,const struct vv_https_cancel *);
struct vv_stream_fetch_result {
 int http_result;
 bool terminal_audio_complete;
 size_t pcm_bytes;
 bool validated_prefix;
 /* Optional explicit retirement proof from this fetch's own worker tree.
  * Publish only from observed launch/join outcomes, including known no-worker
  * paths. A zero/default result is UNKNOWN, never proof of successful join.
  * Scoped dialogue requires it after invoking fetch; unscoped policy is
  * unchanged. It is independent of rc, terminal/audio success and last access. */
 bool retirement_proof_valid;
 bool nested_workers_joined;
};
typedef int (*vv_stream_pcm_fetch_ex)(void *,vv_stream_pcm_sink,void *,
 const struct vv_https_cancel *,struct vv_stream_fetch_result *);
/* CPU0 fetch, CPU1 playback. 128KiB queue, 512ms initial prebuffer.
 * Starvation drains/stops hardware before <=3 bounded 5s refill waits;
 * resume at 1s queued audio or EOF. No sample replay or silence padding.
 * Total 90s bound; all workers joined before return. */
/* VOICE_SESSION instead feeds the manager's already-running audio owner using
 * a fresh TX generation. No second playback worker or hardware acquisition.
 * 512ms initial buffer; software starvation permits bounded 5s refill waits
 * within the original 90s budget, resuming at 1s buffered or true EOF in the
 * same generation. The owner keeps serial clocks running with separately
 * counted zero filler; source PCM is never replayed. FIFO faults remain errors.
 * Cancel waits for generation cleanup, never stops shared RX/KWS.
 * Manager lifetime must extend through this call and its cleanup. */
/* Only the calling thread polls the external cancellation callback; network
 * and playback workers observe an atomic stop latch, never business guards. */
int vv_stream_speaker_run(vv_stream_pcm_fetch,void *,const struct vv_https_cancel *);
/* Explicit eligibility for a previously armed, one-shot diagnostic capture.
 * Only real cloud answers use this entry; cached/fixed prompts use run().
 * Unarmed, flag-off or diagnostic failure leaves playback behavior unchanged. */
int vv_stream_speaker_run_real_tts(vv_stream_pcm_fetch,void *,const struct vv_https_cancel *);
/* Resident natural answers receive into a bounded 960000-byte append-only
 * buffer while the caller feeds the existing TX ring and polls cancellation.
 * Receiving never waits for playback. The 512ms initial playback buffer and
 * original 90s/480000-sample bounds remain; this is not whole-answer download.
 * This resident batch entry permits a 20s software refill while waiting for
 * a fully verified segment. Prompt and legacy entries retain 5s.
 * The total deadline and cancellation coordinator remain active during gaps.
 * The result is zero-initialized
 * for each fetch; only validated full SSE audio with an HTTP protocol error
 * or explicitly validated transport-truncated prefix may finish its existing
 * generation. Return remains the original error,
 * with cancellation/playback failures taking priority during drain. Legacy
 * playback keeps its original error/cancel policy. No HTTP/TLS rule changes. */
int vv_stream_speaker_run_real_tts_ex(vv_stream_pcm_fetch_ex,void *,const struct vv_https_cancel *);
/* Resident-only continuous natural reply. One fetch may stream all sentence
 * fragments into one TX generation; decoded PCM is published immediately.
 * A fixed 960000-byte SPSC ring permits up to 1440000 cumulative samples.
 * Only the producer waits on a full ring, checking cancellation and the same
 * 90s total deadline. Slots are reusable only after TX has copied them.
 * Start/rebuffer remain 512ms/1s; software refill is bounded by 20s and the
 * total deadline. No replay/retry, no extra audio owner, no source dropping.
 * Only complete, exact fetch success returns zero. Partial/error results
 * cancel remaining queued audio and preserve the original error (external
 * cancellation and playback faults retain priority). Workers are joined
 * before releasing state. A persistent join failure returns an error and
 * quarantines this speaker until restart; it never detaches or reuses that
 * state. Other entries are unchanged. Nonresident returns ENOTSUP. */
int vv_stream_speaker_run_dialogue(vv_stream_pcm_fetch_ex,void *,const struct vv_https_cancel *);
struct vv_reply_scope_token;
/* Explicit scoped natural path. Immutable lease copied before launching any
 * worker; only the true tx_begin generation is bound. Coordinator retires it
 * only after authoritative TX result and successful outer network join plus
 * explicit nested-worker retirement from any invoked fetch. A missing proof
 * is unknown and cannot authorize handoff. This does not prove retirement of
 * the shared audio owner, service, or future external observers. Diagnostic
 * only: no keyword/action/AEC changes. Legacy entries keep original behavior. */
int vv_stream_speaker_run_dialogue_scoped(vv_stream_pcm_fetch_ex,void *,
  const struct vv_https_cancel *,const struct vv_reply_scope_token *);
#endif
