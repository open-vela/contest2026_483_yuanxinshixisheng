/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7_RAW_VAD_OWNER_H
#define K7_RAW_VAD_OWNER_H
#include "echo_diag_runtime.h"
#include <stdint.h>
enum k7_raw_vad_owner_phase {
 K7_RAW_OWNER_EMPTY, K7_RAW_OWNER_PREPARING, K7_RAW_OWNER_READY,
 K7_RAW_OWNER_BORROWED, K7_RAW_OWNER_PROBE, K7_RAW_OWNER_RETAINED
};
struct k7_raw_vad_loan { uint64_t owner,generation; };
struct k7_raw_vad_owner_status {
 unsigned phase; int last_error;
 uint64_t model_generation,loan_generation,voice_owner,borrow_owner;
 uint64_t windows,last_window_end,invalid_reports;
 float max_probability;
};
/* All control calls are nonwaiting admission: -EBUSY retains existing state.
 * Only explicit prepare loads/creates ORT. No automatic prepare from open.
 * Models remain READY after joined close; explicit idle clear retires them.
 */
int k7_raw_vad_owner_prepare(void);
int k7_raw_vad_owner_clear(void);
int k7_raw_vad_owner_status(struct k7_raw_vad_owner_status *);
/* Same native service owner as manager start/stop. Covers KWS creation before
 * audio publication and failed/unknown cleanup, not merely native.running. */
int k7_raw_vad_owner_voice_begin(uint64_t owner);
int k7_raw_vad_owner_voice_end(uint64_t owner);
/* Address of loan and runtime pointer slot stays fixed through joined close.
 * open uses old non-VAD route when EMPTY. Failed VAD open never creates a
 * replacement model; caller must treat its error as diagnostic only. */
int k7_raw_vad_owner_open(struct k7_voice_session *,uint64_t owner,
 struct k7_raw_vad_loan *,struct k7_echo_runtime **);
/* Closes real worker first. On join failure retain loan/model/audio and retry.
 * No public unloan bypass: only successful close clears a nonzero loan. */
int k7_raw_vad_owner_close(struct k7_raw_vad_loan *,struct k7_echo_runtime **);
/* Used only by manual vad-probe under the new config. EMPTY->PROBE excludes
 * prepare and voice start for its whole lifecycle, including failed cleanup. */
int k7_raw_vad_owner_probe_begin(void);
int k7_raw_vad_owner_probe_end(void);
int k7_raw_vad_owner_command(int,char **);
#endif
