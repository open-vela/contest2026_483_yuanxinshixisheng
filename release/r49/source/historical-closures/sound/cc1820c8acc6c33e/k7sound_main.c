#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
#include "echo_diag_runtime.h"
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_VAD_PROBE
#include "vad_probe.h"
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
#include "raw_vad_owner.h"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "i2c_owner.inc"
#include <stdlib.h>
#include <unistd.h>
#include "codec_duplex.h"
#include "pio.h"
#include "pcm_playback.h"
#include "platform.h"
#include "observe.h"
#include "reset_probe.h"
#include "start_clear.h"
#include "playback_filter.h"
#include "duplex.h"
#include "mmu_command.h"
#ifdef CONFIG_EXAMPLES_K7SOUND_DUPLEX
#include "duplex_stream.h"
#include <sched.h>
#include <stdatomic.h>
static int duplex_diagnostic(int argc, char **argv);
static atomic_bool duplex_probe_active, duplex_probe_stop;
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION
#include "duplex_session.h"
static int session_diagnostic(int argc, char **argv);
static pthread_mutex_t session_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static struct k7_voice_session *session_probe;
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_AB
#include "session_ab_probe.h"
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
#include "session_capture.h"
#endif
#define BIT(n) (UINT32_C(1) << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#include "input/rockchip_sai.h"
#define SAI ((uintptr_t)0x2a610000)
#define GPIO2 ((uintptr_t)0x2ae20000)
static struct kd_codec codec;
static struct sap_state platform;
static uint32_t capture[96000];
static uint32_t filtered_playback[96000];
static uint32_t loop_capture[512];
static struct dl_result loop_result;
static unsigned captured_frames;
/* Spoken audio only; cues retain their explicit maximum profile. */
static unsigned speech_gain_db = 10;
static unsigned captured_bits;
static bool capture_usable;
static bool playing, amp_ready, data_held, reset_fault;
/* Snapshot only a completed recording; never expose the mutable DMA/PIO buffer. */
int k7sound_copy_mono(float *out, unsigned capacity, unsigned *frames)
{
  if (!out || !frames) return -EINVAL;
  if (pthread_mutex_trylock(&owner)) return -EBUSY;
  int rc = 0;
  if (!capture_usable || !captured_frames || captured_frames > 48000 ||
      bus.poisoned || bus.held || data_held || platform.held || reset_fault)
    rc = -EIO;
  else if (capacity < captured_frames) rc = -ENOSPC;
  else
    {
      for (unsigned i = 0; i < captured_frames; ++i)
        {
          uint32_t word = capture[2 * i];
          int64_t value = word <= INT32_MAX ? (int64_t)word :
                          -1 - (int64_t)(UINT32_MAX - word);
          out[i] = (float)value / 2147483648.0f;
        }
      *frames = captured_frames;
    }
  pthread_mutex_unlock(&owner);
  return rc;
}
static uint32_t old_audio_root, old_audio_gate;
static const uint8_t diagnostic_regs[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0f,0x17,0x18,0x19,0x1a,0x1b,0x27,0x2a,0x2b,0x30,0x31};
static uint8_t diagnostic_values[sizeof(diagnostic_regs)];
static unsigned diagnostic_count, input_high, input_transitions;
static uint32_t input_pull, input_schmitt, input_path;
static uint32_t input_slot_masks[8];
static struct apo_result pad_result;
static uint32_t counter_frequency;
static uint64_t raw_ticks(void *ctx)
{
  uint64_t ticks;
  (void)ctx;
  __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(ticks) :: "memory");
  return ticks;
}
static uint32_t pad_input(void *ctx) { return rd(ctx, GPIO4 + 0x70); }

static int abs_read(void *ctx, uintptr_t addr, uint32_t *value)
{
  *value = rd(ctx, addr);
  return 0;
}
static int abs_write(void *ctx, uintptr_t addr, uint32_t value)
{
  wr(ctx, addr, value);
  return 0;
}
static int sai_read(void *ctx, uint32_t offset, uint32_t *value)
{
  if (!platform.ready || offset > 0x70 || (offset & 3)) return -EINVAL;
  return abs_read(ctx, SAI + offset, value);
}
static int sai_write(void *ctx, uint32_t offset, uint32_t value)
{
  if (!platform.ready || offset > 0x68 || (offset & 3)) return -EINVAL;
  return abs_write(ctx, SAI + offset, value);
}
uint64_t k7sound_clock_us(void) { return now_us(NULL); }
static uint64_t now_ms(void *ctx) { return now_us(ctx) / 1000; }
static int codec_write(void *ctx, uint8_t address, uint8_t reg,
                       uint8_t value, uint64_t deadline)
{
  (void)ctx;
  if (address != 0x10 || now_ms(NULL) >= deadline) return -ETIMEDOUT;
  uint64_t stamp = now_ms(NULL);
  if (stamp >= deadline) return -ETIMEDOUT;
  uint64_t remaining = (deadline - stamp) * 1000;
  if (!remaining) return -ETIMEDOUT;
  int rc = i3_xfer(&bus, false, reg, &value, remaining > 100000 ? 100000 : remaining);
  if (rc) printf("SOUND codec_write reg=%02x error=%d\n", reg, rc);
  return rc;
}
static int codec_read(void *ctx, uint8_t address, uint8_t reg,
                      uint8_t *value, uint64_t deadline)
{
  (void)ctx;
  if (address != 0x10 || now_ms(NULL) >= deadline) return -ETIMEDOUT;
  uint64_t stamp = now_ms(NULL);
  if (stamp >= deadline) return -ETIMEDOUT;
  uint64_t remaining = (deadline - stamp) * 1000;
  if (!remaining) return -ETIMEDOUT;
  int rc = i3_xfer(&bus, true, reg, value, remaining > 100000 ? 100000 : remaining);
  if (rc) printf("SOUND codec_read reg=%02x error=%d\n", reg, rc);
  return rc;
}
static int delay_ms(void *ctx, unsigned ms, uint64_t deadline)
{
  (void)ctx;
  uint64_t end = now_ms(NULL) + ms;
  if (end >= deadline) return -ETIMEDOUT;
  while (now_ms(NULL) < end) usleep(1000);
  return 0;
}
static int amplifier(void *ctx, bool on, uint64_t deadline)
{
  (void)ctx;
  if (!amp_ready || now_ms(NULL) >= deadline) return -EIO;
  masked(GPIO2, 1u << 9, on ? 1u << 9 : 0);
  return (rd(NULL, GPIO2) & (1u << 9)) == (on ? 1u << 9 : 0) ? 0 : -EIO;
}
static int amp_prepare(void)
{
  if (rd(NULL, CRU + 0xa48) & 6) return -EBUSY;
  masked(CRU + 0x848, 2, 0);
  if (rd(NULL, CRU + 0x848) & 2) return -EIO;
  uint32_t version = rd(NULL, GPIO2 + 0x78);
  if (version != 0x01000c2b && version != 0x0101157c && version != 0x010219c8) return -ENODEV;
  /* Program a low output before selecting GPIO2_B1; leave it LOW at exit. */
  masked(GPIO2, 1u << 9, 0);
  masked(GPIO2 + 8, 1u << 9, 1u << 9);
  masked(0x26044048, 0xf0, 0);
  if ((rd(NULL, GPIO2) & 0x200) || !(rd(NULL, GPIO2 + 8) & 0x200) ||
      (rd(NULL, 0x26044048) & 0xf0)) return -EIO;
  amp_ready = true;
  return 0;
}
static int codec_ready(void *ctx, unsigned bits, uint32_t mclk,
                       uint32_t bclk, bool cap, bool play, uint64_t deadline)
{
  (void)ctx;
  if (now_ms(NULL) >= deadline || !platform.ready || (bits != 16 && bits != 32) ||
      mclk != 4096000 || bclk != 32000u * bits ||
      rd(NULL, CRU + 0x330) != 0x00400177 ||
      (rd(NULL, CRU + 0x334) & 3) != 3 ||
      (rd(NULL, CRU + 0x3b8) & 0xfff) != 0x100 ||
      (rd(NULL, 0x26046400) & 2) ||
      (rd(NULL, CRU + 0x820) & 0x70)) return -EIO;
  uint32_t xfer = rd(NULL, SAI + SAI_XFER);
  if (xfer & 0xc) return -EBUSY;
  if (cap || play)
    {
      if ((xfer & 3) != 3) return -EIO;
      /* CKR stores divider-1: 4 for S32 slots, 8 for S16 slots. */
      if (((rd(NULL, SAI + SAI_CKR) >> 3) & 0xfff) !=
          (4096000u / bclk - 1u)) return -EIO;
    }
  return 0;
}
static int quiesce(void *ctx, uint64_t deadline)
{
  (void)ctx;
  if (now_ms(NULL) >= deadline || data_held) return -EBUSY;
  return (rd(NULL, SAI + SAI_XFER) & 0xf) ||
         (rd(NULL, SAI + SAI_DMACR) & (SAI_DMACR_RDE_MASK | SAI_DMACR_TDE_MASK)) ? -EBUSY : 0;
}
static int arm_codec(void *ctx)
{
  (void)ctx;
  int rc = kd_arm(&codec, !playing, playing, now_ms(NULL) + 100);
  diagnostic_count = input_high = input_transitions = 0;
  if (rc) return rc;
  /* Snapshot only before streams start: no I2C or printing in the PIO loop. */
  uint64_t deadline = now_ms(NULL) + 20;
  for (unsigned i = 0; i < sizeof(diagnostic_regs); i++)
    {
      rc = codec_read(NULL, 0x10, diagnostic_regs[i], &diagnostic_values[i], deadline);
      if (rc) return rc;
      diagnostic_count++;
    }
  input_pull = rd(NULL, 0x26046144);
  input_schmitt = rd(NULL, 0x26046244);
  input_path = rd(NULL, SAI + SAI_PATH_SEL);
  for (unsigned i = 0; i < 8; i++) input_slot_masks[i] = rd(NULL, SAI + SAI_TX_SLOT_MASK0 + 4 * i);
  uint64_t frequency;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  if (!frequency || frequency > UINT32_MAX) return -EIO;
  counter_frequency = (uint32_t)frequency;
  struct apo_port observer = {NULL, raw_ticks, pad_input, counter_frequency};
  rc = apo_observe(&observer, &pad_result);
  input_high = pad_result.high[3];
  input_transitions = pad_result.changes[3];
  return rc;
}
static int enable_amp(void *ctx)
{
  (void)ctx;
  return playing ? kd_enable_amp(&codec, now_ms(NULL) + 10) : 0;
}
static int amp_off(void *ctx)
{
  return amplifier(ctx, false, now_ms(NULL) + 10);
}

