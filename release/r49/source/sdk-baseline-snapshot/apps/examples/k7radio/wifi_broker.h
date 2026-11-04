#ifndef WIFI_BROKER_H
#define WIFI_BROKER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum wb_status { WB_RECEIVED, WB_CONNECTING, WB_IP_READY, WB_FAILED,
  WB_CANCELLED, WB_TIMEOUT, WB_BUSY, WB_UNSUPPORTED, WB_INVALID,
  WB_EXHAUSTED, WB_CLOCK_ERROR, WB_UNKNOWN };
enum wb_radio { WB_EXTERNAL, WB_FREE, WB_QUEUED, WB_RUNNING, WB_DRAINING, WB_OWNED, WB_SCAN };
struct wb_result { uint64_t owner, id; enum wb_status status; uint8_t ip[4]; };
struct wb_job { uint64_t owner, id; size_t ssid_len, password_len; char ssid[33], password[64]; };
struct wb_done { uint64_t owner, id, sequence; int success, authenticated, dhcp, worker_exited, link_up; uint8_t ip[4]; };
struct wb_broker {
  enum wb_radio radio;
  uint64_t next_id, last_time, started, timeout, event_sequence;
  int clock_fault;
  struct wb_result active, history[8];
  unsigned history_next;
  struct wb_job pending;
};
/* Serialized service-task calls only. No backend calls, malloc or logging. */
void wb_init(struct wb_broker*, uint64_t now, int proven_offline);
void wb_external_idle(struct wb_broker*); /* trusted supervisor only */
struct wb_result wb_begin(struct wb_broker*, uint64_t owner, const char* ssid,
  size_t ssid_len, const char* password, size_t password_len, uint64_t now, uint64_t timeout);
struct wb_result wb_poll(const struct wb_broker*, uint64_t owner, uint64_t id);
void wb_tick(struct wb_broker*, uint64_t now);
int wb_take_job(struct wb_broker*, struct wb_job*, uint64_t now);
void wb_start_failed(struct wb_broker*, uint64_t owner, uint64_t id, uint64_t now);
void wb_cancel(struct wb_broker*, uint64_t owner, uint64_t id, uint64_t now);
int wb_release(struct wb_broker*, uint64_t owner, uint64_t id, uint64_t now);
int wb_stop_requested(const struct wb_broker*, uint64_t owner, uint64_t id);
void wb_complete(struct wb_broker*, const struct wb_done*, uint64_t now);
void wb_job_clear(struct wb_job*);
#ifdef __cplusplus
}
#endif
#endif
