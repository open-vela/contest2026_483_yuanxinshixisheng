/* SPDX-License-Identifier: Apache-2.0 */
/* Bounded bring-up diagnostics for the 32e6:9221 UVC camera. */
#include <nuttx/config.h>
#include <nuttx/usb/rk3576_flow.h>
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbhost.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include <nuttx/clock.h>
#include <nuttx/semaphore.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>
#include "k7_capture_lifetime.h"
static atomic_bool g_service_stop;
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
static atomic_bool g_service_attempted, g_service_active;
static atomic_int g_service_result;
static bool capture_service_begin(void)
{
  if (atomic_exchange(&g_service_attempted,true)) return false;
  atomic_store(&g_service_stop,false);
  atomic_store(&g_service_result,-EINPROGRESS);
  atomic_store(&g_service_active,true);
  return true;
}
static void capture_service_end(int result)
{
  atomic_store(&g_service_result,result);
  atomic_store(&g_service_active,false);
  printf("K7 CAPTURE service_done=1 result=%d\n",result);
}
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
#include "k7_jpeg.h"
#include "k7_pipeline.h"
#include "k7_photo.h"
#include "k7_pose.h"
#ifdef CONFIG_EXAMPLES_K7DISPLAY
#include "../k7display/rk3576_display_native.h"
#include <stdatomic.h>
#endif
#endif
struct k7_pipeline_sink_s;
struct k7_pipeline_stats_s;
#if defined(CONFIG_EXAMPLES_K7HOST_JPEG) && defined(CONFIG_EXAMPLES_K7DISPLAY)
static mutex_t g_lcd_lock = NXMUTEX_INITIALIZER;
static bool g_lcd_active;
static unsigned int g_lcd_request;
static atomic_bool g_lcd_stop;
static atomic_bool g_panel_active;
static atomic_bool g_panel_stop;
static atomic_int g_panel_preview_result = ATOMIC_VAR_INIT(-EAGAIN);
/* Read-only startup gate: colorbars and a live vision worker do not prove that
 * the LCD sidecar ever committed a camera frame. Errors stay latched. */
int k7host_panel_preview_ready(void)
{
  if (!atomic_load(&g_panel_active)) return -ENODEV;
  if (atomic_load(&g_panel_stop)) return -ECANCELED;
  return atomic_load(&g_panel_preview_result);
}
static int panel_present(void *, const uint8_t *, unsigned int, unsigned int, size_t, size_t);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
static int panel_stage_prepare(void *, const uint8_t *, unsigned, unsigned,
                                size_t, size_t, const struct k7ds_epoch *, unsigned,
                                struct k7ds_ticket *);
static int panel_stage_commit(void *, const struct k7ds_ticket *);
#endif
#endif

extern int rk3576_usbhost_initialize(void);
static mutex_t g_lock = NXMUTEX_INITIALIZER;
static bool g_registered;
static bool g_connected;
static uint8_t g_desc[2048];
static int g_desclen;
/* Disconnect must never wait for g_lock: an in-flight control transfer may
 * need the HCD worker. Re-enumeration is allowed only before capture and
 * when no control/capture owner holds g_lock. */
static bool g_created;
static bool g_capture_attempted;
static struct usbhost_class_s *g_camera_class;
static struct usbhost_hubport_s g_camera_port;
static uint8_t g_probe[26];
static bool g_committed;
static uint8_t g_vsif;
static struct usbhost_hubport_s *g_camera_hport;
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static bool camera_connected(void)
{
  irqstate_t flags = enter_critical_section();
  bool connected = g_connected;
  leave_critical_section(flags);
  return connected;
}

static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static int connect_camera(struct usbhost_class_s *c, const uint8_t *d, int n)
{
  int offset = 0;
  if (n < 9 || n > sizeof(g_desc)) return -E2BIG;
  while (offset < n)
    {
      if (n - offset < 2 || d[offset] < 2 || d[offset] > n - offset)
        return -EINVAL;
      offset += d[offset];
    }
  nxmutex_lock(&g_lock);
  if (g_camera_class != c)
    { nxmutex_unlock(&g_lock); return -ENODEV; }
  memcpy(g_desc, d, n);
  g_desclen = n;
  g_camera_hport = c->hport;
  g_camera_port = *c->hport;
  g_committed = false;
  g_connected = true;
  nxmutex_unlock(&g_lock);
  printf("K7 CAMERA ENUMERATED: 32e6:9221 address=%u speed=%u config_bytes=%d\n",
         c->hport->funcaddr, c->hport->speed, n);
  return 0;
}
static int disconnect_camera(struct usbhost_class_s *c)
{
  irqstate_t flags = enter_critical_section();
  if (g_camera_class == c)
    {
      g_connected = false;
      g_committed = false;
      g_camera_class = NULL;
      g_created = false;
    }
  leave_critical_section(flags);
  /* No control/capture operation retains the class allocation itself.
   * Hub disconnect contract transfers its destruction to this callback. */
  kmm_free(c);
  return 0;
}
static struct usbhost_class_s *create_camera(struct usbhost_hubport_s *port,
                                             const struct usbhost_id_s *id)
{
  struct usbhost_class_s *c;
  if (id->vid != 0x32e6 || id->pid != 0x9221) return NULL;
  if (nxmutex_trylock(&g_lock) < 0) return NULL;
  if (g_created || g_capture_attempted)
    { nxmutex_unlock(&g_lock); return NULL; }
  c = kmm_zalloc(sizeof(*c));
  if (c)
    {
      g_created = true;
      g_camera_class = c;
      c->hport = port;
      c->connect = connect_camera;
      c->disconnected = disconnect_camera;
    }
  nxmutex_unlock(&g_lock);
  return c;
}
static const struct usbhost_id_s g_id[] = {{0xef, 2, 1, 0x32e6, 0x9221}};
static struct usbhost_registry_s g_registry = {NULL, create_camera, 1, g_id};

static void camera_info(void)
{
  int offset = 0;
  int cls = -1;
  int sub = -1;
  int alt = -1;
  int interface = -1;
  nxmutex_lock(&g_lock);
  printf("camera connected=%u committed=%u descriptor_bytes=%d\n", g_connected, g_committed, g_desclen);
  while (offset < g_desclen)
    {
      const uint8_t *d = &g_desc[offset];
      unsigned int n = d[0];
      if (n < 2 || n > g_desclen - offset) break;
      if (d[1] == 4 && n >= 9)
        {
          interface = d[2]; alt = d[3]; cls = d[5]; sub = d[6];
          printf("IF %d alt=%d class=%02x subclass=%02x endpoints=%u\n",
                 interface, alt, cls, sub, d[4]);
        }
      else if (d[1] == 5 && n >= 7 && cls == 14)
        {
          unsigned int packet = le16(d + 4);
          printf(" video EP=%02x transfer=%u packet=%u transactions=%u interval=%u\n",
                 d[2], d[3] & 3, packet & 0x7ff, 1 + ((packet >> 11) & 3), d[6]);
        }
      else if (d[1] == 0x24 && n >= 5 && cls == 14 && sub == 1 && d[2] == 1)
        printf(" UVC version=%04x\n", le16(d + 3));
      else if (d[1] == 0x24 && n >= 9 && cls == 14 && sub == 2 &&
               (d[2] == 5 || d[2] == 7))
        printf(" frame subtype=%u index=%u width=%u height=%u\n",
               d[2], d[3], le16(d + 5), le16(d + 7));
      offset += n;
    }
  nxmutex_unlock(&g_lock);
  puts("Use probe, then snapshot, burst or stream (one capture attempt per boot).");
}

/* UVC 1.0 VS controls have a 26-byte layout. Request and data allocations
 * are separate DMA-safe HCD buffers. Run a probe in the background during
 * bring-up so NSH remains available if the experimental HCD stalls. */
static int camera_query(uint8_t type, uint8_t request, uint16_t value,
                        uint16_t index, uint8_t *data, uint16_t len)
{
  struct usbhost_driver_s *drvr = g_camera_port.drvr;
  struct usb_ctrlreq_s *req;
  uint8_t *setup = NULL;
  uint8_t *buffer = NULL;
  size_t setup_len, buffer_len;
  int ret;
  if (!camera_connected()) return -ENODEV;
  ret = DRVR_ALLOC(drvr, &setup, &setup_len);
  if (ret < 0) return ret;
  ret = DRVR_ALLOC(drvr, &buffer, &buffer_len);
  if (ret < 0) goto done;
  if (setup_len < sizeof(*req) || buffer_len < len)
    { ret = -ENOMEM; goto done; }
  req = (struct usb_ctrlreq_s *)setup;
  req->type = type;
  req->req = request;
  put16(req->value, value);
  put16(req->index, index);
  put16(req->len, len);
  memset(buffer, 0, buffer_len);
  if (!(type & 0x80) && len) memcpy(buffer, data, len);
  ret = (type & 0x80) ? DRVR_CTRLIN(drvr, g_camera_port.ep0, req, buffer) :
                       DRVR_CTRLOUT(drvr, g_camera_port.ep0, req, len ? buffer : NULL);
  if (ret >= 0 && (type & 0x80) && len) memcpy(data, buffer, len);
done:
  if (buffer) DRVR_FREE(drvr, buffer);
  DRVR_FREE(drvr, setup);
  return ret;
}