/* Official SRST_H_SAI1_8CH=134 / M=133: SOFTRST_CON08 bits6/5.
 * The SAI block is idle, clocks running and exclusively owned before use. */
static int set_sai_reset(void *ctx, enum sr_domain domain, int asserted)
{
  (void)ctx;
  uint32_t bit = domain == SR_H ? 0x40u : 0x20u;
  masked(CRU + 0xa20, bit, asserted ? bit : 0);
  return 0;
}
static int get_sai_reset(void *ctx, enum sr_domain domain, int *asserted)
{
  (void)ctx;
  *asserted = !!(rd(NULL, CRU + 0xa20) & (domain == SR_H ? 0x40u : 0x20u));
  return 0;
}

static int sound_run(int argc, char **argv, const uint32_t *speech32,
                     const int16_t *speech16,
                     unsigned speech_frames, unsigned listen_frames,
                     pio_sample_sink sink, void *sink_arg,
                     int (*cancelled)(void *), void *cancel_arg,
                     int (*prepared)(void *), void *prepared_arg,
                     pio_sample_source pull,void *pull_arg,
                     int (*next)(void *),void *next_arg)
{
  const bool speech = speech32 || speech16 || pull;
  if (argc > 1 && !strcmp(argv[1], "mmu"))
    return k7sound_mmu_command(argc, argv, &owner);
  if (argc < 2 || argc > 3) { puts("usage: k7sound probe|capture|capture-pga24|capture-pga24-16|capture-pga24-rde|capture-pga24-rx4|capture-pga24-rx4-all|capture-pga24-mono|capture-pga24-grouped|capture-pause250 [frames]|dump|tone [gain_db]|replay [gain_db]; gain_db=0,6,12,18,24"); return 1; }
  if (pthread_mutex_trylock(&owner)) return 1;
  if (!strcmp(argv[1], "dump"))
    {
      printf("SOUND_PCM frames=%u rate=16000 channels=2 bits=%u\n", captured_frames, captured_bits);
      for (unsigned i = 0; i < captured_frames * 2; i += 8)
        {
          printf("PCM %06x", i);
          for (unsigned j = i; j < i + 8 && j < captured_frames * 2; j++) printf(" %08" PRIx32, capture[j]);
          putchar('\n');
          /* Pace diagnostics so the USB serial adapter can drain each row. */
          usleep(1000);
        }
      puts("SOUND_PCM_END");
      pthread_mutex_unlock(&owner);
      return 0;
    }
  bool guarded = !strcmp(argv[1], "loopback-guard");
  bool rx2 = guarded || !strcmp(argv[1], "loopback-rx2");
  bool numbered = !strcmp(argv[1], "loopback-numbered");
  bool route_all = !strcmp(argv[1], "loopback-rxall");
  bool loopback = rx2 || numbered || route_all || !strcmp(argv[1], "loopback");
  bool probe = !strcmp(argv[1], "probe");
  bool ma4 = !strcmp(argv[1], "replay-ma4-max");
  bool hp80 = !strcmp(argv[1], "replay-hp-max");
  bool cued = listen_frames || !strcmp(argv[1], "capture-cued");
  bool maximum = (speech16 && !cued) || (!speech && cued) || ma4 || hp80 ||
                 !strcmp(argv[1], "replay-max") ||
                 !strcmp(argv[1], "tone-max");
  bool replay = ma4 || hp80 || !strcmp(argv[1], "replay") || !strcmp(argv[1], "replay-max");
  bool clockwait = !strcmp(argv[1], "capture-clockwait") || !strcmp(argv[1], "tone-clockwait");
  bool clearing = !strcmp(argv[1], "capture-clear") || !strcmp(argv[1], "tone-clear");
  bool resetting = !strcmp(argv[1], "capture-reset") || !strcmp(argv[1], "tone-reset");
  playing = (speech && !cued) || !strcmp(argv[1], "tone") || !strcmp(argv[1], "tone-max") || !strcmp(argv[1], "tone-reset") || !strcmp(argv[1], "tone-clear") || !strcmp(argv[1], "tone-clockwait") || replay;
  bool common = !strcmp(argv[1], "capture-common");
  bool pause250 = !strcmp(argv[1], "capture-pause250");
  bool sample16 = !strcmp(argv[1], "capture-pga24-16");
  bool dma_request = !strcmp(argv[1], "capture-pga24-rde");
  bool rx4 = !strcmp(argv[1], "capture-pga24-rx4");
  bool rx4_all = !strcmp(argv[1], "capture-pga24-rx4-all");
  bool mono = !strcmp(argv[1], "capture-pga24-mono");
  bool grouped = cued || !strcmp(argv[1], "capture-pga24-grouped");
  bool pga24 = pause250 || sample16 || dma_request || rx4 || rx4_all || mono || grouped || !strcmp(argv[1], "capture-pga24");
  bool vmid = !strcmp(argv[1], "capture-vmid");
  bool recording = !strcmp(argv[1], "capture") || !strcmp(argv[1], "capture-reset") || !strcmp(argv[1], "capture-clear") || !strcmp(argv[1], "capture-clockwait") || common || vmid || pga24;
  if (recording) { captured_frames = 0; captured_bits = 0; capture_usable = false; }
  unsigned frames = pause250 ? 128 : 3200;
  if (speech) frames = listen_frames ? listen_frames : speech_frames;
  /* Spoken volume is separate from the maximum audible capture cue. */
  unsigned gain_db = speech ? (cued ? 24 : speech_gain_db) : (maximum ? 24 : 0);
  if (speech && !cued) maximum = gain_db == 24;
  if (replay)
    {
      if (!capture_usable || !captured_frames || captured_frames > 48000 || captured_bits != 32) goto invalid;
      frames = captured_frames;
    }
  if (argc == 3)
    {
      if (pause250) goto invalid;
      if (maximum && !cued) goto invalid;
      char *end;
      unsigned long count = strtoul(argv[2], &end, 10);
      if (end == argv[2] || *end) goto invalid;
      if (recording)
        {
          if (count < 1 || count > (sink ? PIO_STREAM_FRAMES : PIO_FRAMES)) goto invalid;
          frames = (unsigned)count;
        }
      else if (replay || !strcmp(argv[1], "tone"))
        {
          if (count > 24 || (!replay && count % 6)) goto invalid;
          gain_db = (unsigned)count;
        }
      else goto invalid;
    }
  if ((!probe && !playing && !recording && !loopback) || bus.poisoned || bus.held || data_held || platform.held || reset_fault) goto invalid;
  const uint32_t *playback_source = capture;
  if (replay && !ma4 && !hp80)
    {
      /* Match ASR's left channel and mono TTS routing. Keep capture intact. */
      for (unsigned i = 0; i < captured_frames; i++)
        filtered_playback[2 * i] = filtered_playback[2 * i + 1] = capture[2 * i];
      playback_source = filtered_playback;
    }
  if (ma4 || hp80)
    {
      uint32_t saturations = 0;
      int filter_rc = af_filter(capture, 96000, filtered_playback, 96000,
                                frames, hp80 ? AF_MA4_HP80 : AF_MA4,
                                &saturations);
      printf("SOUND filter=%u result=%d frames=%u saturations=%u raw_preserved=1\n",
             hp80 ? 2u : 1u, filter_rc, frames, saturations);
      if (filter_rc) goto invalid;
      playback_source = filtered_playback;
    }
  struct saved_i2c old = {0};
  struct sap_port sap = {NULL, abs_read, abs_write, now_us};
  struct pio_result result = {0};
  struct kd_port kd = {NULL, now_ms, codec_write, codec_read, delay_ms, amplifier, codec_ready, quiesce};
  memset(&bus, 0, sizeof(bus));
  memset(&platform, 0, sizeof(platform));
  int rc = prepare_i2c(&old);
  if (!rc) rc = amp_prepare();
  old_audio_root = rd(NULL, CRU + 0x3a8);
  old_audio_gate = rd(NULL, CRU + 0x81c);
  bool clock_changed = false;
  if (!rc)
    {
      if (rd(NULL, CRU + 0xa20) & 0x60) rc = -EBUSY;
      else
        {
          /* All audio users are owned here. Choose HCLK xin24m, no PLL writes. */
          masked(CRU + 0x3a8, 3, 3);
          masked(CRU + 0x81c, 2, 0);
          clock_changed = true;
          if ((rd(NULL, CRU + 0x3a8) & 3) != 3 || (rd(NULL, CRU + 0x81c) & 2)) rc = -EIO;
        }
    }
  up_disable_irq(220);
  if (!rc) rc = sap_setup(&sap, &platform, 1);
  printf("SOUND platform_result=%d version=%08" PRIx32 " repair=%08" PRIx32
         " ack=%08" PRIx32 " idle=%08" PRIx32 "\n", rc, platform.version, platform.repair, platform.ack, platform.idle);
  printf("SOUND mclkout before=%08" PRIx32 " after=%08" PRIx32 "\n", platform.mclkout_before, platform.mclkout_after);
  if (!rc && clearing)
    {
      struct sc_port port = {NULL, sai_read, sai_write, now_us};
      struct sc_result state = {0};
      rc = sc_once(&port, &state, 1);
      reset_fault = state.held;
      if (reset_fault) data_held = true;
      printf("SOUND start_clear result=%d completed=%u held=%u reapply=%u us=%" PRIu64 " polls=%u\n", rc, state.completed, state.held, state.needs_reapply, state.end_us - state.start_us, state.polls);
      printf("SOUND clear_fifo tx_before=%08" PRIx32 " rx_before=%08" PRIx32 " tx_after=%08" PRIx32 " rx_after=%08" PRIx32 "\n", state.fifo_before[0], state.fifo_before[1], state.fifo_after[0], state.fifo_after[1]);
    }
  if (!rc && resetting)
    {
      struct sr_port port = {NULL, sai_read, set_sai_reset, get_sai_reset, now_us};
      struct sr_result state = {0};
      rc = sr_once(&port, &state, 1);
      reset_fault = state.held;
      if (reset_fault) data_held = true;
      printf("SOUND reset result=%d steps=%u completed=%u held=%u reapply=%u\n", rc, state.steps, state.completed, state.held, state.needs_reapply);
      for (unsigned i = 0; i < 5; i++)
        printf("SOUND reset_reg index=%u before=%08" PRIx32 " after=%08" PRIx32 "\n", i, state.before[i], state.after[i]);
      /* pio_run applies the same explicit profile after this isolated reset. */
    }
  if (!rc) rc = kd_bind(&codec, &kd);
  enum kd_adc_variant variant = pga24 ? KD_ADC_PGA24 : common ? KD_ADC_COMMON_NORMAL : (vmid ? KD_ADC_VMID_50K : KD_ADC_BASELINE);
  if (!rc) rc = kd_prepare_adc_variant(&codec, sample16 ? 16 : 32, variant, now_ms(NULL) + 3000);
  /* Speech and replay use the same analog path, without PCM scaling. */
  if (!rc && ((speech && !cued) || replay))
    {
      /* Replay and speech share OUT2 settings; gain only changes DAC attenuation. */
      rc = kd_set_playback_max(&codec, now_ms(NULL) + 100);
      if (!rc && gain_db < 24) rc = kd_set_playback_gain(&codec, gain_db, now_ms(NULL) + 100);
    }
  else if (!rc && maximum) rc = kd_set_playback_max(&codec, now_ms(NULL) + 100);
  else if (!rc && gain_db) rc = kd_set_playback_gain(&codec, gain_db, now_ms(NULL) + 100);
  unsigned window = 0;
  if (!rc && recording)
    {
      /* Retain the full first-use analog settle, but allow bounded stop. */
      for (unsigned settled = 0; !rc && settled < 1000; settled += 10)
        {
          if (cancelled && cancelled(cancel_arg)) rc = -ECANCELED;
          else rc = delay_ms(NULL, 10, now_ms(NULL) + 20);
        }
    }
stream_again:
  if (!rc && recording && prepared) rc = prepared(prepared_arg);
  printf("SOUND codec_prepare=%d state=%u\n", rc, codec.state);
  printf("SOUND adc_variant=%u settle_ms=%u\n", (unsigned)variant, recording && !window ? 1000 : 0);
  printf("SOUND playback_gain_db=%u dac_attenuation_db=%u maximum=%u\n", gain_db, 24 - gain_db, maximum);
  if (!rc && loopback)
    {
      /* VERSION/CKR/FSCR/PATH are checked by dl_run for this shared-clock
       * SAI1 profile. Codec stays PREPARED/muted. No amp-enable callback. */
      struct dl_port dl = {NULL, sai_read, sai_write, now_us, amp_off};
      if (guarded)
        rc = dl_run_numbered_rx2_guard(&dl, platform.ready, 1, 4096000, loop_capture, 512, &loop_result);
      else if (rx2)
        rc = dl_run_numbered_rx2(&dl, platform.ready, 1, 4096000, loop_capture, 512, &loop_result);
      else if (numbered)
        rc = dl_run_numbered(&dl, platform.ready, 1, 4096000, loop_capture, 512, &loop_result);
      else
        rc = dl_run_route(&dl, platform.ready, 1, 4096000, loop_capture, 512,
                  128, route_all ? DL_ROUTE_RX_ALL_SDI0 : DL_ROUTE_DEFAULT, &loop_result);
      data_held = loop_result.held;
      if (rx2) printf("AUDIO RXCSR before=%08x active=%08x restored=%08x restore_rc=%d\n", loop_result.rxcr_before, loop_result.rxcr_active, loop_result.rxcr_restored, loop_result.rx_restore_rc);
      printf("SOUND loopback result=%d frames=%u tx_queued=%u polls=%u stop=%d restore=%d amp=%d held=%d\n",
             rc, loop_result.frames, loop_result.tx_words, loop_result.polls,
             loop_result.stop_rc, loop_result.restore_rc, loop_result.amp_rc,
             loop_result.held);
      printf("SOUND loop_path before=%08" PRIx32 " active=%08" PRIx32
             " restored=%08" PRIx32 " start=%" PRIu64 " end=%" PRIu64 "\n",
             loop_result.path_before, loop_result.path_active,
             loop_result.path_restored, loop_result.start_us, loop_result.end_us);
      for (unsigned direction = 0; direction < 2; direction++)
        {
          unsigned count = direction ? loop_result.nrx : loop_result.ntx;
          const struct dl_word *words = direction ? loop_result.rx : loop_result.tx;
          for (unsigned i = 0; i < count; i++)
            printf("SOUND loop_word dir=%u index=%u us=%" PRIu64
                   " before=%08" PRIx32 " word=%08" PRIx32
                   " after=%08" PRIx32 " rc=%d\n", direction, i,
                   words[i].us, words[i].before, words[i].word,
                   words[i].after, words[i].rc);
        }
      for (unsigned i = 0; i < loop_result.frames * 2; i += 8)
        {
          printf("LOOP_PCM %04x", i);
          for (unsigned j = i; j < i + 8 && j < loop_result.frames * 2; j++)
            printf(" %08" PRIx32, loop_capture[j]);
          putchar('\n');
        }
    }
  else if (!rc && !probe)
    {
      struct pio_port pio = {NULL, sai_read, sai_write, now_us, arm_codec, enable_amp, amp_off, 1u << 27,
                             cancelled, cancel_arg};
      /* Prepare and settle the ADC before the audible cue. Keep ownership and
       * codec state across TX -> RX: no reset, serial logging, or one-second
       * settling delay between the cue and the capture window. */
      struct pio_result cue_result = {0};
      uint64_t cue_end_us = 0;
      if (cued)
        {
          playing = true;
          rc = speech16 ? pio_play_mono16(&pio, 1, 4096000, speech16,
                     speech_frames, speech_frames, &cue_result) :
               speech ? pio_play_buffer(&pio, 1, 4096000, speech32,
                     2 * speech_frames, speech_frames, &cue_result) :
                     pio_run(&pio, 1, 4096000, true, capture, 96000, 3200, &cue_result);
          playing = false;
          cue_end_us = now_us(NULL);
          data_held = cue_result.held;
          if (!rc && (cue_result.held || cue_result.stop_result ||
                      cue_result.frames != (speech ? speech_frames : 3200))) rc = -EIO;
        }
      if (!rc) rc = pull ? pio_play_stream16(&pio,1,4096000,pull,pull_arg,&result) :
                    speech16 && !cued ? pio_play_mono16(&pio, 1, 4096000, speech16, speech_frames, frames, &result) :
                    speech && !cued ? pio_play_buffer(&pio, 1, 4096000, speech32, 2 * speech_frames, frames, &result) :
                    sample16 ? pio_run16(&pio, 1, 4096000, capture, 96000, frames, &result) :
                    dma_request ? pio_run_rde(&pio, 1, 4096000, capture, 96000, frames, &result) :
                    rx4_all ? pio_run_rx4_all(&pio, 1, 4096000, capture, 96000, frames, &result) :
                    rx4 ? pio_run_rx4(&pio, 1, 4096000, capture, 96000, frames, &result) :
                    mono ? pio_run_mono(&pio, 1, 4096000, capture, 96000, frames, &result) :
                    grouped && sink ? pio_run_grouped_sink(&pio, 1, 4096000, capture, 96000, frames, &result, sink, sink_arg) :
                    grouped ? pio_run_grouped(&pio, 1, 4096000, capture, 96000, frames, &result) :
                    pause250 ? pio_run_pause250(&pio, 1, 4096000, capture, 96000, &result) :
                    replay ? pio_play_buffer(&pio, 1, 4096000, playback_source, 2 * captured_frames, frames, &result) :
                    clockwait ? pio_run_clockwait(&pio, 1, 4096000, playing, capture, 96000, frames, &result) :
                    pio_run(&pio, 1, 4096000, playing, capture, 96000, frames, &result);
      printf("SOUND timing direction=%s stream_start_us=%" PRIu64 " result=%d\n",
             (speech && !cued) || replay || playing ? "tx" : "rx", result.stream_start_us, rc);
      printf("SOUND experiment clear=%u reset=%u clockwait20=%u\n", clearing, resetting, clockwait);
      for (unsigned i = 0; i < 8; i++) printf("SOUND slot_mask offset=%02x value=%08" PRIx32 "\n", SAI_TX_SLOT_MASK0 + 4 * i, input_slot_masks[i]);
      data_held = data_held || result.held;
      if (cued) printf("SOUND cue frames=%u stop=%d held=%d end_us=%" PRIu64
                       " capture_start_us=%" PRIu64 "\n", cue_result.frames,
                       cue_result.stop_result, cue_result.held, cue_end_us,
                       result.rx_trace.start_us);
      if (pause250) printf("SOUND pause250 start=%" PRIu64 " end=%" PRIu64 " before=%08" PRIx32 " after=%08" PRIx32 "\n", result.pause_start_us, result.pause_end_us, result.pause_before, result.pause_after);
      if (recording) { captured_frames = result.frames > PIO_FRAMES ? 0 : result.frames; captured_bits = sample16 ? 16 : 32; }
      printf("SOUND pio=%d frames=%u polls=%u stop=%d held=%d max_fifo=%u\n",
             rc, result.frames, result.polls, result.stop_result, result.held, result.max_fifo);
      if (dma_request) printf("SOUND dma_request active=%08" PRIx32 " restore=%d\n",
                              result.rx_trace.init[4], result.dma_restore_result);
      if (mono) printf("SOUND mono active=%08" PRIx32 " restore=%d\n",
                       result.rx_trace.init[0], result.mono_restore_result);
      if (grouped) printf("SOUND grouped words=%u mismatches=%u\n",
                          result.grouped_words, result.grouped_mismatches);
      printf("SOUND fifo first=%08" PRIx32 " last=%08" PRIx32 " TXCR=%08" PRIx32 " RXCR=%08" PRIx32 " FSCR=%08" PRIx32 " CKR=%08" PRIx32 "\n",
             result.first_fifo_raw, result.last_fifo_raw, rd(NULL, SAI + SAI_TXCR),
             rd(NULL, SAI + SAI_RXCR), rd(NULL, SAI + SAI_FSCR), rd(NULL, SAI + SAI_CKR));
      if (recording)
        {
          const struct pio_rx_trace *t = &result.rx_trace;
          printf("SOUND trace start=%" PRIu64 " end=%" PRIu64 " started=%u count=%u\n", t->start_us, t->end_us, t->window_started, t->count);
          for (unsigned i = 0; i < 6; i++)
            printf("SOUND trace_init index=%u value=%08" PRIx32 " rc=%d\n", i, t->init[i], t->init_rc[i]);
          for (unsigned i = 0; i < t->count; i++)
            {
              const struct pio_rx_trace_entry *e = &t->entry[i];
              printf("SOUND trace_word index=%u us=%" PRIu64 " before=%08" PRIx32 " word=%08" PRIx32 " after=%08" PRIx32 " rc=%d,%d,%d\n", i, e->time_us, e->fifo_before, e->word, e->fifo_after, e->before_rc, e->word_rc, e->after_rc);
            }
        }
      for (unsigned i = 0; i < diagnostic_count; i++)
        printf("SOUND codec reg=%02x value=%02x\n", diagnostic_regs[i], diagnostic_values[i]);
      printf("SOUND input pull=%08" PRIx32 " schmitt=%08" PRIx32 " path=%08" PRIx32
             " gpio_high=%u transitions=%u samples=%u\n", input_pull, input_schmitt, input_path,
             input_high, input_transitions, pad_result.samples);
      printf("SOUND pad ticks=%" PRIu64 " max_gap=%" PRIu64 " counter_hz=%" PRIu32 " complete=%d\n",
             pad_result.elapsed_ticks, pad_result.max_gap_ticks, counter_frequency, pad_result.complete);
      for (unsigned i = 0; i < 4; i++)
        printf("SOUND pad channel=%u high=%" PRIu32 " changes=%" PRIu32 "\n", i, pad_result.high[i], pad_result.changes[i]);
      if (recording && captured_frames)
        {
          struct pio_stats stats;
          pio_summarize(capture, captured_frames, &stats);
          capture_usable = !pause250 && !rc && captured_frames == frames && (stats.nonzero[0] || stats.nonzero[1]);
          printf("SOUND pcm left_min=%" PRId32 " left_max=%" PRId32 " left_nonzero=%u "
                 "right_min=%" PRId32 " right_max=%" PRId32 " right_nonzero=%u\n",
                 stats.min[0], stats.max[0], stats.nonzero[0], stats.min[1], stats.max[1], stats.nonzero[1]);
        }
    }
  if (!rc && next)
    {
      /* PIO has stopped/cleared SAI and forced the amplifier low. Keep ADC
       * bias and exclusive ownership only while the caller approves reuse.
       * platform.held is the normal lease set by sap_setup and retained until
       * sap_cleanup; platform.ready distinguishes successful preparation. */
      if (!recording || !grouped || !sink || result.held || result.stop_result ||
          data_held || reset_fault || bus.poisoned || bus.held ||
          !platform.ready || codec.state != KD_ACTIVE) rc = -EIO;
      else
        {
          int again = next(next_arg);
          if (again > 0)
            {
              memset(&result, 0, sizeof(result));
              captured_frames = 0; capture_usable = false;
              cued = false; window++;
              goto stream_again;
            }
          if (again < 0) rc = again;
        }
    }
  int stop_rc = (reset_fault || data_held) ? -EBUSY : platform.ready ? kd_stop(&codec, now_ms(NULL) + 1500) : 0;
  if (amp_ready) amp_off(NULL);
  up_udelay(10);
  /* A failed/unknown stop must not fall through to restoring gated clocks.
   * Retain the fault until an explicit reset; amplifier force-low remains safe. */
  if (stop_rc || data_held || reset_fault) platform.held = true;
  int platform_rc = platform.ready && !stop_rc && !data_held ? sap_cleanup(&sap, &platform, !quiesce(NULL, now_ms(NULL) + 10)) : (platform.held ? -EBUSY : 0);
  if (clock_changed && !platform.held)
    {
      masked(CRU + 0x3a8, 3, old_audio_root);
      masked(CRU + 0x81c, 2, old_audio_gate);
      if ((rd(NULL, CRU + 0x3a8) & 3) != (old_audio_root & 3) ||
          (rd(NULL, CRU + 0x81c) & 2) != (old_audio_gate & 2)) platform_rc = -EIO;
    }
  int bus_rc = !platform.held ? restore_i2c(&old) : -EBUSY;
  if (bus_rc) bus.poisoned = true;
  printf("SOUND result=%d codec_stop=%d platform_restore=%d i2c_restore=%d amp_low=%u\n",
         rc, stop_rc, platform_rc, bus_rc, amp_ready && !(rd(NULL, GPIO2) & 0x200));
  pthread_mutex_unlock(&owner);
  if (rc == -ECANCELED && !stop_rc && !platform_rc && !bus_rc) return -ECANCELED;
  return rc || stop_rc || platform_rc || bus_rc ? 1 : 0;
invalid:
  puts("SOUND invalid_request_or_latched_fault");
  pthread_mutex_unlock(&owner);
  return 1;
}

