#ifndef K7_VAD_ORT_BACKEND_H
#define K7_VAD_ORT_BACKEND_H
#include "k7_vad.h"
#if defined(__MINGW32__) && !defined(_stdcall)
/* Native ORT1.17.1 header spells this MSVC keyword with one underscore. */
#define _stdcall __stdcall
#endif
#if defined(__MINGW32__)
#include <specstrings.h>
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#endif
#include "onnxruntime_c_api.h"
#ifdef __cplusplus
extern "C" {
#endif
struct k7_vad_ort;
struct k7_vad_model {
  const void *data;
  size_t bytes;
  /* Required caller SHA provider, e.g. existing NuttX SHA256 adapter. */
  int (*sha256)(const void *,size_t,uint8_t digest[32],void *context);
  void *hash_context;
};
/* Only pinned original Silero5.1 ONNX or reviewed ORT1.17.1 disable artifact.
 * Model bytes are borrowed and MUST remain immutable/alive until destroy.
 * CPU only, threads1, sequential, disabled graph optimization, no file path.
 * OrtApiBase/module must also outlive backend. CreateSessionFromArray avoids
 * Sherpa's file loader and its default60-second segment buffering entirely.
 */
K7VAD_API int k7_vad_ort_create(const OrtApiBase *,const struct k7_vad_model *,
                               struct k7_vad_ort **);
K7VAD_API const struct k7_vad_backend *k7_vad_ort_ops(void);
K7VAD_API size_t k7_vad_ort_context_bytes(void);
K7VAD_API const char *k7_vad_ort_last_error(const struct k7_vad_ort *);
K7VAD_API void k7_vad_ort_destroy(struct k7_vad_ort *);
#ifdef __cplusplus
}
#endif
#endif
