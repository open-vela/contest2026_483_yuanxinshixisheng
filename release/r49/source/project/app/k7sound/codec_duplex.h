#ifndef K7_CODEC_DUPLEX_H
#define K7_CODEC_DUPLEX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum kd_state { KD_OFF, KD_CONFIGURING, KD_PREPARED, KD_ACTIVE, KD_FAULT };
enum kd_adc_variant { KD_ADC_BASELINE, KD_ADC_COMMON_NORMAL, KD_ADC_VMID_50K, KD_ADC_PGA24 = 24 };
/* All callbacks serialized and deadline-bounded. 0=success, negative errno=failure.
 * write: combined register/value TX2. read: repeated-start register TX1/RX1.
 * amp(false) must physically disable GPIO2_B1. quiesce must JOIN old DMA/I/O.
 * ready confirms real clocks/slots and, for activation/arm, configured but STOPPED
 * data streams with zero FIFO prefill. CLK/FS may run; TXS/RXS must remain off.
 * After successful arm, caller starts data flow then enables amp and polls.
 * No callback may retain stack pointers or merely enqueue and return success. */
struct kd_port {
 void *arg;
 uint64_t (*now)(void *);
 int (*write)(void *,uint8_t address,uint8_t reg,uint8_t value,uint64_t deadline);
 int (*read)(void *,uint8_t address,uint8_t reg,uint8_t *value,uint64_t deadline);
 int (*delay)(void *,unsigned ms,uint64_t deadline);
 int (*amp)(void *,bool on,uint64_t deadline);
 int (*ready)(void *,unsigned word_bits,uint32_t mclk,uint32_t bclk,bool capture,bool playback,uint64_t deadline);
 int (*quiesce)(void *,uint64_t deadline);
};
struct kd_codec { struct kd_port io; enum kd_state state; uint64_t last; int error; bool busy; unsigned word_bits; bool amp_armed; };
int kd_bind(struct kd_codec *,const struct kd_port *);
/* word_bits=16 or32, stereo slots of the same width, fixed Fs16000/MCLK4096000. */
int kd_prepare(struct kd_codec *,unsigned word_bits,uint64_t deadline);
/* PREPARED only, gain_db is 0/6/12/18/24 relative to default DAC -24dB.
 * amp(false) MUST drive LOW and verify physical GPIO readback before returning0.
 * Does not unmute, enable amp, alter OUT2 or modify default init.
 * Accepted operation failure -> FAULT; failed amp callback cannot prove LOW. */
int kd_set_playback_gain(struct kd_codec *,unsigned gain_db,uint64_t deadline);
/* PREPARED only: DAC0dB and OUT2+4.5dB (max legal code33), no unmute.
 * Same amp(false) physical LOW/readback contract and FAULT policy as gain. */
int kd_set_playback_max(struct kd_codec *,uint64_t deadline);
/* Explicit independent experiments, from KD_OFF only; no automatic retry/ramp.
 * BASELINE=00:36/01:60; COMMON_NORMAL=36/40; VMID_50K=35/60. */
int kd_prepare_adc_variant(struct kd_codec *,unsigned word_bits,enum kd_adc_variant,uint64_t deadline);
int kd_activate(struct kd_codec *,bool capture,bool playback,uint64_t deadline);
/* Prestart PIO hook: unmute +30ms playback wait, amplifier remains LOW. */
int kd_arm(struct kd_codec *,bool capture,bool playback,uint64_t deadline);
/* Poststart PIO hook: only bounded amp GPIO, no I2C or delay. Call before polling.
 * Caller guarantees TX has started and FIFO is prefilled; readback is caller work. */
int kd_enable_amp(struct kd_codec *,uint64_t deadline);
int kd_mute(struct kd_codec *,uint64_t deadline);
/* Keeps clocks available; root may disable clocks only after successful stop. */
int kd_stop(struct kd_codec *,uint64_t deadline);
#endif