int main(int argc, char **argv)
{
#ifdef CONFIG_EXAMPLES_K7SOUND_RAW_VAD_OWNER
  if(argc>=2&&!strncmp(argv[1],"vad-raw-",8))return k7_raw_vad_owner_command(argc,argv);
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_VAD_PROBE
  if(argc>=2&&!strcmp(argv[1],"vad-probe"))return k7_vad_probe_command(argc,argv);
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_ECHO_RUNTIME
  if(argc>=2&&!strncmp(argv[1],"echo-diag-",10))return k7_echo_runtime_command(argc,argv);
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_CAPTURE
  if(argc>=2&&!strncmp(argv[1],"session-capture-",16))return k7_session_capture_command(argc,argv);
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_AB
  if (argc >= 2 && !strncmp(argv[1], "session-ab", 10)) return k7_session_ab_command(argc, argv);
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION
  if (argc == 2 && !strcmp(argv[1], "session-stop")) {
    pthread_mutex_lock(&session_probe_lock);
    if (session_probe) k7_voice_session_stop(session_probe);
    printf("SESSION stop_requested=1 active=%u\n", session_probe != NULL);
    pthread_mutex_unlock(&session_probe_lock); return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "session-probe")) return session_diagnostic(argc, argv);
#endif
#ifdef CONFIG_EXAMPLES_K7SOUND_DUPLEX
  if (argc == 2 && !strcmp(argv[1], "duplex-stop")) {
    atomic_store(&duplex_probe_stop, true);
    printf("DUPLEX stop_requested=1 active=%u\n", (unsigned)atomic_load(&duplex_probe_active));
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "duplex-probe"))
    return duplex_diagnostic(argc, argv);
