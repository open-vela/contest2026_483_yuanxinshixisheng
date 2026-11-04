#if defined(K7_NEAR_EXPERIMENT) && K7_NEAR_EXPERIMENT
#ifndef VV_NEAR_DRIVER_H
#define VV_NEAR_DRIVER_H
#include "vv_barge_route.h"
/* Calibration importer must verify pinned actual capture/model/DSP records;
 * nonzero epochs and caller claims are not a valid importer implementation. */
struct nd_calibration {
 struct vv_confirmed_wake_context context;
 uint8_t evidence_sha256[32],model_sha256[32],transform_sha256[32];
 uint64_t first,next,expires_ms,revision;
 bool tail_supported;
 bool experimental_policy; /* policy permission, never calibrated evidence */
};
struct nd_evidence {
 uint64_t calibration_revision;
 struct vv_barge_onset_support support;
 /* Private worker adapter checks actual transform and exact support against
  * calibration. This field never carries a RAW or numeric-only report. */
};
struct near_driver;
struct nd_ops {
 void *arg;
 int (*verify_import)(void *,const struct nd_calibration *);
 /* Acquires actual owner/input/TX/mode/calibration pin; no route re-entry. */
 int (*pin)(void *,const struct nd_calibration *,struct vv_barge_observation *);
 void (*unpin)(void *);
 /* Called under the independent pin. Must independently verify the actual
  * transform/physical range and near evidence; NULL fails closed. */
 int (*verify_frame)(void *,const struct nd_calibration *,const struct k7_voice_frame *,const struct k7_voice_frame *,bool tail);
 int (*verify_onset)(void *,const struct nd_calibration *,const struct nd_evidence *);
 uint64_t (*now_ms)(void *);
 /* Subscribe/unsubscribe the existing serial worker; no new MIC owner. On
  * failed start no observer may remain. stop must stop/join before success. */
 int (*start)(void *,struct near_driver *);
 int (*stop_join)(void *,struct near_driver *);
};
int nd_create(const struct nd_ops *,struct near_driver **);
int nd_install(struct near_driver *,const struct nd_calibration *);
void nd_invalidate(struct near_driver *);
void nd_export(struct near_driver *,struct vv_barge_driver *);
/* Exact actual RAW and independently transformed PCM, same sample interval.
 * tail=false forwards clean PCM to existing pre-roll queue. tail=true stores
 * transform then passes RAW through existing cancel-tail staging/validation. */
int nd_frame(struct near_driver *,const struct vv_confirmed_wake_context *,const struct k7_voice_frame *raw,const struct k7_voice_frame *clean,bool tail);
int nd_onset(struct near_driver *,const struct nd_evidence *,uint64_t frame_first,uint64_t frame_next);
int nd_destroy(struct near_driver *);
#endif

#else
#ifndef VV_NEAR_DRIVER_H
#define VV_NEAR_DRIVER_H
#include "vv_barge_route.h"
/* Calibration importer must verify pinned actual capture/model/DSP records;
 * nonzero epochs and caller claims are not a valid importer implementation. */
struct nd_calibration {
 struct vv_confirmed_wake_context context;
 uint8_t evidence_sha256[32],model_sha256[32],transform_sha256[32];
 uint64_t first,next,expires_ms,revision;
 bool tail_supported;
};
struct nd_evidence {
 uint64_t calibration_revision;
 struct vv_barge_onset_support support;
 /* Private worker adapter checks actual transform and exact support against
  * calibration. This field never carries a RAW or numeric-only report. */
};
struct near_driver;
struct nd_ops {
 void *arg;
 int (*verify_import)(void *,const struct nd_calibration *);
 /* Acquires actual owner/input/TX/mode/calibration pin; no route re-entry. */
 int (*pin)(void *,const struct nd_calibration *,struct vv_barge_observation *);
 void (*unpin)(void *);
 /* Called under the independent pin. Must independently verify the actual
  * transform/physical range and near evidence; NULL fails closed. */
 int (*verify_frame)(void *,const struct nd_calibration *,const struct k7_voice_frame *,const struct k7_voice_frame *,bool tail);
 int (*verify_onset)(void *,const struct nd_calibration *,const struct nd_evidence *);
 uint64_t (*now_ms)(void *);
 /* Subscribe/unsubscribe the existing serial worker; no new MIC owner. On
  * failed start no observer may remain. stop must stop/join before success. */
 int (*start)(void *,struct near_driver *);
 int (*stop_join)(void *,struct near_driver *);
};
int nd_create(const struct nd_ops *,struct near_driver **);
int nd_install(struct near_driver *,const struct nd_calibration *);
void nd_invalidate(struct near_driver *);
void nd_export(struct near_driver *,struct vv_barge_driver *);
/* Exact actual RAW and independently transformed PCM, same sample interval.
 * tail=false forwards clean PCM to existing pre-roll queue. tail=true stores
 * transform then passes RAW through existing cancel-tail staging/validation. */
int nd_frame(struct near_driver *,const struct vv_confirmed_wake_context *,const struct k7_voice_frame *raw,const struct k7_voice_frame *clean,bool tail);
int nd_onset(struct near_driver *,const struct nd_evidence *,uint64_t frame_first,uint64_t frame_next);
int nd_destroy(struct near_driver *);
#endif

#endif

#if defined(K7_NEAR_MULTIROUND) && K7_NEAR_MULTIROUND
int nd_rearm(struct near_driver *);
#endif
