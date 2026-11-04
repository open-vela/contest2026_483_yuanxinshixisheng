#include "vv_native_assessment.h"
static int upload(void *arg,const struct vv_assessment_request *q,char id[37],const struct vv_https_cancel *cancel) {
  struct vv_assessment_backend_adapter *a=arg;
  if(!a || !a->client)return VV_BACKEND_CLIENT_INVALID;
  return vv_backend_client_upload(a->client,q->capture_session,q->consent_ref,q->boundary_hex,
    q->idempotency_key,q->images,id,cancel);
}
static int task(void *arg,const char *id,struct vv_backend_task *out,const struct vv_https_cancel *cancel) {
  if(!arg || !((struct vv_assessment_backend_adapter *)arg)->client)return VV_BACKEND_CLIENT_INVALID;
  return vv_backend_client_task(((struct vv_assessment_backend_adapter *)arg)->client,id,out,cancel);
}
static int brief(void *arg,const char *id,struct vv_backend_brief *out,const struct vv_https_cancel *cancel) {
  if(!arg || !((struct vv_assessment_backend_adapter *)arg)->client)return VV_BACKEND_CLIENT_INVALID;
  return vv_backend_client_brief(((struct vv_assessment_backend_adapter *)arg)->client,id,out,cancel);
}
static int tts(void *arg,const char *text,unsigned char *pcm,size_t cap,size_t *size,const struct vv_https_cancel *cancel) {
  struct vv_assessment_backend_adapter *a=arg;
  if(!a || !a->client)return VV_BACKEND_CLIENT_INVALID;
  return vv_backend_client_tts(a->client,&a->tts_endpoint,a->tts_device_bearer,text,pcm,cap,size,cancel);
}
const struct vv_assessment_ops vv_assessment_backend_ops={upload,task,brief,tts};