#endif

  if ((argc == 2 || argc == 3) && !strcmp(argv[1], "speech-volume"))
    {
      unsigned long gain = 0;
      if (argc == 3)
        {
          char *end;errno = 0;gain = strtoul(argv[2], &end, 10);
          if (errno || !argv[2][0] || *end || gain > 24) return 1;
        }
      if (pthread_mutex_trylock(&owner)) return 1;
      if (argc == 3) speech_gain_db = (unsigned)gain;
      printf("SOUND speech_gain_db=%u cue_gain_db=24 next_playback=1\n", speech_gain_db);
      pthread_mutex_unlock(&owner);
      return 0;
    }
  return sound_run(argc, argv, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

int k7sound_speak_pcm(const uint32_t *pcm, unsigned frames,
                      unsigned capture_frames)
{
  if (!pcm || !frames || frames > PIO_PLAYBACK_FRAMES ||
      capture_frames > PIO_FRAMES) return -EINVAL;
  char *argv[] = {(char *)"k7sound", (char *)"speech-internal"};
  return sound_run(2, argv, pcm, NULL, frames, capture_frames, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL) ? -EIO : 0;
}

int k7sound_speak_mono16(const int16_t *pcm, unsigned frames,
                         unsigned capture_frames)
{
  if (!pcm || !frames || frames > PIO_PLAYBACK_FRAMES ||
      capture_frames > PIO_FRAMES) return -EINVAL;
  char *argv[] = {(char *)"k7sound", (char *)"speech-internal"};
  return sound_run(2, argv, NULL, pcm, frames, capture_frames, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL) ? -EIO : 0;
}

int k7sound_speak_mono16_cancel(const int16_t *pcm, unsigned frames,
                               int (*cancelled)(void *), void *arg)
{
  if (!pcm || !frames || frames > PIO_PLAYBACK_FRAMES || !cancelled) return -EINVAL;
  char *argv[] = {(char *)"k7sound", (char *)"speech-internal"};
  int rc = sound_run(2, argv, NULL, pcm, frames, 0, NULL, NULL, cancelled, arg, NULL, NULL, NULL, NULL, NULL, NULL);
  return rc == -ECANCELED ? rc : rc ? -EIO : 0;
}

int k7sound_capture_grouped(unsigned frames)
{
  if (frames < 1 || frames > 48000) return -EINVAL;
  char count[16];
  int length = snprintf(count, sizeof(count), "%u", frames);
  if (length <= 0 || (size_t)length >= sizeof(count)) return -EOVERFLOW;
  char *argv[] = {(char *)"k7sound", (char *)"capture-pga24-grouped", count};
  return main(3, argv) ? -EIO : 0;
}

int k7sound_play_prompt_tone(void)
{
  char *argv[] = {(char *)"k7sound", (char *)"tone-max"};
  return main(2, argv) ? -EIO : 0;
}

static int capture_stream(unsigned frames, pio_sample_sink sink, void *arg,int cued)
{
  char count[16];
  char *argv[] = {(char *)"k7sound", (char *)(cued?"capture-cued":"capture-pga24-grouped"), count};
  if (!sink || frames < 320 || frames > PIO_STREAM_FRAMES || frames % 320)
    return -EINVAL;
  snprintf(count, sizeof(count), "%u", frames);
  return sound_run(3, argv, NULL, NULL, 0, 0, sink, arg, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL) ? -EIO : 0;
}

int k7sound_capture_stream(unsigned frames,pio_sample_sink sink,void *arg)
{return capture_stream(frames,sink,arg,0);}
int k7sound_capture_stream_cued(unsigned frames,pio_sample_sink sink,void *arg)
{return capture_stream(frames,sink,arg,1);}

/* The gate runs after ADC settling, before cue/PCM, under codec ownership.
 * Gate failure follows the ordinary sound_run cleanup path. */
int k7sound_capture_stream_prepared(unsigned frames,pio_sample_sink sink,void *arg,
                                   int (*prepared)(void *),void *prepared_arg)
{
  char count[16];
  char *argv[]={(char *)"k7sound",(char *)"capture-cued",count};
  if(!sink||!prepared||frames<320||frames>PIO_STREAM_FRAMES||frames%320)return -EINVAL;
  snprintf(count,sizeof(count),"%u",frames);
  int rc=sound_run(3,argv,NULL,NULL,0,0,sink,arg,NULL,NULL,prepared,prepared_arg, NULL, NULL, NULL, NULL);
  return rc==-ECANCELED?rc:rc?-EIO:0;
}

int k7sound_speak_stream16(k7_pcm16_pull pull,void *arg,
                         int (*cancelled)(void *),void *cancel_arg)
{
  if(!pull||!cancelled)return -EINVAL;
  char *argv[]={(char *)"k7sound",(char *)"speech-internal"};
  int rc=sound_run(2,argv,NULL,NULL,PIO_PLAYBACK_FRAMES,0,NULL,NULL,
                   cancelled,cancel_arg,NULL,NULL,pull,arg,NULL,NULL);
  return rc==-ECANCELED?rc:rc?-EIO:0;
}

int k7sound_capture_stream_session(unsigned frames,pio_sample_sink sink,void *arg,
                                  int (*prepared)(void *),void *prepared_arg,
                                  int (*next)(void *),void *next_arg,
                                  int (*cancelled)(void *),void *cancel_arg)
{
  char count[16];
  char *argv[]={(char *)"k7sound",(char *)"capture-cued",count};
  if(!sink||!prepared||!next||!cancelled||frames<320||
     frames>PIO_STREAM_FRAMES||frames%320)return -EINVAL;
  snprintf(count,sizeof(count),"%u",frames);
  int rc=sound_run(3,argv,NULL,NULL,0,0,sink,arg,cancelled,cancel_arg,
                   prepared,prepared_arg,NULL,NULL,next,next_arg);
  return rc==-ECANCELED?rc:rc?-EIO:0;
}

#ifdef CONFIG_EXAMPLES_K7SOUND_DUPLEX
static int duplex_arm(void *ctx)
{
  (void)ctx;
  /* Both converter paths are armed before either stream is started. */
  int rc = kd_arm(&codec, true, true, now_ms(NULL) + 100);
#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION_AB
  if (!rc && k7_session_ab_active()) {
    /* Diagnostic reads before RXS begins, never I2C or logging in active PIO. */
    static const uint8_t regs[] = {0x19,0x1c,0x2b};
    for (unsigned i=0;i<3;i++) {
      uint8_t value=0;
      int read_rc=codec_read(NULL,0x10,regs[i],&value,now_ms(NULL)+10);
      k7_session_ab_codec(i,value,read_rc);
      if (read_rc && !rc) rc=read_rc;
    }
  }
#endif
  return rc;
}
static int duplex_amp(void *ctx)
{
  (void)ctx;
  return kd_enable_amp(&codec, now_ms(NULL) + 10);
}
int k7sound_duplex_stream16(const struct k7_duplex_stream_config *c,
                            struct k7_duplex_stream_result *out)
{
  if (!out) return -EINVAL;
  memset(out, 0, sizeof(*out));
  if (!c || !c->microphone || !c->playback || !c->stop_all ||
      c->max_capture_frames < 320 || c->max_capture_frames > K7_DUPLEX_MAX_CAPTURE_FRAMES ||
      c->max_capture_frames % 320) return out->result = -EINVAL;
  if (c->stop_all(c->stop_arg)) return out->result = -ECANCELED;
  /* Existing audio entry points use this same lock. Never release ownership
   * to another task while the ADC or shared serial clocks remain active. */
  if (pthread_mutex_trylock(&owner)) return out->result = -EBUSY;
  if (bus.poisoned || bus.held || data_held || platform.held || reset_fault) {
    pthread_mutex_unlock(&owner); return out->result = -EIO;
  }
  struct saved_i2c old = {0};
  struct sap_port sap = {NULL, abs_read, abs_write, now_us};
  struct kd_port kd = {NULL, now_ms, codec_write, codec_read, delay_ms,
                       amplifier, codec_ready, quiesce};
  memset(&bus, 0, sizeof(bus)); memset(&platform, 0, sizeof(platform));
  int rc = prepare_i2c(&old);
  if (!rc) rc = amp_prepare();
  old_audio_root = rd(NULL, CRU + 0x3a8); old_audio_gate = rd(NULL, CRU + 0x81c);
  bool clock_changed = false;
  if (!rc) {
    if (rd(NULL, CRU + 0xa20) & 0x60) rc = -EBUSY;
    else {
      masked(CRU + 0x3a8, 3, 3); masked(CRU + 0x81c, 2, 0); clock_changed = true;
      if ((rd(NULL, CRU + 0x3a8) & 3) != 3 || (rd(NULL, CRU + 0x81c) & 2)) rc = -EIO;
    }
  }
  up_disable_irq(220);
  if (!rc) rc = sap_setup(&sap, &platform, 1);
  if (!rc) rc = kd_bind(&codec, &kd);
  if (!rc) rc = kd_prepare_adc_variant(&codec, 32, KD_ADC_PGA24, now_ms(NULL) + 3000);
  if (!rc) rc = kd_set_playback_max(&codec, now_ms(NULL) + 100);
  if (!rc) rc = kd_set_playback_gain(&codec, speech_gain_db, now_ms(NULL) + 100);
  for (unsigned settled = 0; !rc && settled < 1000; settled += 10) {
    if (c->stop_all(c->stop_arg)) rc = -ECANCELED;
    else rc = delay_ms(NULL, 10, now_ms(NULL) + 20);
  }
  if (!rc) {
    struct pio_port p = {NULL, sai_read, sai_write, now_us, duplex_arm,
                         duplex_amp, amp_off, 0, NULL, NULL};
    rc = pio_duplex_stream16(&p, platform.ready, 4096000, c, out);
    data_held = out->held;
  }
  int stop_rc = data_held ? -EBUSY : platform.ready ? kd_stop(&codec, now_ms(NULL) + 1500) : 0;
  if (amp_ready) amp_off(NULL);
  up_udelay(10);
  if (stop_rc || data_held) platform.held = true;
  int platform_rc = platform.ready && !stop_rc && !data_held ?
    sap_cleanup(&sap, &platform, !quiesce(NULL, now_ms(NULL) + 10)) : (platform.held ? -EBUSY : 0);
  if (clock_changed && !platform.held) {
    masked(CRU + 0x3a8, 3, old_audio_root); masked(CRU + 0x81c, 2, old_audio_gate);
    if ((rd(NULL, CRU + 0x3a8) & 3) != (old_audio_root & 3) ||
        (rd(NULL, CRU + 0x81c) & 2) != (old_audio_gate & 2)) platform_rc = -EIO;
  }
  int bus_rc = !platform.held ? restore_i2c(&old) : -EBUSY;
  if (bus_rc) bus.poisoned = true;
  out->codec_stop = stop_rc; out->platform_restore = platform_rc; out->i2c_restore = bus_rc;
  out->held = data_held || platform.held;
  out->result = rc ? rc : stop_rc ? stop_rc : platform_rc ? platform_rc : bus_rc;
  pthread_mutex_unlock(&owner);
  return out->result;
}
struct duplex_probe {
  unsigned tx_at, tone_frames, cancel_at, rx_count, reference_count;
  uint64_t abs_sum;
  unsigned peak, clipped;
};
static int probe_stop(void *arg) { (void)arg; return atomic_load(&duplex_probe_stop); }
static int probe_cancel(void *arg)
{
  struct duplex_probe *p = arg;
  return p->cancel_at && p->rx_count >= p->cancel_at;
}
static int probe_pull(void *arg, int16_t *sample)
{
  struct duplex_probe *p = arg;
  if (p->tx_at >= p->tone_frames) return 0;
  /* Explicit, bounded low-amplitude diagnostic tone; never microphone data. */
  *sample = (p->tx_at++ / 8) % 2 ? -1024 : 1024;
  return 1;
}
static int probe_mic(void *arg, int16_t sample, uint64_t index)
{
  struct duplex_probe *p = arg;
  if (index != p->rx_count) return -EILSEQ;
  p->rx_count++; unsigned magnitude = sample < 0 ? -(int)sample : sample;
  p->abs_sum += magnitude; if (magnitude > p->peak) p->peak = magnitude;
  if (sample == INT16_MAX || sample == INT16_MIN) p->clipped++;
  return 0;
}
static int probe_render(void *arg, int16_t sample, uint64_t index)
{
  struct duplex_probe *p = arg;
  if (index != p->reference_count || (sample != -1024 && sample != 1024)) return -EILSEQ;
  p->reference_count++; return 0;
}
struct duplex_probe_job {
  const struct k7_duplex_stream_config *config;
  struct k7_duplex_stream_result result;
  atomic_bool done;
  int rc;
};
static void *duplex_probe_worker(void *arg)
{
  struct duplex_probe_job *job = arg;
  job->rc = k7sound_duplex_stream16(job->config, &job->result);
  atomic_store_explicit(&job->done, true, memory_order_release);
  return NULL;
}
static int duplex_diagnostic(int argc, char **argv)
{
  /* 1..10s RX, 0.5s tone; optional cancellation at 0.25s exercises RX survival. */
  if (argc != 3 && argc != 4) return 1;
  char *end; errno = 0; unsigned long seconds = strtoul(argv[2], &end, 10);
  if (errno || !argv[2][0] || *end || seconds < 1 || seconds > 10 ||
      (argc == 4 && strcmp(argv[3], "cancel-tx"))) return 1;
  bool expected = false;
  if (!atomic_compare_exchange_strong(&duplex_probe_active, &expected, true)) return 1;
  atomic_store(&duplex_probe_stop, false);
  struct duplex_probe probe = {.tone_frames = 8000, .cancel_at = argc == 4 ? 4000 : 0};
  struct k7_duplex_stream_config c = {
    .max_capture_frames = (unsigned)seconds * 16000, .microphone = probe_mic, .microphone_arg = &probe,
    .playback = probe_pull, .playback_arg = &probe, .render = probe_render, .render_arg = &probe,
    .stop_all = probe_stop, .cancel_tx = probe_cancel, .cancel_tx_arg = &probe
  };
  struct duplex_probe_job job = {.config = &c}; atomic_init(&job.done, false);
  cpu_set_t single; CPU_ZERO(&single); CPU_SET(1, &single);
  pthread_attr_t attr; pthread_t thread;
  int rc = pthread_attr_init(&attr);
  bool initialized = !rc;
  if (!rc) rc = pthread_attr_setstacksize(&attr, 32768);
  if (!rc) rc = pthread_attr_setaffinity_np(&attr, sizeof(single), &single);
  if (!rc) rc = pthread_create(&thread, &attr, duplex_probe_worker, &job);
  if (initialized) pthread_attr_destroy(&attr);
  if (rc) { atomic_store(&duplex_probe_active, false); return 1; }
  int join_rc = pthread_join(thread, NULL);
  if (join_rc) {
    atomic_store(&duplex_probe_stop, true);
    while (!atomic_load_explicit(&job.done, memory_order_acquire)) usleep(1000);
    (void)pthread_detach(thread);
  }
  struct k7_duplex_stream_result r = job.result; rc = job.rc;
  atomic_store(&duplex_probe_active, false);
  printf("DUPLEX result=%d captured=%u tx_queued=%u discarded_words=%u rx_after_tx_stop=%u tx_cancelled=%d eof=%d grouped_mismatches=%u rx_overflow=%u tx_underflow=%u stop=%d held=%d codec=%d platform=%d i2c=%d join=%d\n",
         rc, r.captured, r.tx_queued, r.tx_discarded_words, r.rx_after_tx_stop,
         r.tx_cancelled, r.tx_eof, r.grouped_mismatches, r.rx_overflows, r.tx_underflows,
         r.stop_result, r.held, r.codec_stop, r.platform_restore, r.i2c_restore, join_rc);
  printf("DUPLEX mic_samples=%u abs_mean=%u peak=%u clipped=%u render_samples=%u duration_us=%" PRIu64 "\n",
         probe.rx_count, probe.rx_count ? (unsigned)(probe.abs_sum / probe.rx_count) : 0,
         probe.peak, probe.clipped, probe.reference_count, r.end_us >= r.start_us ? r.end_us - r.start_us : 0);
  return rc || join_rc ? 1 : 0;
}
#endif

#ifdef CONFIG_EXAMPLES_K7SOUND_SESSION
static int session_amp(void *ctx)
{
  /* Both converters remain armed for the entire resident lease. Repeated
   * turns switch only GPIO/SAI TXS; no I2C delay can block active RX. */
  return amplifier(ctx, true, now_ms(NULL) + 10);
}
static int sound_session_run(void *unused, struct k7_voice_session *session)
{
  (void)unused;
  if (k7_voice_session_stop_requested(session)) {
    k7_voice_session_cleanup_complete(session, 0); return -ECANCELED;
  }
  if (pthread_mutex_trylock(&owner)) {
    k7_voice_session_cleanup_complete(session, -EBUSY); return -EBUSY;
  }
  if (bus.poisoned || bus.held || data_held || platform.held || reset_fault) {
    k7_voice_session_cleanup_complete(session, -EIO);
    pthread_mutex_unlock(&owner); return -EIO;
  }
  struct k7_duplex_session_result result = {0};
  struct saved_i2c old = {0};
  struct sap_port sap = {NULL, abs_read, abs_write, now_us};
  struct kd_port kd = {NULL, now_ms, codec_write, codec_read, delay_ms,
                       amplifier, codec_ready, quiesce};
  memset(&bus, 0, sizeof(bus)); memset(&platform, 0, sizeof(platform));
  int rc = prepare_i2c(&old);
  if (!rc) rc = amp_prepare();
  old_audio_root = rd(NULL, CRU + 0x3a8); old_audio_gate = rd(NULL, CRU + 0x81c);
  bool clock_changed = false;
  if (!rc) {
    if (rd(NULL, CRU + 0xa20) & 0x60) rc = -EBUSY;
    else {
      masked(CRU + 0x3a8, 3, 3); masked(CRU + 0x81c, 2, 0); clock_changed = true;
      if ((rd(NULL, CRU + 0x3a8) & 3) != 3 || (rd(NULL, CRU + 0x81c) & 2)) rc = -EIO;
    }
  }
  up_disable_irq(220);
  if (!rc) rc = sap_setup(&sap, &platform, 1);
  if (!rc) rc = kd_bind(&codec, &kd);
  if (!rc) rc = kd_prepare_adc_variant(&codec, 32, KD_ADC_PGA24, now_ms(NULL) + 3000);
  if (!rc) rc = kd_set_playback_max(&codec, now_ms(NULL) + 100);
  if (!rc) rc = kd_set_playback_gain(&codec, speech_gain_db, now_ms(NULL) + 100);
  for (unsigned settled = 0; !rc && settled < 1000; settled += 10) {
    if (k7_voice_session_stop_requested(session)) rc = -ECANCELED;
    else rc = delay_ms(NULL, 10, now_ms(NULL) + 20);
  }
  if (!rc) {
    struct pio_port p = {NULL, sai_read, sai_write, now_us, duplex_arm,
                         session_amp, amp_off, 0, NULL, NULL};
    rc = pio_duplex_session(&p, platform.ready, 4096000, session, &result);
    data_held = result.held;
  }
  int stop_rc = data_held ? -EBUSY : platform.ready ? kd_stop(&codec, now_ms(NULL) + 1500) : 0;
  int amp_rc = amp_ready ? amp_off(NULL) : 0;
  up_udelay(10);
  if (stop_rc || data_held || amp_rc) platform.held = true;
  int platform_rc = platform.ready && !stop_rc && !data_held && !amp_rc ?
    sap_cleanup(&sap, &platform, !quiesce(NULL, now_ms(NULL) + 10)) : (platform.held ? -EBUSY : 0);
  int clock_rc = 0;
  if (clock_changed && !platform.held) {
    masked(CRU + 0x3a8, 3, old_audio_root); masked(CRU + 0x81c, 2, old_audio_gate);
    if ((rd(NULL, CRU + 0x3a8) & 3) != (old_audio_root & 3) ||
        (rd(NULL, CRU + 0x81c) & 2) != (old_audio_gate & 2)) {
      clock_rc = -EIO; platform.held = true; /* never grant a second owner */
    }
  } else if (clock_changed) {
    clock_rc = -EBUSY;
  }
  int bus_rc = !platform.held ? restore_i2c(&old) : -EBUSY;
  if (bus_rc) bus.poisoned = true;
  int cleanup_rc = result.amp_off_result ? result.amp_off_result :
    result.stop_result ? result.stop_result : stop_rc ? stop_rc : amp_rc ? amp_rc :
    platform_rc ? platform_rc : clock_rc ? clock_rc : bus_rc ? bus_rc :
    (data_held || platform.held || bus.held || bus.poisoned || reset_fault) ? -EIO : 0;
  k7_voice_session_cleanup_complete(session, cleanup_rc);
  /* An expected cancellation is successful only if every cleanup passed. */
  if (cleanup_rc) rc = cleanup_rc;
  printf("SESSION end result=%d captured=%" PRIu64 " turns=%" PRIu64 " queued=%" PRIu64 " cancelled=%" PRIu64 " failed=%" PRIu64 " mismatch=%u rx_overflow=%u tx_underflow=%u stop=%d held=%u codec=%d platform=%d clock=%d i2c=%d cleanup=%d pauses=%" PRIu64 " resumes=%" PRIu64 " silence=%" PRIu64 " discarded_silence_words=%" PRIu64 " provenance_errors=%u\n",
         rc, result.captured, result.turns, result.queued, result.cancelled_turns, result.failed_turns,
         result.grouped_mismatches, result.rx_overflows, result.tx_underflows, result.stop_result,
         (unsigned)(data_held || platform.held), stop_rc, platform_rc, clock_rc, bus_rc,
         cleanup_rc, result.tx_pauses, result.tx_resumes, result.tx_silence_samples,
         result.tx_discarded_silence_words, result.tx_provenance_errors);
  pthread_mutex_unlock(&owner); return rc;
}
#if defined(K7_APM21_FIXED_PHYSICAL) && K7_APM21_FIXED_PHYSICAL
int k7sound_session_start(const struct k7_voice_session_config *config, struct k7_voice_session **out)
{
  int rc = k7_voice_session_create(config, out); if (rc) return rc;
  rc = k7_voice_session_start(*out, sound_session_run, NULL);
  if (rc) {
    /* start may have launched its worker before a later mutex error. Never
     * lose an owner whose actual joined lifetime could not be proven. */
    int destroy_rc = k7_voice_session_destroy(*out);
    if (destroy_rc) return destroy_rc;
    *out = NULL;
  }
  return rc;
}
#else
#line 1050
int k7sound_session_start(const struct k7_voice_session_config *config, struct k7_voice_session **out)
{
  int rc = k7_voice_session_create(config, out); if (rc) return rc;
  rc = k7_voice_session_start(*out, sound_session_run, NULL);
  if (rc) { k7_voice_session_destroy(*out); *out = NULL; }
  return rc;
}
#endif
#line 1056
static int session_diagnostic(int argc, char **argv)
{
  if (argc != 3) return 1;
  char *end; errno = 0; unsigned long seconds = strtoul(argv[2], &end, 10);
  if (errno || !argv[2][0] || *end || seconds < 3 || seconds > 300) return 1;
  struct k7_voice_session_config c = {.tx_prebuffer_samples = 3200, .render_reference = true};
  struct k7_voice_session *session = NULL;
  pthread_mutex_lock(&session_probe_lock);
  int rc = session_probe ? -EBUSY : k7sound_session_start(&c, &session);
  if (!rc) session_probe = session;
  pthread_mutex_unlock(&session_probe_lock);
  if (rc) return 1;
  k7_voice_session_asr_epoch(session, 1);
  unsigned generation = 0, submitted = 0, kws = 0, asr = 0, render = 0, turns = 0;
  bool ended = false, cancelled = false, playing_seen = false, complete = false;
  uint64_t tx_started_at = 0, began = now_ms(NULL), next_at = began;
  while (!rc) {
    struct k7_voice_session_status status; k7_voice_session_status(session, &status);
    if (status.finished) { rc = status.result; break; }
    uint64_t now = now_ms(NULL);
    if (status.stop_requested || now < began || now - began >= seconds * 1000u) break;
    struct k7_voice_frame frame;
    int got;
    while ((got = k7_voice_session_take_kws(session, &frame)) == 1) kws++;
    if (got < 0 && got != -EPIPE) { rc = got; break; }
    while ((got = k7_voice_session_take_asr(session, 1, &frame)) == 1) asr++;
    if (got < 0) { rc = got; break; }
    while ((got = k7_voice_session_take_render(session, &frame)) == 1) render++;
    if (got < 0) { rc = got; break; }
    if (status.ready && (!generation || complete) && now >= next_at) {
      generation++; submitted = 0; ended = cancelled = playing_seen = complete = false;
      rc = k7_voice_session_tx_begin(session, generation); if (rc) break;
    }
    if (generation && !complete) {
      if (submitted < 8000 && !cancelled) {
        int16_t tone[320]; for (unsigned i = 0; i < 320; ++i) tone[i] = ((submitted + i) / 8) % 2 ? -1024 : 1024;
        int put = k7_voice_session_tx_write(session, generation, tone, 320);
        if (!put) submitted += 320;
        else if (put != -EAGAIN && put != -ECANCELED && put != -EPIPE) { rc = put; break; }
      }
      if (submitted == 8000 && !ended && !cancelled) {
        rc = k7_voice_session_tx_end(session, generation); if (rc) break; ended = true;
      }
      if (status.tx_state == K7_VOICE_TX_RUNNING && !playing_seen) {
        playing_seen = true; tx_started_at = status.microphone_samples;
      }
      if (!(generation & 1u) && playing_seen && !cancelled && status.microphone_samples - tx_started_at >= 4000) {
        rc = k7_voice_session_tx_cancel(session, generation); if (rc) break; cancelled = true;
      }
      struct k7_voice_tx_result tx_result;
      int result_rc = k7_voice_session_tx_result(session, generation, &tx_result);
      if (!result_rc) {
        printf("SESSION turn=%u result=%d queued=%u discarded_words=%u microphone_samples=%" PRIu64 "\n",
               generation, tx_result.result, tx_result.queued_samples, tx_result.discarded_words, status.microphone_samples);
        if (tx_result.result && tx_result.result != -ECANCELED) { rc = tx_result.result; break; }
        turns++; complete = true; next_at = now + 200;
      } else if (result_rc != -EAGAIN) { rc = result_rc; break; }
    }
    usleep(1000);
  }
  k7_voice_session_stop(session);
  int join_rc = k7_voice_session_join(session);
  if (!rc && join_rc != -ECANCELED) rc = join_rc;
  struct k7_voice_session_status status; k7_voice_session_status(session, &status);
  printf("SESSION probe result=%d turns=%u kws_frames=%u asr_frames=%u render_frames=%u kws_overflow=%u asr_overflow=%u render_overflow=%u samples=%" PRIu64 " storage=%zu\n",
         rc, turns, kws, asr, render, status.kws_overflows, status.asr_overflows,
         status.render_overflows, status.microphone_samples, k7_voice_session_storage_bytes());
  pthread_mutex_lock(&session_probe_lock); session_probe = NULL;
  pthread_mutex_unlock(&session_probe_lock);
  int destroy_rc = k7_voice_session_destroy(session);
  return rc || destroy_rc || !turns ? 1 : 0;
}
#endif
