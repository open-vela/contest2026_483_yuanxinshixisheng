#ifndef VV_TTS_SEGMENTS_H
#define VV_TTS_SEGMENTS_H
#include <stddef.h>
#define VV_TTS_SEGMENT_SCALARS 1000
#define VV_TTS_SEGMENT_BYTES 4000
enum vv_tts_segment_result { VV_TTS_SEGMENT_DONE=0, VV_TTS_SEGMENT_READY=1,
  VV_TTS_SEGMENT_INVALID=-1, VV_TTS_SEGMENT_EMPTY=-2, VV_TTS_SEGMENT_SPACE=-3 };
struct vv_tts_segments { const char *text; size_t bytes,offset; };
/* Borrowed immutable UTF-8, explicit byte length (excluding trailing NUL).
 * Single owner; no threads/network/playback. Init validates all input and leaves
 * an existing cursor unchanged on failure. NUL/DEL and non-whitespace ASCII
 * control characters are rejected to match TTS JSON transport constraints.
 * Unicode White_Space is stripped only at segment boundaries. Interior ASCII
 * whitespace controls force a boundary, so emitted strings contain no controls.
 * No non-whitespace scalar is dropped, changed or cut. */
int vv_tts_segments_init(struct vv_tts_segments *,const char *,size_t);
/* READY writes a NUL-terminated segment and advances; DONE writes empty output.
 * Failure leaves cursor unchanged, sets written=0 and clears output if possible.
 * Buffer needs actual selected segment bytes+1, not always the maximum size. */
int vv_tts_segments_next(struct vv_tts_segments *,char *,size_t,size_t *written);
/* Same lossless rules with an explicit 1..VV_TTS_SEGMENT_SCALARS cap.
 * Stream report callers use a shorter cap; legacy next retains 1000. */
int vv_tts_segments_next_limit(struct vv_tts_segments *,char *,size_t,size_t *written,size_t max_scalars);
#endif
