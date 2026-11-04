#ifndef VV_ALIYUN_PROTOCOL_H
#define VV_ALIYUN_PROTOCOL_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define VV_ALIYUN_ASR_PCM_MAX 96000u
#define VV_ALIYUN_JSON_MAX 16384u
struct vv_aliyun_download {char host[129];char path[2049];};
/* Pure codecs; no keys/network/audio. Input/output must not alias. Output is
 * cleared on rejection except WAV overlap rejection, which preserves input. */
int vv_aliyun_asr_json(const char *model,const unsigned char *pcm,size_t,char *,size_t,size_t *);
int vv_aliyun_asr_text(const char *,size_t,char *,size_t);
int vv_aliyun_tts_json(const char *model,const char *voice,const char *text,char *,size_t,size_t *);
/* Exact approved host list, HTTPS443 only; no auth/userinfo/redirect acceptance.
 * Signed query is preserved but MUST NOT be logged or sent vendor Bearer. */
int vv_aliyun_tts_download_policy(const char *,size_t,const char *const *,size_t,bool,struct vv_aliyun_download *);
int vv_aliyun_tts_download(const char *,size_t,const char *const *,size_t,struct vv_aliyun_download *);
/* Strict finite RIFF/WAVE PCM16, mono/stereo, 8..48kHz; linear resampling to
 * 16k mono S16LE, <=30sec. Reject raw/MP3/truncation instead of guessing. */
/* Explicit compatibility for the exact observed 44-byte Aliyun sentinel header.
 * Caller must supply a complete, successfully framed HTTPS body. */
int vv_aliyun_wav_pcm_policy(const unsigned char *,size_t,unsigned char *,size_t,size_t *,bool);
int vv_aliyun_wav_pcm(const unsigned char *,size_t,unsigned char *,size_t,size_t *);
#endif
