#ifndef VV_PARAFORMER_H
#define VV_PARAFORMER_H
#include "vv_asr_batch.h"
#include "vv_wss_incremental.h"
#define VV_PF_RX_CAP 32768u
struct vv_pf_protocol {
 char task_id[37],text[VV_ASR_BATCH_TEXT_CAP];
 unsigned state,finals;size_t used,last_offset;
 uint32_t last_begin;bool pending_partial;
 unsigned command_intent;bool command_seen,command_veto,command_conflict;
 bool command_stop,command_quiet;
};
/* States: 0 waiting task-started, 1 audio, 2 finish sent, 3 complete, 4 failed. */
int vv_pf_start(struct vv_pf_protocol *,const uint8_t random[16],char *,size_t,size_t *);
int vv_pf_finish(struct vv_pf_protocol *,char *,size_t,size_t *);
int vv_pf_event(struct vv_pf_protocol *,const void *,size_t);
unsigned vv_pf_command(const struct vv_pf_protocol *);
/* Single capture producer. start begins bounded recording; take returns 1 for
 * exactly 640 bytes, 0 empty, negative error. done=1 success,0 active,<0 error.
 * join(false) acknowledges a stopped, immutable capture window; an adapter may
 * retain its own idle hardware session until its outer operation decides to
 * reuse or close it. join(true) stops/joins the worker before returning.
 * Neither return leaves sample writes in flight. No motion or TTS here. */
struct vv_pf_capture {
 int (*start)(void *);int (*take)(void *,uint8_t pcm[640]);
 int (*done)(void *);int (*join)(void *,bool cancel);void *arg;
 /* Normal bounded end at a whole PCM frame; distinct from cancellation.
  * Producer remains owned until done, drain and join(false). */
 int (*request_finish)(void *);
};
struct vv_pf_run_config {
 struct vv_wsi_ops io; /* verified TLS+Upgrade, ownership transfers to run */
 struct vv_pf_capture capture;
 vv_https_now_ms_fn now;void *clock_arg;
 void (*sleep_ms)(void *,unsigned);void *sleep_arg;
};
/* Owns/cleans connection; no retries. Final text only after task-finished and
 * recording stopped. The K7 outer adapter closes retained hardware before any
 * command result is returned. Per-task 60s, ready15s, TX10s, final20s bounded. */
int vv_pf_run(const struct vv_pf_run_config *,const struct vv_https_cancel *,struct vv_asr_batch_result *);
#endif