static void camera_probe(void)
{
  int offset = 0, cls = -1, sub = -1, interface = -1;
  int format = -1, wanted_format = -1, wanted_frame = -1;
  unsigned int version = 0;
  uint32_t interval = 0;
  uint8_t probe[26] = {0};
  int ret = -ENODEV;
  nxmutex_lock(&g_lock);
  g_committed = false;
  if (!camera_connected()) goto done;
  while (offset < g_desclen)
    {
      const uint8_t *d = g_desc + offset;
      unsigned int n = d[0];
      if (n < 2 || n > g_desclen - offset) { ret = -EINVAL; goto done; }
      if (d[1] == 4 && n >= 9)
        { interface = d[2]; cls = d[5]; sub = d[6]; }
      else if (d[1] == 0x24 && n >= 5 && cls == 14 && sub == 1 && d[2] == 1)
        version = le16(d + 3);
      else if (d[1] == 0x24 && n >= 4 && cls == 14 && sub == 2)
        {
          if (d[2] == 6) format = d[3];
          else if (d[2] == 4) format = -1;
          else if (d[2] == 7 && n >= 26 && format > 0 &&
                   le16(d + 5) == 640 && le16(d + 7) == 480)
            { wanted_format = format; wanted_frame = d[3];
              interval = le32(d + 21); g_vsif = interface; }
        }
      offset += n;
    }
  if (version != 0x0100 || wanted_format < 1 || wanted_frame < 1 || !interval)
    { ret = -ENOTSUP; goto done; }
  usleep(5000);
  printf("UVC request MJPEG 640x480 format=%d frame=%d interval=%" PRIu32 " IF=%u\n",
         wanted_format, wanted_frame, interval, g_vsif);
  ret = camera_query(0x01, 11, 0, g_vsif, NULL, 0);
  usleep(5000);
  printf("UVC SET_INTERFACE alt0 ret=%d\n", ret);
  if (ret < 0) goto done;
  ret = camera_query(0xa1, 0x81, 0x0100, g_vsif, probe, sizeof(probe));
  usleep(5000);
  printf("UVC GET_CUR PROBE ret=%d\n", ret);
  if (ret < 0) goto done;
  put16(probe, 1);
  probe[2] = wanted_format; probe[3] = wanted_frame;
  put32(probe + 4, interval);
  put32(probe + 18, 0); put32(probe + 22, 0);
  ret = camera_query(0x21, 1, 0x0100, g_vsif, probe, sizeof(probe));
  usleep(5000);
  printf("UVC SET_CUR PROBE ret=%d\n", ret);
  if (ret < 0) goto done;
  memset(probe, 0, sizeof(probe));
  ret = camera_query(0xa1, 0x81, 0x0100, g_vsif, probe, sizeof(probe));
  usleep(5000);
  printf("UVC negotiated ret=%d format=%u frame=%u interval=%" PRIu32
         " max_frame=%" PRIu32 " payload=%" PRIu32 "\n", ret,
         probe[2], probe[3], le32(probe+4), le32(probe+18), le32(probe+22));
  if (ret < 0) goto done;
  if (probe[2] != wanted_format || probe[3] != wanted_frame ||
      le32(probe+4) != interval || !le32(probe+18) ||
      le32(probe+18) > 4*1024*1024 || !le32(probe+22) || le32(probe+22) > 3072)
    { ret = -ERANGE; goto done; }
  ret = camera_query(0x21, 1, 0x0200, g_vsif, probe, sizeof(probe));
  usleep(5000);
  printf("UVC SET_CUR COMMIT ret=%d\n", ret);
  if (ret >= 0 && camera_connected())
    { memcpy(g_probe, probe, sizeof(probe)); g_committed = true; }
done:
  usleep(5000);
  printf("UVC probe result=%d committed=%u (no streaming yet)\n", ret, g_committed);
  nxmutex_unlock(&g_lock);
}


/* One bounded capture attempt per boot while the experimental HCD endpoint
 * teardown is being validated. Retain endpoint and DMA memory until reset;
 * a timeout must never free memory that hardware may still write. */
static int32_t g_batch_results[K7_FLOW_BANKS][2048];
extern int k7_xhci_isoc_batch(struct usbhost_driver_s *, usbhost_ep_t, uint8_t *, size_t, int32_t *);
extern int k7_xhci_isoc_submit(struct usbhost_driver_s *, usbhost_ep_t, uint8_t *, size_t, int32_t *);
extern int k7_xhci_isoc_wait(usbhost_ep_t);
extern int k7_xhci_flow_start(struct usbhost_driver_s *, usbhost_ep_t,
                              uint8_t **, size_t, int32_t **);
extern int k7_xhci_flow_wait(usbhost_ep_t, unsigned int, bool *);
extern int k7_xhci_flow_refill(struct usbhost_driver_s *, usbhost_ep_t, unsigned int);
extern int k7_xhci_flow_end(usbhost_ep_t);
extern void k7_xhci_flow_diagnostics(usbhost_ep_t);
static uint8_t *g_packet[K7_FLOW_BANKS];
static uint8_t *g_frame;
static size_t g_frame_len;
static usbhost_ep_t g_video_ep;
static _Atomic(usbhost_ep_t) g_video_diag_ep;

/* Retain the latest eight complete JPEG candidates for host decoding. */
#define K7_BURST_FRAMES 8
static uint8_t *g_burst_data;
static size_t g_burst_stride;
static size_t g_burst_lengths[K7_BURST_FRAMES];
static unsigned int g_burst_count;

static uint64_t capture_msec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Metadata only: status must remain readable while a USB control request is
 * blocked.  No status reader takes g_lock, which the producer owns throughout
 * capture.  Counters are sampled once per bank, not per microframe. */
enum k7_capture_stage_e
{
  K7_CAPTURE_IDLE, K7_CAPTURE_SETUP, K7_CAPTURE_WAIT, K7_CAPTURE_PARSE,
  K7_CAPTURE_PUBLISH, K7_CAPTURE_REFILL, K7_CAPTURE_DRAIN,
  K7_CAPTURE_ALT0, K7_CAPTURE_JOIN, K7_CAPTURE_DONE
};
static atomic_uint g_capture_stage;
static atomic_uint g_capture_banks, g_capture_packets, g_capture_bad;
static atomic_uint g_capture_jpegs, g_capture_eof, g_capture_invalid;
static atomic_uint g_capture_markers, g_capture_uvc_errors;
static atomic_uint_fast64_t g_capture_last_bank_ms, g_capture_last_jpeg_ms;
static void capture_diag_print(void)
{
  uint64_t now=capture_msec();
  uint64_t bank=atomic_load(&g_capture_last_bank_ms);
  uint64_t jpeg=atomic_load(&g_capture_last_jpeg_ms);
  printf("K7 CAPTURE_DIAG stage=%u banks=%u packets=%u bad=%u jpegs=%u "
         "eof=%u invalid=%u markers=%u uvc_errors=%u bank_age_ms=%" PRIu64
         " jpeg_age_ms=%" PRIu64 "\n",
         atomic_load(&g_capture_stage),atomic_load(&g_capture_banks),
         atomic_load(&g_capture_packets),atomic_load(&g_capture_bad),
         atomic_load(&g_capture_jpegs),atomic_load(&g_capture_eof),
         atomic_load(&g_capture_invalid),atomic_load(&g_capture_markers),
         atomic_load(&g_capture_uvc_errors),bank && now>=bank ? now-bank : 0,
         jpeg && now>=jpeg ? now-jpeg : 0);
}

#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
static uint8_t *g_rgb;
static size_t g_rgb_len;
static size_t g_rgb_capacity;
static unsigned int g_rgb_width;
static unsigned int g_rgb_height;
static bool g_rgb_busy;
#endif

struct k7_track_config_s;
/* Poll enumeration only. Negotiate once when present; callers must treat
 * negotiation failure as terminal rather than repeatedly resetting UVC. */
int k7host_headless_camera_ready(void)
{
  nxmutex_lock(&g_lock);
  bool connected = camera_connected();
  bool attempted = g_capture_attempted;
  nxmutex_unlock(&g_lock);
  if (attempted) return -EALREADY;
  if (!connected) return -EAGAIN;
  camera_probe();
  nxmutex_lock(&g_lock);
  int rc = camera_connected() && g_committed ? 0 : -EIO;
  nxmutex_unlock(&g_lock);
  return rc;
}

