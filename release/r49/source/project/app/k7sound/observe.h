#ifndef AUDIO_PAD_OBSERVE_H
#define AUDIO_PAD_OBSERVE_H
#include <stdint.h>
struct apo_port {
 void *ctx;
 uint64_t (*ticks)(void *); /* monotonic raw CNTVCT, ordered read */
 uint32_t (*ext)(void *); /* GPIO4 EXT_PORT only, no mux/DDR writes */
 uint32_t frequency; /* CNTFRQ, not sample or audio clock rate */
};
struct apo_result {
 uint32_t samples,high[4],changes[4],first,last;
 uint64_t elapsed_ticks,max_gap_ticks;
 int complete;
};
/* 20ms window, <=1000000 samples. GPIO4 A2/A3/A5/B3 bits2/3/5/11.
 * Caller ensures live GPIO4 input path, owner, and clocks started.
 * Returns0 only full window observed; not a clock-rate/audio acceptance. */
int apo_observe(const struct apo_port *,struct apo_result *);
#endif
