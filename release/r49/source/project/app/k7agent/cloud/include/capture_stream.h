#ifndef K7_CAPTURE_STREAM_H
#define K7_CAPTURE_STREAM_H
#include "pio.h"
/* Dedicated capture task holds the existing codec owner lock. A successful
 * call delivers exactly frames real samples; error/cancel invalidates the
 * utterance. Streaming window is 320..160000 (20 ms..10 s), step 320.
 * Beyond 48000 samples, retained debug storage wraps and cannot be replayed.
 * Codec setup includes its existing settling delay. Connect and await network
 * readiness before starting; UI readiness must account for codec settling. */
int k7sound_capture_stream(unsigned frames, pio_sample_sink sink, void *arg);
/* Settle ADC before the audible cue, then capture under the same owner lock. */
int k7sound_capture_stream_cued(unsigned frames,pio_sample_sink sink,void *arg);
/* No samples/cue before prepared returns success. Callback must be bounded. */
int k7sound_capture_stream_prepared(unsigned frames,pio_sample_sink sink,void *arg,
                                   int (*prepared)(void *),void *prepared_arg);
/* One CPU/thread keeps codec ownership across approved adjacent windows.
 * next runs only after a successful, quiescent RX stop: 1 reopens RX using
 * the settled ADC, 0 closes, negative closes with error. It must be bounded.
 * Only the first window has a cue; each later window still passes prepared.
 * Every exit performs the ordinary codec/platform/I2C cleanup before return. */
int k7sound_capture_stream_session(unsigned frames,pio_sample_sink sink,void *arg,
                                  int (*prepared)(void *),void *prepared_arg,
                                  int (*next)(void *),void *next_arg,
                                  int (*cancelled)(void *),void *cancel_arg);
#endif