static int camera_capture_sink(unsigned int wanted, unsigned int seconds,
                          unsigned int scale, unsigned int delay_ms, bool detect_faces,
                          const struct k7_track_config_s *track,
                          const struct k7_pipeline_sink_s *sink,
                          struct k7_pipeline_stats_s *sink_stats, bool panel)
{
  struct usbhost_driver_s *drvr = g_camera_port.drvr;
  struct usbhost_epdesc_s ep = {0};
  int offset = 0, alt = -1, interface = -1, chosen_alt = -1;
  unsigned int capacity = 0, best = 0xffff, packets = 0, bytes = 0;
  unsigned int headers = 0, errors = 0, eof = 0, changes = 0;
  unsigned int bad = 0, jpeg_candidates = 0;
  unsigned int current = 0, completed = 0;
  bool flow_started = false;
  unsigned int queued = 0;
  unsigned int boundary_drop = 0, invalid_drop = 0, marker_drop = 0;
  unsigned int missing_eof = 0, padded_jpeg = 0;
  uint64_t started = 0, elapsed = 0;
  uint32_t required, limit;
  uint8_t samples[8][12] = {{0}};
  unsigned int sample_len[8] = {0};
  size_t used = 0;
  bool assembling = false, invalid = false;
  int lastfid = -1, ret = -ENODEV;

#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
  struct k7_pipeline_s *pipeline = NULL;
  struct k7_pipeline_stats_s stats = {0};
  bool joined = false;
#if defined(CONFIG_EXAMPLES_K7DISPLAY)
  struct k7_pipeline_s *panel_pipeline = NULL;
  struct k7_pipeline_stats_s panel_stats = {0};
  int panel_error = 0;
#else
  (void)panel;
#endif
#else
  (void)sink;
  (void)sink_stats;
#endif

