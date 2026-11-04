#ifndef WIFI_DISPATCH_H
#define WIFI_DISPATCH_H
#include "wifi_broker.h"
#include <stdbool.h>
#define WD_REQUESTS 8u
#define WD_EVENTS 16u
#define WD_RESULTS 16u
enum k7wd_rc { WD_OK,WD_FULL,WD_INVALID,WD_UNKNOWN,WD_FORBIDDEN,WD_BUSY,WD_EXHAUSTED };
enum k7wd_phase { WD_QUEUED,WD_BROKER };
enum k7wd_event_kind { WD_PROGRESS,WD_COMPLETION };
struct k7wd_view { uint64_t ticket;enum k7wd_phase phase;enum wb_status status;bool held;uint8_t ip[4]; };
struct k7wd_event { enum k7wd_event_kind kind;struct wb_done done; };
struct k7wd_port {
 void *ctx;
 void (*lock)(void *);void (*unlock)(void *);
 uint64_t (*now_ms)(void *); /* thread-safe, monotonic, nonblocking */
 /* Outside queue lock, nonblocking. Copy job before return=0; nonzero MUST
  * prove no worker or link was started. Backend clears its own copy.
  * Stop return=0 only acknowledges receipt; it is NOT a quiescence fence. */
 int (*submit)(void *,const struct wb_job *);
 int (*stop)(void *,uint64_t owner,uint64_t broker_id);
};
struct k7wd_command {uint64_t ticket,owner,submitted,timeout;struct wb_job credentials;};
struct k7wd_slot {uint64_t owner;struct k7wd_view view;bool live,cancel,release;};
struct k7wd_dispatch {
 struct ws_service *scan; /* Optional attached shared scan extension, before publication. */
 struct k7wd_port port;
 /* Only pump touches broker/active/sequence/stop_sent outside queue lock. */
 struct wb_broker broker;uint64_t active_ticket,sequence;bool stop_sent;
 struct k7wd_command requests[WD_REQUESTS];unsigned rq_head,rq_count;
 struct k7wd_event events[WD_EVENTS];unsigned ev_head,ev_count;
 struct k7wd_slot slots[WD_RESULTS];uint64_t next_ticket;
 bool pumping;
 uint64_t request_full,event_full; /* queue lock protected */
};
/* Call before threads start. proven_offline requires supervisor proof that all
 * legacy join/maintenance workers exited AND link offline. No takeover API. */
int k7wd_init(struct k7wd_dispatch *,const struct k7wd_port *,bool proven_offline);
/* Trusted entry points: never accept owner IDs from BLE JSON or speech text.
 * On WD_OK credentials are copied; caller must wipe its own input afterwards.
 * All valid-context attempts consume a unique ticket, including rejection.
 * Rejections are returned inline, not retained in query history. Only exhausted
 * IDs / invalid API pointers produce ticket 0. No alias to another request. */
enum k7wd_rc k7wd_ble_begin(struct k7wd_dispatch *,const char *,size_t,const char *,size_t,uint64_t,uint64_t *);
enum k7wd_rc k7wd_voice_begin(struct k7wd_dispatch *,const char *,size_t,const char *,size_t,uint64_t,uint64_t *);
enum k7wd_rc k7wd_ble_poll(struct k7wd_dispatch *,uint64_t,struct k7wd_view *);
enum k7wd_rc k7wd_voice_poll(struct k7wd_dispatch *,uint64_t,struct k7wd_view *);
enum k7wd_rc k7wd_ble_cancel(struct k7wd_dispatch *,uint64_t);
enum k7wd_rc k7wd_voice_cancel(struct k7wd_dispatch *,uint64_t);
enum k7wd_rc k7wd_ble_release(struct k7wd_dispatch *,uint64_t);
enum k7wd_rc k7wd_voice_release(struct k7wd_dispatch *,uint64_t);
/* Trusted backend publisher, strictly increasing per-job sequence. On FULL
 * retain/retry the exact completion until accepted; never lose an exit fence.
 * PROGRESS never calls wb_complete or promotes status to IP_READY.
 * COMPLETION carries a complete authoritative snapshot, not accumulated bits.
 */
enum k7wd_rc k7wd_post(struct k7wd_dispatch *,const struct k7wd_event *);
/* One owner invocation processes at most one request and one event. Overlap
 * or callback reentry returns BUSY. Does not wait for worker or network. */
enum k7wd_rc k7wd_pump(struct k7wd_dispatch *);
#endif
