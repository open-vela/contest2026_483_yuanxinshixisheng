#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H
#include "wifi_dispatch.h"
#include "scan_contract.h"
#include "scan_collector.h"
/* No hardware backend is supplied. submit nonzero MUST mean nothing started;
 * accepted submit copies ticket before return. stop only requests cancellation.
 * Completion is supervisor-authored after actual worker/RX fences, not worker
 * return-soon flags. No producer may relabel old firmware reports as new IDs. */
struct ws_port {void *ctx;int (*submit)(void*,uint64_t);int (*stop)(void*,uint64_t);};
struct ws_done {uint64_t ticket,sequence;int error;bool success,scan_done,stop_ok,close_ok,worker_exited,rx_quiescent,offline;};
struct ws_service {
 struct k7wd_dispatch*d;struct ws_port port;struct sc_collector collector;
 struct vs_snapshot view;uint64_t owner,submitted,timeout,last_time,sequence;
 bool pending,cancel,stop_sent,event_pending;struct ws_done event;
};
/* Before threads: d initialized and not attached. Never attach around a live pump. */
int ws_attach(struct ws_service*,struct k7wd_dispatch*,const struct ws_port*);
enum k7wd_rc ws_voice_begin(struct ws_service*,uint64_t timeout,uint64_t*ticket);
enum k7wd_rc ws_ble_begin(struct ws_service*,uint64_t timeout,uint64_t*ticket);
enum k7wd_rc ws_voice_poll(struct ws_service*,uint64_t,struct vs_snapshot*);
enum k7wd_rc ws_ble_poll(struct ws_service*,uint64_t,struct vs_snapshot*);
enum k7wd_rc ws_voice_cancel(struct ws_service*,uint64_t);
enum k7wd_rc ws_ble_cancel(struct ws_service*,uint64_t);
/* Fixed-capacity publishers under same dispatcher lock; FULL requires retry
 * exact completion until accepted, never drop a cleanup fence. */
enum sc_rc ws_record(struct ws_service*,uint64_t,const struct sc_record*);
enum k7wd_rc ws_post(struct ws_service*,const struct ws_done*);
/* Internal only: called by k7wd_pump's single-owner path, not clients. */
void ws_pump(struct ws_service*,uint64_t now);
#endif