  int lockret=(sink || panel) ? nxmutex_trylock(&g_lock) : nxmutex_lock(&g_lock);
  if (lockret<0) return lockret;
  atomic_store(&g_capture_stage,K7_CAPTURE_SETUP);
#if defined(CONFIG_EXAMPLES_K7HOST_JPEG) && defined(CONFIG_EXAMPLES_K7DISPLAY)
  if (sink && atomic_load(&g_lcd_stop)) { ret=-ECANCELED; goto done; }
#endif
  if (!camera_connected() || !g_committed) goto done;
  required = le32(g_probe + 22);
  limit = le32(g_probe + 18);
  unsigned int max_seconds = 1800;
  bool continuous_allowed = false;
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  /* Only the explicit active voice service may use the reserved duration. */
  if (track && track->voice_control) {
    max_seconds = 10800;
    continuous_allowed = seconds == K7_CAPTURE_CONTINUOUS &&
                         atomic_load(&g_service_active);
  }
#endif
  if ((wanted != 1 && wanted != K7_BURST_FRAMES) ||
      (seconds > max_seconds && !continuous_allowed))
    { ret = -EINVAL; goto done; }
  if (g_capture_attempted) { ret = -EALREADY; goto done; }
  if (g_camera_port.speed != USB_SPEED_HIGH || !required || required > 3072 ||
      !limit || limit > 4*1024*1024) { ret = -ENOTSUP; goto done; }
  while (offset < g_desclen)
    {
      const uint8_t *d = g_desc + offset;
      unsigned int n = d[0];
      if (n < 2 || n > g_desclen - offset) { ret = -EINVAL; goto done; }
      if (d[1] == 4 && n >= 9) { interface = d[2]; alt = d[3]; }
      else if (d[1] == 5 && n >= 7 && interface == g_vsif && alt > 0 &&
               (d[2] & 0x80) && (d[3] & 3) == 1)
        {
          unsigned int raw = le16(d + 4);
          capacity = (raw & 0x7ff) * (1 + ((raw >> 11) & 3));
          if (capacity >= required && capacity < best && d[6] == 1)
            { chosen_alt = alt; best = capacity;
              ep.addr = d[2] & 15; ep.mxpacketsize = raw;
              ep.interval = d[6]; }
        }
      offset += n;
    }
  if (chosen_alt < 1) { ret = -ENOSPC; goto done; }
  g_capture_attempted = true;
  ep.hport = g_camera_hport;
  ep.in = true; ep.xfrtype = USB_EP_ATTR_XFER_ISOC;
  g_packet[0] = kmm_memalign(4096, (seconds ? K7_FLOW_SLOTS : 2048) * 4096);
  if (!g_packet[0]) { ret = -ENOMEM; goto done; }
  if (seconds)
    {
      for (unsigned int i=1;i<K7_FLOW_BANKS;i++)
        {
          g_packet[i] = kmm_memalign(4096, K7_FLOW_SLOTS * 4096);
          if (!g_packet[i]) { ret = -ENOMEM; goto done; }
        }
    }
  g_frame = kmm_malloc(limit);
  if (!g_frame) { ret = -ENOMEM; goto done; }
  if (wanted > 1)
    {
      g_burst_stride = limit;
      g_burst_data = kmm_malloc(g_burst_stride * wanted);
      if (!g_burst_data) { ret = -ENOMEM; goto done; }
    }
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
  if (scale)
    {
      size_t needed = (640 / scale) * (480 / scale) * 3;
      if (g_rgb_busy) { ret = -EBUSY; goto done; }
      g_rgb_len = 0;
      if (g_rgb_capacity < needed)
        {
          uint8_t *replacement = kmm_malloc(needed);
          if (!replacement) { ret = -ENOMEM; goto done; }
          kmm_free(g_rgb);
          g_rgb = replacement;
          g_rgb_capacity = needed;
        }
      ret = sink ? k7_pipeline_start_sink(&pipeline,limit,g_rgb,g_rgb_capacity,sink) :
                   k7_pipeline_start(&pipeline, limit, scale, delay_ms,
                                     g_rgb, g_rgb_capacity, detect_faces, track);
      if (ret < 0) goto done;
      g_rgb_busy = true;
#ifdef CONFIG_EXAMPLES_K7DISPLAY
      if (panel)
        {
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
          uint64_t stage_lease=0;
          panel_error=k7display_video_stage_open(&stage_lease);
          const struct k7_pipeline_sink_s panel_sink={
            .arg=NULL,.present=panel_present,.stage_lease=stage_lease,
            .stage_snapshot=k7display_video_stage_snapshot,
            .stage_prepare=panel_stage_prepare,.stage_commit=panel_stage_commit,
            .stage_discard=k7display_video_stage_discard,
            .stage_quiesce=k7display_video_stage_quiesce};
          if(!panel_error)
            panel_error=k7_pipeline_start_lcd_preview(&panel_pipeline,limit,&panel_sink);
          if(panel_error && stage_lease)
            (void)k7display_video_stage_quiesce(NULL,stage_lease);
#else
          const struct k7_pipeline_sink_s panel_sink={NULL,panel_present};
          panel_error=k7display_video_ready();
          if (!panel_error)
            panel_error=k7_pipeline_start_lcd_preview(&panel_pipeline,limit,&panel_sink);
#endif
          if (panel_error) atomic_store(&g_panel_preview_result,panel_error);
          printf("K7 PANEL start result=%d vision_preserved=1\n",panel_error);
        }
#endif
    }
#endif
  printf("UVC ISO configure EP=%02x alt=%d capacity=%u raw=%04x\n",
         ep.addr | 0x80, chosen_alt, best, ep.mxpacketsize);
  ret = DRVR_EPALLOC(drvr, &ep, &g_video_ep);
  usleep(5000); printf("UVC ISO EPALLOC ret=%d\n", ret);
  if (ret < 0) goto done;
  atomic_store(&g_video_diag_ep,g_video_ep);
  ret = camera_query(0x01, 11, chosen_alt, g_vsif, NULL, 0);
  usleep(5000); printf("UVC ISO SET_INTERFACE ret=%d\n", ret);
  if (ret < 0) goto done;
  /* Keep eight short banks queued; carry frame assembly across bank
   * boundaries unless the HCD reports that the queue actually drained. */
  started = capture_msec();
  if (seconds)
    {
      int32_t *results[K7_FLOW_BANKS];
      for (unsigned int i=0;i<K7_FLOW_BANKS;i++) results[i]=g_batch_results[i];
      ret = k7_xhci_flow_start(drvr, g_video_ep, g_packet, best, results);
      if (ret < 0) goto stop_camera;
      flow_started = true;
      queued = K7_FLOW_BANKS;
    }
  for (unsigned int batch = 0;
       seconds ? queued > 0 : (batch < 12 && jpeg_candidates < wanted);
       batch++)
    {
      bool gap = true;
#if defined(CONFIG_EXAMPLES_K7HOST_JPEG) && defined(CONFIG_EXAMPLES_K7DISPLAY)
      if (sink && atomic_load(&g_lcd_stop)) { ret=-ECANCELED; goto stop_camera; }
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
      if (sink && pipeline && (ret=k7_pipeline_sink_status(pipeline))<0)
        goto stop_camera;
#endif
      if (seconds)
        {
          atomic_store(&g_capture_stage,K7_CAPTURE_WAIT);
          ret = k7_xhci_flow_wait(g_video_ep, current, &gap);
          if (ret < 0) goto stop_camera;
          queued--;
#if defined(CONFIG_EXAMPLES_K7HOST_JPEG) && defined(CONFIG_EXAMPLES_K7DISPLAY)
          if (sink && atomic_load(&g_lcd_stop)) { ret=-ECANCELED; goto stop_camera; }
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
          if (sink && pipeline && (ret=k7_pipeline_sink_status(pipeline))<0)
            goto stop_camera;
#endif
        }
      else
        {
          ret = k7_xhci_isoc_batch(drvr, g_video_ep, g_packet[0], best,
                                  g_batch_results[0]);
          usleep(5000);
          printf("UVC ISO batch=%u ret=%d slots=2048\n", batch, ret);
          if (ret < 0) goto stop_camera;
        }
      completed++;
      atomic_store(&g_capture_last_bank_ms,capture_msec());
      atomic_store(&g_capture_stage,K7_CAPTURE_PARSE);
      if (gap)
        {
          if (assembling && used) missing_eof++;
          lastfid = -1; assembling = false; invalid = false; used = 0;
        }
  for (unsigned int packet_index = 0; packet_index < (seconds ? K7_FLOW_SLOTS : 2048); packet_index++)
    {
      ssize_t n = g_batch_results[current][packet_index];
      uint8_t *packet_data = g_packet[current] + packet_index * 4096;
      unsigned int h, flags, fid;
      if (n < 0) { bad++; invalid = true; continue; }
      if (packets < 8)
        { sample_len[packets] = n;
          memcpy(samples[packets], packet_data, n < 12 ? n : 12); }
      packets++; bytes += n;
      if (n == 0) continue;
      if (n < 2 || n > best || packet_data[0] < 2 || packet_data[0] > n)
        { bad++; invalid = true; continue; }
      h = packet_data[0]; flags = packet_data[1]; fid = flags & 1;
      headers++;
      if (lastfid != (int)fid)
        {
          if (assembling && used) missing_eof++;
          if (lastfid >= 0) { assembling = true; changes++; }
          lastfid = fid; used = 0; invalid = false;
        }
      if (flags & 0x40) { errors++; invalid = true; }
      if (assembling && !invalid)
        {
          if (used + n - h > limit) { bad++; invalid = true; }
          else { memcpy(g_frame + used, packet_data + h, n - h); used += n - h; }
        }
      if (flags & 2)
        {
          /* The matched 32e6:9221 camera pads some odd-size MJPEG frames
           * to an even byte count with one zero AFTER the JPEG EOI.
           * Strip only that exact suffix; do not search for an earlier EOI
           * or salvage frames carrying a UVC error or incomplete boundary.
           */
          if (assembling && !invalid && used >= 5 && !(used & 1) &&
              g_frame[0] == 0xff && g_frame[1] == 0xd8 &&
              g_frame[used - 3] == 0xff && g_frame[used - 2] == 0xd9 &&
              g_frame[used - 1] == 0)
            {
              used--;
              padded_jpeg++;
            }
          eof++;
          if (!assembling) boundary_drop++;
          else if (invalid) invalid_drop++;
          else if (used < 4 || g_frame[0] != 0xff || g_frame[1] != 0xd8 ||
                   g_frame[used - 2] != 0xff || g_frame[used - 1] != 0xd9)
            marker_drop++;
          if (assembling && !invalid && used >= 4 &&
              g_frame[0] == 0xff && g_frame[1] == 0xd8 &&
              g_frame[used-2] == 0xff && g_frame[used-1] == 0xd9)
            {
              g_frame_len = used;
              if (wanted > 1)
                {
                  unsigned int slot = jpeg_candidates % K7_BURST_FRAMES;
                  memcpy(g_burst_data + g_burst_stride * slot, g_frame, used);
                  g_burst_lengths[slot] = used;
                  if (g_burst_count < K7_BURST_FRAMES) g_burst_count++;
                }
              jpeg_candidates++;
              atomic_store(&g_capture_last_jpeg_ms,capture_msec());
              atomic_store(&g_capture_stage,K7_CAPTURE_PUBLISH);
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
              /* The committed profile above is MJPEG 640x480. */
              extern void k7_video_publish(const uint8_t *,size_t,uint32_t,uint64_t);
              if (!sink) k7_video_publish(g_frame,used,jpeg_candidates,capture_msec()*1000);
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
              if (pipeline) k7_pipeline_publish(pipeline, g_frame, used);
#ifdef CONFIG_EXAMPLES_K7DISPLAY
              /* Vision receives the original frame first. Display faults and
               * user paneloff stop only this optional branch; never join on
               * the USB producer's hot path. */
              if (panel_pipeline && !panel_error)
                {
                  panel_error=atomic_load(&g_panel_stop) ? -ECANCELED :
                    k7_pipeline_sink_status(panel_pipeline);
                  if (!panel_error) k7_pipeline_publish(panel_pipeline,g_frame,used);
                  else { atomic_store(&g_panel_preview_result,panel_error);
                    printf("K7 PANEL disabled result=%d vision_preserved=1\n",panel_error); }
                }
#endif
#endif
              if (!seconds && jpeg_candidates == wanted) break;
              atomic_store(&g_capture_stage,K7_CAPTURE_PARSE);
            }
          assembling = false;
        }
    }
      atomic_store(&g_capture_banks,completed);
      atomic_store(&g_capture_packets,packets);
      atomic_store(&g_capture_bad,bad);
      atomic_store(&g_capture_jpegs,jpeg_candidates);
      atomic_store(&g_capture_eof,eof);
      atomic_store(&g_capture_invalid,invalid_drop);
      atomic_store(&g_capture_markers,marker_drop);
      atomic_store(&g_capture_uvc_errors,errors);
      if (seconds)
        {
          /* A service stop drains submitted banks instead of aborting DMA.
           * flow_end/alt0/pipeline joins below remain the cleanup authority. */
          if (k7_capture_refill(seconds,capture_msec()-started,
                                atomic_load(&g_service_stop),camera_connected()))
            {
              atomic_store(&g_capture_stage,K7_CAPTURE_REFILL);
              ret = k7_xhci_flow_refill(drvr, g_video_ep, current);
              if (ret < 0) goto stop_camera;
              queued++;
            }
          current = (current + 1) % K7_FLOW_BANKS;
        }
    }
stop_camera:
  atomic_store(&g_capture_stage,K7_CAPTURE_DRAIN);
  /* On success all submitted banks were consumed. On failure do not
   * reuse/free any DMA memory; flow_end releases the task mutex but leaves
   * completion state alive for late hardware events until board reset. */
  if (flow_started)
    {
      int endret = k7_xhci_flow_end(g_video_ep);
      if (ret >= 0 && endret < 0) ret = endret;
      k7_xhci_flow_diagnostics(g_video_ep);
    }
  if (started) elapsed = capture_msec() - started;
  if (ret >= 0 && !camera_connected()) ret = -ENODEV;
  if (ret >= 0 && !jpeg_candidates) ret = -ENODATA;
  /* No memory or endpoint free here, even if stopping the camera fails. */
  if (camera_connected())
    {
      /* Print before the control transfer: a stuck EP0 must not hide the
       * original stream's error counters, as happened on cold-start 18j. */
      capture_diag_print();
      atomic_store(&g_capture_stage,K7_CAPTURE_ALT0);
      int stopret = camera_query(0x01, 11, 0, g_vsif, NULL, 0);
      usleep(5000); printf("UVC ISO alt0 stop ret=%d\n", stopret);
      if (ret >= 0 && stopret < 0) ret = stopret;
    }
done:
  atomic_store(&g_capture_stage,K7_CAPTURE_JOIN);
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
  if (pipeline)
    {
      int joinret = k7_pipeline_finish(pipeline, &stats);
      if (joinret == 0)
        {
          joined = true;
          g_rgb_busy = false;
          g_rgb_len = sink ? 0 : stats.bytes;
          g_rgb_width = stats.width;
          g_rgb_height = stats.height;
          if (ret >= 0 && stats.sink_error) ret = stats.sink_error;
          if (ret >= 0 && sink && !stats.sink_presented) ret = -ENODATA;
          if (ret >= 0 && stats.failed) ret = stats.last_error;
          if (ret >= 0 && stats.face_failures) ret = -EIO;
          if (ret >= 0 && stats.track_error) ret = stats.track_error;
          if (ret >= 0 &&
              stats.published != stats.decoded + stats.failed + stats.dropped)
            ret = -EIO;
        }
      else if (ret >= 0) ret = joinret;
    }
  if (sink_stats) *sink_stats=stats;
#ifdef CONFIG_EXAMPLES_K7DISPLAY
  if (panel_pipeline)
    {
      int panel_join=k7_pipeline_finish(panel_pipeline,&panel_stats);
      uint64_t rate=panel_stats.elapsed_us ?
        (uint64_t)panel_stats.sink_presented*100000000/panel_stats.elapsed_us : 0;
      printf("K7 PANEL end join=%d error=%d presented=%u failed=%u sink_error=%d display_fps=%" PRIu64 ".%02" PRIu64 "\n",
        panel_join,panel_error,panel_stats.sink_presented,panel_stats.failed,
        panel_stats.sink_error,rate/100,rate%100);
      /* Failed joins retain worker-owned buffers through pipeline_finish. */
      if (panel_join < 0 && ret >= 0) ret=panel_join;
    }
#endif
#endif
  /* Rotate the eight-slot retention ring into chronological export order.
   * g_frame is CPU-only scratch here; outstanding DMA never targets it. */
  if (wanted > 1 && seconds && jpeg_candidates >= K7_BURST_FRAMES)
    {
      unsigned int oldest = jpeg_candidates % K7_BURST_FRAMES;
      for (unsigned int n = 0; n < oldest; n++)
        {
          size_t first_len = g_burst_lengths[0];
          memcpy(g_frame, g_burst_data, first_len);
          memmove(g_burst_data, g_burst_data + g_burst_stride,
                  g_burst_stride * (K7_BURST_FRAMES - 1));
          memmove(g_burst_lengths, g_burst_lengths + 1,
                  sizeof(g_burst_lengths[0]) * (K7_BURST_FRAMES - 1));
          memcpy(g_burst_data + g_burst_stride * (K7_BURST_FRAMES - 1),
                 g_frame, first_len);
          g_burst_lengths[K7_BURST_FRAMES - 1] = first_len;
        }
    }
  /* Assembly may contain a partial next frame. Restore the last complete
   * retained frame so existing read-only export tooling remains valid. */
  if (g_burst_count)
    {
      g_frame_len = g_burst_lengths[g_burst_count - 1];
      memcpy(g_frame, g_burst_data + g_burst_stride * (g_burst_count - 1),
             g_frame_len);
    }
  for (unsigned int i = 0; i < g_burst_count; i++)
    {
      usleep(5000);
      printf("UVC retained[%u] bytes=%u\n", i,
             (unsigned)g_burst_lengths[i]);
    }
  usleep(5000);
  printf("UVC ISO result=%d packets=%u bytes=%u headers=%u errors=%u bad=%u\n",
         ret, packets, bytes, headers, errors, bad);
  usleep(5000);
  printf("UVC boundaries fid_changes=%u eof=%u JPEG_candidates=%u retained=%u\n",
         changes, eof, jpeg_candidates, (unsigned)g_frame_len);
  for (unsigned int i = 0; i < packets && i < 8; i++)
    {
      usleep(5000); printf("UVC packet[%u] bytes=%u head=", i, sample_len[i]);
      for (unsigned int j = 0; j < 12 && j < sample_len[i]; j++) printf("%02x", samples[i][j]);
      printf("\n");
    }
  if (seconds)
    {
      printf("UVC REJECT boundary=%u invalid=%u marker=%u missing_eof=%u padded_jpeg=%u\n",
             boundary_drop, invalid_drop, marker_drop, missing_eof, padded_jpeg);
      usleep(5000);
      uint64_t rate100 = elapsed ? (uint64_t)jpeg_candidates * 100000 / elapsed : 0;
      usleep(5000);
      printf("UVC STREAM requested_s=%u elapsed_ms=%" PRIu64
             " batches=%u complete_JPEG=%u retained=%u accepted_fps=%"
             PRIu64 ".%02" PRIu64 " result=%d\n",
             seconds, elapsed, completed, jpeg_candidates, g_burst_count,
             rate100 / 100, rate100 % 100, ret);
    }
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
  if (joined)
    {
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
      if (track)
        {
          usleep(5000);
          printf("TRACK END obs=%u updates=%u tx=%u errors=%u result=%d\n",
                 stats.track_observations, stats.track_updates,
                 stats.track_tx, stats.track_errors, stats.track_error);
          usleep(5000);
          printf("TRACK HOLD x=%d y=%d max_step=%u\n",
                 stats.track_x,stats.track_y,stats.track_max_step);
          usleep(5000);
          printf("TRACK STATES lost=%u confirm=%u active=%u stale=%u halt=%u clipped=%u\n",
                 stats.track_states[0],stats.track_states[1],stats.track_states[2],
                 stats.track_states[3],stats.track_states[4],stats.track_states[5]);
        }
#endif
      if (detect_faces)
        {
          usleep(5000);
          printf("K7 VISION inferred=%u face_frames=%u failures=%u last_count=%u\n",
                 stats.face_inferences, stats.face_frames,
                 stats.face_failures, stats.last_face_count);
          usleep(5000);
          printf("K7 VISION TIME avg_us=%" PRIu64 " max_us=%" PRIu64 "\n",
                 stats.face_inferences ? stats.inference_us / stats.face_inferences : 0,
                 stats.max_inference_us);
          if (stats.last_face_count)
            {
              struct k7_face_s *f = &stats.primary;
              usleep(5000);
              printf("K7 TARGET x=%.3f y=%.3f w=%.3f h=%.3f score=%.6f\n",
                     f->x, f->y, f->width, f->height, f->score);
              usleep(5000);
              printf("K7 TARGET OFFSET dx=%.3f dy=%.3f pixels (detection summary)\n",
                     f->x + f->width / 2 - 80, f->y + f->height / 2 - 60);
            }
        }
      unsigned int attempts = stats.decoded + stats.failed;
      uint64_t fps100 = stats.elapsed_us ?
        (uint64_t)stats.decoded * 100000000 / stats.elapsed_us : 0;
      usleep(5000);
      printf("K7 LIVE published=%u decoded=%u failed=%u dropped=%u peak_pending=%u "
             "last_sequence=%u result=%d\n",
             stats.published, stats.decoded, stats.failed, stats.dropped,
             stats.peak_pending, stats.last_sequence, ret);
      usleep(5000);
      printf("K7 LIVE TIMING scale=%u delay_ms=%u elapsed_us=%" PRIu64
             " decoded_fps=%" PRIu64 ".%02" PRIu64 " avg_decode_us=%" PRIu64
             " max_decode_us=%" PRIu64 " max_parser_age_us=%" PRIu64 "\n",
             scale, delay_ms, stats.elapsed_us, fps100 / 100, fps100 % 100,
             attempts ? stats.decode_us / attempts : 0,
             stats.max_decode_us, stats.max_age_us);
      usleep(5000);
      if (!sink)
        printf("K7 LIVE RGB width=%u height=%u bytes=%u crc32=%08" PRIx32 "\n",
             g_rgb_width, g_rgb_height, (unsigned)g_rgb_len,
             g_rgb_len ? k7_rgb_crc32(g_rgb, g_rgb_len) : 0);
      if (sink)
        {
          usleep(5000);
          printf("K7 LCD presented=%u sink_error=%d camera_only=1 result=%d\n",
                 stats.sink_presented,stats.sink_error,ret);
          printf("K7 LCD TIMING avg_sink_us=%" PRIu64 " max_sink_us=%" PRIu64 " target_fps=30\n",
                 stats.sink_calls ? stats.sink_us / stats.sink_calls : 0,
                 stats.max_sink_us);
          uint64_t display_fps100 = stats.elapsed_us ?
            (uint64_t)stats.sink_presented * 100000000 / stats.elapsed_us : 0;
          printf("K7 LCD RATE display_fps=%" PRIu64 ".%02" PRIu64
                 " rgb_dropped=%u max_display_age_us=%" PRIu64 "\n",
                 display_fps100 / 100, display_fps100 % 100,
                 stats.sink_dropped, stats.max_sink_age_us);
        }
    }
#endif
  atomic_store(&g_capture_stage,K7_CAPTURE_DONE);
  nxmutex_unlock(&g_lock);
  return ret;
}

