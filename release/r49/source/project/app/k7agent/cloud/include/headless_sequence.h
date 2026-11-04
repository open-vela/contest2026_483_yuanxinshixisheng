/* Bounded, opt-in standalone trial. No reconnect/restart or storage writes. */
#ifndef VV_HEADLESS_SEQUENCE_H
#define VV_HEADLESS_SEQUENCE_H
#include <errno.h>
struct vv_headless_ops {
  int (*network_ready)(void);
  int (*gateway_check)(void);
  int (*camera_start)(void);
  int (*camera_ready)(void);
  int (*voice_run)(void);
  void (*stop)(void);
  void (*tick)(void);
  void (*state)(const char *, int);
};
static int vv_headless_run(const struct vv_headless_ops *o)
{
  int rc = -ETIMEDOUT;
  unsigned i;
  o->state("waiting-network", 0);
  for (i = 0; i < 120; ++i) {
    rc = o->network_ready();
    if (rc == 0) break;
    if (rc != -EAGAIN) goto failed;
    o->tick();
  }
  if (i == 120) { rc = -ETIMEDOUT; goto failed; }
  o->state("gateway-tcp-check", 0);
  rc = o->gateway_check();
  if (rc) goto failed;
  /* A TCP connection is not proof of ASR/TTS readiness. */
  o->state("camera-start", 0);
  rc = o->camera_start();
  if (rc) goto failed;
  for (i = 0; i < 30; ++i) {
    rc = o->camera_ready();
    if (!rc) break;
    if (rc != -EAGAIN) goto stop;
    o->tick();
  }
  if (i == 30) { rc = -ETIMEDOUT; goto stop; }
  o->state("voice-session", 0);
  rc = o->voice_run();
stop:
  o->stop();
failed:
  o->state(rc ? "failed" : "session-ended", rc);
  return rc;
}
#endif