static int camera_capture(unsigned int wanted, unsigned int seconds,
                          unsigned int scale, unsigned int delay_ms, bool detect_faces,
                          const struct k7_track_config_s *track)
{
  return camera_capture_sink(wanted,seconds,scale,delay_ms,detect_faces,track,NULL,NULL,false);
}

#if defined(CONFIG_EXAMPLES_K7HOST_JPEG) && defined(CONFIG_EXAMPLES_K7DISPLAY)
static int panel_present(void *arg, const uint8_t *rgb, unsigned int width,
                         unsigned int height, size_t stride, size_t bytes)
{
  (void)arg;
  if (atomic_load(&g_panel_stop)) return -ECANCELED;
  struct k7_pipeline_track_snapshot_s target;
  struct k7display_box box;
  const struct k7display_box *overlay = NULL;
  if (k7_pipeline_track_snapshot(&target) == 0 && target.valid &&
      isfinite(target.box.x) && isfinite(target.box.y) &&
      isfinite(target.box.width) && isfinite(target.box.height) &&
      target.box.width > 0 && target.box.height > 0)
    {
      /* Detection uses 160x120. Clip before float-to-int conversion; the
       * display layer applies the same rotation/mirror as the video. */
      float left = fmaxf(0, target.box.x);
      float top = fmaxf(0, target.box.y);
      float right = fminf(160, target.box.x + target.box.width);
      float bottom = fminf(120, target.box.y + target.box.height);
      if (left < right && top < bottom)
        {
          box.x = (int32_t)floorf(left * 4);
          box.y = (int32_t)floorf(top * 4);
          box.width = (int32_t)ceilf(right * 4) - box.x;
          box.height = (int32_t)ceilf(bottom * 4) - box.y;
          overlay = &box;
        }
    }
  int rc=k7display_video_present_rgb888_box(rgb,width,height,stride,bytes,overlay);
  if (rc) atomic_store(&g_panel_preview_result,rc);
  else { int expected=-EAGAIN;
    atomic_compare_exchange_strong(&g_panel_preview_result,&expected,0); }
  return rc;
}

static int lcd_present(void *arg, const uint8_t *rgb, unsigned int width,
                       unsigned int height, size_t stride, size_t bytes)
{
  (void)arg;
  if (atomic_load(&g_lcd_stop)) return -ECANCELED;
  return k7display_video_present_rgb888(rgb,width,height,stride,bytes);
}
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
static int panel_stage_prepare(void *arg,const uint8_t *rgb,unsigned width,unsigned height,
                                size_t stride,size_t bytes,const struct k7ds_epoch *epoch,
                                unsigned sequence,struct k7ds_ticket *ticket)
{
  (void)arg;
  if(atomic_load(&g_panel_stop)){if(ticket)memset(ticket,0,sizeof(*ticket));return -ECANCELED;}
  struct k7_pipeline_track_snapshot_s target;
  struct k7display_box box;
  const struct k7display_box *overlay=NULL;
  if(k7_pipeline_track_snapshot(&target)==0 && target.valid &&
     isfinite(target.box.x) && isfinite(target.box.y) &&
     isfinite(target.box.width) && isfinite(target.box.height) &&
     target.box.width>0 && target.box.height>0)
    {
      float left=fmaxf(0,target.box.x),top=fmaxf(0,target.box.y);
      float right=fminf(160,target.box.x+target.box.width);
      float bottom=fminf(120,target.box.y+target.box.height);
      if(left<right && top<bottom)
        {
          box.x=(int32_t)floorf(left*4);box.y=(int32_t)floorf(top*4);
          box.width=(int32_t)ceilf(right*4)-box.x;
          box.height=(int32_t)ceilf(bottom*4)-box.y;overlay=&box;
        }
    }
  return k7display_video_stage_prepare_box(rgb,width,height,stride,bytes,overlay,epoch,sequence,ticket);
}
static int panel_stage_commit(void *arg,const struct k7ds_ticket *ticket)
{
  if(atomic_load(&g_panel_stop))return -ECANCELED;
  int rc=k7display_video_stage_commit(arg,ticket);
  if(rc && rc!=K7DS_STALE_ERROR && rc!=-ECANCELED && rc!=-ENODEV)
    atomic_store(&g_panel_preview_result,rc);
  else if(!rc)
    {int expected=-EAGAIN;atomic_compare_exchange_strong(&g_panel_preview_result,&expected,0);}
  return rc;
}
#endif
#endif

#ifdef CONFIG_EXAMPLES_K7HOST_JPEG

static int camera_decode(unsigned int scale)
{
  struct k7_jpeg_result_s result;
  uint64_t total = 0, minimum = UINT64_MAX, maximum = 0;
  unsigned int decoded = 0;
  size_t needed = (640 / scale) * (480 / scale) * 3;
  int ret = 0;
  nxmutex_lock(&g_lock);
  if (g_rgb_busy) { ret = -EBUSY; goto done; }
  if (!g_burst_count) { ret = -ENOENT; goto done; }
  g_rgb_len = 0;
  if (g_rgb_capacity < needed)
    {
      uint8_t *replacement = kmm_malloc(needed);
      if (!replacement) { ret = -ENOMEM; goto done; }
      kmm_free(g_rgb);
      g_rgb = replacement;
      g_rgb_capacity = needed;
    }
  for (unsigned int i = 0; i < g_burst_count; i++)
    {
      g_rgb_len = 0;
      ret = k7_jpeg_decode(g_burst_data + i * g_burst_stride,
                           g_burst_lengths[i], scale, g_rgb,
                           g_rgb_capacity, &result);
      uint32_t crc = ret == 0 ? k7_rgb_crc32(g_rgb, result.bytes) : 0;
      usleep(5000);
      printf("K7 JPEG frame=%u scale=%u width=%u height=%u bytes=%u "
             "us=%" PRIu64 " crc32=%08" PRIx32 " warnings=%u result=%d\n",
             i + 1, scale, result.width, result.height,
             (unsigned)result.bytes, result.usec, crc, result.warnings, ret);
      if (ret < 0) break;
      g_rgb_len = result.bytes;
      g_rgb_width = result.width;
      g_rgb_height = result.height;
      total += result.usec;
      if (result.usec < minimum) minimum = result.usec;
      if (result.usec > maximum) maximum = result.usec;
      decoded++;
    }
done:
  usleep(5000);
  printf("K7 DECODE scale=%u decoded=%u avg_us=%" PRIu64
         " min_us=%" PRIu64 " max_us=%" PRIu64 " result=%d\n",
         scale, decoded, decoded ? total / decoded : 0,
         decoded ? minimum : 0, maximum, ret);
  if (g_rgb_len)
    {
      usleep(5000);
      printf("K7 RGB width=%u height=%u bytes=%u\n",
             g_rgb_width, g_rgb_height, (unsigned)g_rgb_len);
    }
  nxmutex_unlock(&g_lock);
  return ret;
}

static int camera_jpegcheck(void)
{
  struct k7_jpeg_result_s result;
  const uint8_t invalid[4] = {0, 0, 0, 0};
  uint8_t *scratch;
  int bad, truncated, valid, ret;
  nxmutex_lock(&g_lock);
  if (!g_burst_count)
    { nxmutex_unlock(&g_lock); return -ENOENT; }
  scratch = kmm_malloc(160 * 120 * 3);
  if (!scratch)
    { nxmutex_unlock(&g_lock); return -ENOMEM; }
  bad = k7_jpeg_decode(invalid, sizeof(invalid), 4, scratch,
                       160 * 120 * 3, &result);
  truncated = k7_jpeg_decode(g_burst_data, 16, 4, scratch,
                             160 * 120 * 3, &result);
  valid = k7_jpeg_decode(g_burst_data, g_burst_lengths[0], 4, scratch,
                         160 * 120 * 3, &result);
  ret = bad < 0 && truncated < 0 && valid == 0 ? 0 : -EIO;
  kmm_free(scratch);
  printf("K7 JPEGCHECK invalid=%d truncated=%d valid_after_errors=%d result=%d\n",
         bad, truncated, valid, ret);
  nxmutex_unlock(&g_lock);
  return ret;
}
#endif

#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
static struct k7_yunet_s *g_face_ctx;
/* Diagnostic raw tensor export: stable until the next detect command. */
static float g_face_raw[8400];
static size_t g_face_raw_count;
static int camera_detect(void)
{
  struct k7_faces_s faces;
  int ret;
  nxmutex_lock(&g_lock);
  if (g_rgb_busy || g_rgb_len != 160 * 120 * 3 ||
      g_rgb_width != 160 || g_rgb_height != 120)
    { ret = -EINVAL; goto done; }
  if (!g_face_ctx) g_face_ctx = k7_yunet_create();
  if (!g_face_ctx) { ret = -ENOMEM; goto done; }
  g_face_raw_count = 0;
  ret = k7_yunet_detect(g_face_ctx, g_rgb, g_rgb_len, &faces);
  if (ret < 0) goto done;
  for (unsigned int i = 0; i < 12; i++)
    {
      size_t n;
      const float *raw = k7_yunet_output(g_face_ctx, i, &n);
      if (!raw || n > 8400 - g_face_raw_count) { ret = -EOVERFLOW; goto done; }
      memcpy(g_face_raw + g_face_raw_count, raw, n * sizeof(float));
      g_face_raw_count += n;
    }
  usleep(5000);
  printf("K7 DETECT count=%u usec=%" PRIu64 " raw_floats=%u\n",
         faces.count, faces.usec, (unsigned)g_face_raw_count);
  for (unsigned int i = 0; i < faces.count; i++)
    {
      struct k7_face_s *f = &faces.face[i];
      usleep(5000);
      printf("K7 FACE index=%u x=%.3f y=%.3f w=%.3f h=%.3f score=%.6f\n",
             i, f->x, f->y, f->width, f->height, f->score);
    }
done:
  usleep(5000);
  printf("K7 DETECT result=%d\n", ret);
  nxmutex_unlock(&g_lock);
  return ret;
}
#endif


int k7host_main(int argc, char *argv[])
{
  int ret;
#if defined(CONFIG_EXAMPLES_K7HOST_JPEG) && defined(CONFIG_EXAMPLES_K7DISPLAY)
  if (argc==2 && !strcmp(argv[1],"paneloff"))
    {
      nxmutex_lock(&g_lcd_lock);
      bool active=atomic_load(&g_panel_active);
      if (active) atomic_store(&g_panel_stop,true);
      nxmutex_unlock(&g_lcd_lock);
      printf("K7 PANEL stop_requested active=%u camera_unchanged=1\n",active);
      return active ? 0:1;
    }
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  if ((argc==3 && (!strcmp(argv[1],"voicepanel") || !strcmp(argv[1],"visionpanel"))) ||
      (argc==2 && !strcmp(argv[1],"voicepanel-service")))
    {
      bool service=argc==2;
      char *end; errno=0; unsigned int seconds=K7_CAPTURE_CONTINUOUS;
      long parsed=service?1:strtol(argv[2],&end,10);
      bool dry=!strcmp(argv[1],"visionpanel");
      if(!service && (errno || !argv[2][0] || *end || parsed<1 || parsed>(dry?1800:10800))) goto usage;
      if(!service)seconds=(unsigned int)parsed;
      nxmutex_lock(&g_lcd_lock);
      if (atomic_load(&g_panel_active))
        { nxmutex_unlock(&g_lcd_lock); return 1; }
      if(service && !capture_service_begin())
        { nxmutex_unlock(&g_lcd_lock); return 1; }
      atomic_store(&g_panel_stop,false);
      atomic_store(&g_panel_preview_result,-EAGAIN);
      atomic_store(&g_panel_active,true);
      nxmutex_unlock(&g_lcd_lock);
      struct k7_track_config_s config={.transmit=!dry,.axes=TRACK_XY,
        .calibrated=true,.photo=!dry,.voice_control=!dry,.min_x=-800,.max_x=800,
        .min_y=-120,.max_y=975,.center_x=0,.center_y=0,.sign_x=1,.sign_y=-1};
#ifdef CONFIG_EXAMPLES_K7BOOT_TRACK_TRIAL_RANGE
      if (!dry) {
        config.min_x=config.min_y=-30;
        config.max_x=config.max_y=30;
        puts("DIAGNOSTIC TRACK RANGE X=-30..30 Y=-30..30; MCU reset zero required; not assessment acceptance");
      }
#endif
      printf("K7 PANEL session dry=%u initial_hold=%u microphone_started=0\n",dry,!dry);
      ret=camera_capture_sink(K7_BURST_FRAMES,seconds,4,0,true,&config,NULL,NULL,true);
      nxmutex_lock(&g_lcd_lock);
      atomic_store(&g_panel_active,false);
      nxmutex_unlock(&g_lcd_lock);
      if(service)capture_service_end(ret);
      return ret<0 ? 1:0;
    }
#endif
  if (argc==2 && (!strcmp(argv[1],"lcdstop") || !strcmp(argv[1],"halt")))
    {
      nxmutex_lock(&g_lcd_lock);
      bool active=g_lcd_active;
      unsigned int request=g_lcd_request;
      if (active) atomic_store(&g_lcd_stop,true);
      nxmutex_unlock(&g_lcd_lock);
      if (active || !strcmp(argv[1],"lcdstop"))
        {
          printf("K7 LCD stop_requested request=%u active=%u\n",request,active);
          return active ? 0:1;
        }
    }
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  if(argc==2 && !strcmp(argv[1],"capture-service-status")) {
    printf("K7 CAPTURE attempted=%u active=%u stop_requested=%u result=%d\n",
      atomic_load(&g_service_attempted),atomic_load(&g_service_active),
      atomic_load(&g_service_stop),atomic_load(&g_service_result));
    capture_diag_print();
    /* The diagnostic endpoint is retained until board reset. */
    k7_xhci_flow_diagnostics(atomic_load(&g_video_diag_ep));
    return 0;
  }
  if(argc==2 && !strcmp(argv[1],"capture-service-stop")) {
    bool active=atomic_load(&g_service_active);
    if(active){k7_pipeline_halt();atomic_store(&g_service_stop,true);}
    printf("K7 CAPTURE stop_requested=%u active=%u\n",active,active);
    return active?0:1;
  }
  /* Keep this preset below NSH's argument limit, including the background &.
   * v1.3-xcenter only: an operator-confirmed MCU reset establishes 0/0.
   */
  if ((argc==3 && (!strcmp(argv[1],"photos") || !strcmp(argv[1],"voicecam"))) ||
      (argc==2 && !strcmp(argv[1],"voicecam-service"))) {
      bool service=argc==2;
      char *end;errno=0;long parsed=service?1:strtol(argv[2],&end,10);
      unsigned int seconds=K7_CAPTURE_CONTINUOUS;
      bool voice = service || !strcmp(argv[1],"voicecam");
      if(!service && (errno||!argv[2][0]||*end||parsed<1||parsed>(voice?10800:300)))goto usage;
      if(!service)seconds=(unsigned int)parsed;
      if(service && !capture_service_begin())return 1;
      struct k7_track_config_s config={.transmit=true,.axes=TRACK_XY,
        .calibrated=true,.photo=true,.voice_control=voice,.min_x=-800,.max_x=800,
        .min_y=-120,.max_y=975,.center_x=0,.center_y=0,.sign_x=1,.sign_y=-1};
#ifdef CONFIG_EXAMPLES_K7BOOT_TRACK_TRIAL_RANGE
      if (voice) {
        config.min_x=config.min_y=-30;
        config.max_x=config.max_y=30;
        puts("DIAGNOSTIC TRACK RANGE X=-30..30 Y=-30..30; MCU reset zero required; not assessment acceptance");
      } else
#endif
      puts("PHOTO PRESET v1.3-xcenter; X=-800..800 Y=-120..975; native pose");
      ret=camera_capture(K7_BURST_FRAMES,seconds,4,0,true,&config);
      if(service)capture_service_end(ret);
      return ret<0 ? 1:0;
    }
  if (argc==3 && !strcmp(argv[1],"voicemode")) {
      unsigned int mode;
      if (!strcmp(argv[2],"hold")) mode=0;
      else if (!strcmp(argv[2],"track")) mode=1;
      else if (!strcmp(argv[2],"photo")) mode=2;
      else goto usage;
      ret=k7_pipeline_voice_mode(mode);
      printf("VOICE MODE requested=%u result=%d\n",mode,ret);
      return ret<0?1:0;
    }
  if (argc >= 2 && (!strcmp(argv[1], "trackcal") || !strcmp(argv[1], "photocal")))
    {
      long v[9];
      if (argc != 12 || (strcmp(argv[2], "run") && strcmp(argv[2], "dry"))) goto usage;
      for (int i=0; i<9; i++)
        {
          char *end;
          errno=0; v[i]=strtol(argv[i+3], &end, 10);
          if (errno || !argv[i+3][0] || *end || v[i]<-2000 || v[i]>2000) goto usage;
        }
      if (v[0]<1 || v[0]>300) goto usage;
      struct k7_track_config_s config={.transmit=!strcmp(argv[2],"run"),
        .axes=TRACK_XY,.calibrated=true,.photo=!strcmp(argv[1],"photocal"),.min_x=v[1],.max_x=v[2],
        .min_y=v[3],.max_y=v[4],.center_x=v[5],.center_y=v[6],
        .sign_x=v[7],.sign_y=v[8]};
      struct k7_track_s check;
      if (k7_track_init(&check,&config,config.center_x,config.center_y)<0) goto usage;
      printf("TRACK CAL X=[%d,%d] Y=[%d,%d] center=%d,%d sign=%d,%d\n",
             config.min_x,config.max_x,config.min_y,config.max_y,
             config.center_x,config.center_y,config.sign_x,config.sign_y);
      return camera_capture(K7_BURST_FRAMES,v[0],4,0,true,&config)<0 ? 1:0;
    }
  if (argc==2 && !strcmp(argv[1],"posecheck")) {
      struct k7_pose_s *pose=k7_pose_create();
      float *input=calloc(12288,sizeof(float)),output[3]={0};
      if(!pose||!input){k7_pose_destroy(pose);free(input);return -ENOMEM;}
      struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
      int result=k7_pose_infer(pose,input,output);
      clock_gettime(CLOCK_MONOTONIC,&b);
      long ms=(b.tv_sec-a.tv_sec)*1000+(b.tv_nsec-a.tv_nsec)/1000000;
      printf("POSECHECK result=%d yaw=%.5f pitch=%.5f roll=%.5f ms=%ld\n",
             result,output[0],output[1],output[2],ms);
      k7_pose_destroy(pose);free(input);return result;
    }
  if (argc==2 && !strcmp(argv[1],"photoreset")) {
      k7_photo_reset();puts("PHOTO reset requested; saved files retained");return 0;
    }
  if (argc==6 && !strcmp(argv[1],"photoack")) {
      unsigned long values[4];
      for(int i=0;i<4;i++) {char *end;errno=0;values[i]=strtoul(argv[2+i],&end,10);
        if(errno||!argv[2+i][0]||*end||values[i]>0xffffffffUL)goto usage;}
      if((values[2]!=1&&values[2]!=2&&values[2]!=4)||values[3]>1)goto usage;
      k7_photo_ack(values[0],values[1],values[2],values[3]);return 0;
    }
  if (argc == 2 && !strcmp(argv[1], "halt"))
    {
      k7_pipeline_halt();
      puts("TRACK halt requested: hold last target; not motor disable.");
      return 0;
    }
  if (argc == 2 && !strcmp(argv[1], "linkvideo"))
    {
      extern int k7_link_video(void);
      ret=k7_link_video();
      printf("linkvideo result=%d\n",ret);
      return ret<0 ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "linkprobe"))
    {
      extern int k7_link_probe(void);
      ret=k7_link_probe();
      printf("linkprobe result=%d\n",ret);
      return ret<0 ? 1 : 0;
    }
  if (argc == 3 && !strcmp(argv[1], "uartbench"))
    {
      char *end;
      long gap=strtol(argv[2],&end,10);
      if (!argv[2][0] || *end || gap<0 || gap>5000) return 1;
      for (unsigned int row=0;row<256;row++)
        {
          char payload[49];
          for (unsigned int j=0;j<48;j++) payload[j]='A'+(row+j)%26;
          payload[48]=0;
          printf("BENCH %03u %s\n",row,payload);
          if(gap) usleep(gap);
        }
      usleep(10000);
      puts("BENCH DONE");
      return 0;
    }
  if (argc >= 2 && (!strcmp(argv[1], "track") || !strcmp(argv[1], "preview")))
    {
      long v[4]={10,30,1,1};
      if (argc < 3 || argc > 8 ||
          (strcmp(argv[2],"dry") && strcmp(argv[2],"run"))) goto usage;
      for (int i=3; i<argc && i<7; i++)
        {
          char *end;
          errno=0;
          v[i-3]=strtol(argv[i],&end,10);
          if (errno || !argv[i][0] || *end) goto usage;
        }
      if (v[0]<1 || v[0]>60 || v[1]<1 || v[1]>150 ||
          (v[2]!=-1 && v[2]!=1) || (v[3]!=-1 && v[3]!=1)) goto usage;
      unsigned int axes=TRACK_XY;
      if (argc==8)
        {
          if (!strcmp(argv[7],"x")) axes=TRACK_X;
          else if (!strcmp(argv[7],"y")) axes=TRACK_Y;
          else if (strcmp(argv[7],"xy")) goto usage;
        }
      struct k7_track_config_s config={
        .transmit=!strcmp(argv[2],"run"),.limit=v[1],
        .sign_x=v[2],.sign_y=v[3],.axes=axes,
        .preview=!strcmp(argv[1],"preview")};
      return camera_capture(K7_BURST_FRAMES,v[0],4,0,true,&config)<0 ? 1:0;
    }
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
#ifdef CONFIG_EXAMPLES_K7DISPLAY
  if (argc >= 2 && !strcmp(argv[1], "lcd"))
    {
      unsigned long seconds=60;
      if (argc==3)
        {
          char *end;
          errno=0; seconds=strtoul(argv[2],&end,10);
          if (errno || !argv[2][0] || *end) goto usage;
        }
      else if (argc!=2) goto usage;
      if (seconds<1 || seconds>1800) goto usage;
      ret=k7display_video_ready();
      if (ret<0) { printf("K7 LCD display_not_ready=%d\n",ret); return 1; }
      nxmutex_lock(&g_lcd_lock);
      if (g_lcd_active)
        { nxmutex_unlock(&g_lcd_lock); puts("K7 LCD busy"); return 1; }
      atomic_store(&g_lcd_stop,false);
      g_lcd_active=true;
      unsigned int request=++g_lcd_request;
      nxmutex_unlock(&g_lcd_lock);
#if defined(K7_DISPLAY_STAGE_SPLIT) && K7_DISPLAY_STAGE_SPLIT
      const struct k7_pipeline_sink_s sink={.arg=NULL,.present=lcd_present};
#else
      const struct k7_pipeline_sink_s sink={NULL,lcd_present};
#endif
      struct k7_pipeline_stats_s stats={0};
      printf("K7 LCD start request=%u seconds=%lu scale=1 target_fps=30 camera_only=1\n",request,seconds);
      ret=camera_capture_sink(1,seconds,1,0,false,NULL,&sink,&stats,false);
      nxmutex_lock(&g_lcd_lock);
      g_lcd_active=false;
      nxmutex_unlock(&g_lcd_lock);
      usleep(5000);
      printf("K7 LCD result request=%u ret=%d presented=%u dropped=%u sink_error=%d\n",
             request,ret,stats.sink_presented,stats.dropped,stats.sink_error);
      return ret<0 ? 1:0;
    }
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
  if (argc == 2 && !strcmp(argv[1], "detect"))
    return camera_detect() < 0 ? 1 : 0;
  if (argc >= 2 && !strcmp(argv[1], "faces"))
    {
      unsigned long seconds = 10;
      if (argc == 3)
        {
          char *end;
          errno = 0;
          seconds = strtoul(argv[2], &end, 10);
          if (errno || !argv[2][0] || *end) goto usage;
        }
      else if (argc != 2) goto usage;
      if (seconds < 1 || seconds > 60) goto usage;
      return camera_capture(K7_BURST_FRAMES, seconds, 4, 0, true, NULL) < 0 ? 1 : 0;
    }
#endif
  if (argc >= 2 && !strcmp(argv[1], "live"))
    {
      unsigned long values[3] = {10, 2, 0};
      if (argc > 5) goto usage;
      for (int i = 2; i < argc; i++)
        {
          char *end;
          errno = 0;
          values[i - 2] = strtoul(argv[i], &end, 10);
          if (errno || !argv[i][0] || *end) goto usage;
        }
      if (values[0] < 1 || values[0] > 60 || values[2] > 200 ||
          (values[1] != 1 && values[1] != 2 &&
           values[1] != 4 && values[1] != 8)) goto usage;
      return camera_capture(K7_BURST_FRAMES, values[0],
                             values[1], values[2], false, NULL) < 0 ? 1 : 0;
    }
  if (argc >= 2 && !strcmp(argv[1], "decode"))
    {
      unsigned int scale = 2;
      if (argc == 3)
        {
          if (!strcmp(argv[2], "1")) scale = 1;
          else if (!strcmp(argv[2], "2")) scale = 2;
          else if (!strcmp(argv[2], "4")) scale = 4;
          else if (!strcmp(argv[2], "8")) scale = 8;
          else goto usage;
        }
      else if (argc != 2) goto usage;
      return camera_decode(scale) < 0 ? 1 : 0;
    }
  if (argc == 2 && !strcmp(argv[1], "jpegcheck"))
    return camera_jpegcheck() < 0 ? 1 : 0;
#endif
  if (argc >= 2 && !strcmp(argv[1], "stream"))
    {
      unsigned long seconds = 10;
      char *end;
      if (argc == 3)
        {
          errno = 0;
          seconds = strtoul(argv[2], &end, 10);
          if (errno || !argv[2][0] || *end) goto usage;
        }
      else if (argc != 2) goto usage;
      if (seconds < 1 || seconds > 1800) goto usage;
      return camera_capture(K7_BURST_FRAMES, seconds, 0, 0, false, NULL) < 0 ? 1 : 0;
    }
  if (argc != 2) goto usage;
  if (!strcmp(argv[1], "snapshot")) return camera_capture(1, 0, 0, 0, false, NULL) < 0 ? 1 : 0;
  if (!strcmp(argv[1], "burst")) return camera_capture(K7_BURST_FRAMES, 0, 0, 0, false, NULL) < 0 ? 1 : 0;
  if (!strcmp(argv[1], "probe")) { camera_probe(); return 0; }
  if (!strcmp(argv[1], "camera")) { camera_info(); return 0; }
  if (!strcmp(argv[1], "start"))
    {
      nxmutex_lock(&g_lock);
      if (!g_registered)
        {
          ret = usbhost_registerclass(&g_registry);
          if (ret < 0) { nxmutex_unlock(&g_lock); return 1; }
          g_registered = true;
        }
      nxmutex_unlock(&g_lock);
      ret = rk3576_usbhost_initialize();
      printf("K7 host start result=%d; run 'k7host camera' after enumeration.\n", ret);
      return ret < 0 ? 1 : 0;
    }
usage:
#ifdef CONFIG_EXAMPLES_K7HOST_TRACK
  puts("TRACK: k7host track dry|run [1..60 seconds] [1..150 limit] [-1|1 X] [-1|1 Y] [xy|x|y]");
  puts("CAL: k7host trackcal dry|run SECONDS XMIN XMAX YMIN YMAX CX CY SX SY (1..300s)");
  puts("VIEW: k7host preview dry|run [1..60 seconds] [1..150 limit] [-1|1 X] [-1|1 Y] [xy|x|y]");
  puts("TRACK: k7host halt (hold; not disable); limits are MCU units, NOT degrees.");
  puts("VOICE: k7host voicecam 1..10800 (v1.3-xcenter reset required); voicemode hold|track|photo");
  puts("SERVICE: voicecam-service | voicepanel-service (boot-owned); capture-service-stop | capture-service-status");
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_YUNET
  puts("FACE: k7host faces [1..60 seconds] | detect (retained RGB160)");
#endif
#ifdef CONFIG_EXAMPLES_K7HOST_JPEG
#ifdef CONFIG_EXAMPLES_K7DISPLAY
  puts("LCD: k7host lcd [1..1800 seconds] (default 60; display already started; camera only)");
  puts("PANEL: voicepanel seconds (starts hold; no microphone), visionpanel seconds (no motor TX), paneloff (display branch only)");
  puts("LCD: k7host lcdstop (stop capture; display keeps last frame)");
#endif
  puts("LIVE: k7host live [1..60 seconds] [1|2|4|8 scale] [0..200 test_delay_ms]");
  puts("JPEG: k7host decode [1|2|4|8] | jpegcheck (after burst/stream)");
#endif
  puts("Usage: k7host start | camera | probe | snapshot | burst | stream [1..1800 seconds]");
  return 1;
}
