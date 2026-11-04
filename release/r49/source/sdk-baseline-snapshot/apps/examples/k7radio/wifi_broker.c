#include "wifi_broker.h"
#include <string.h>

static void zero(void* p, size_t n) {
  volatile unsigned char* q = (volatile unsigned char*)p;
  while(n--) *q++ = 0;
}
void wb_job_clear(struct wb_job* job) { zero(job, sizeof(*job)); }
static struct wb_result result(uint64_t owner, uint64_t id, enum wb_status status) {
  struct wb_result r; memset(&r, 0, sizeof(r)); r.owner=owner; r.id=id; r.status=status; return r;
}
static int match(const struct wb_broker* b, uint64_t owner, uint64_t id) {
  return id && owner && b->active.id==id && b->active.owner==owner;
}
static void remember(struct wb_broker* b, struct wb_result r) {
  b->history[b->history_next]=r; b->history_next=(b->history_next+1)%8;
}
static void free_radio(struct wb_broker* b) {
  remember(b,b->active); memset(&b->active,0,sizeof(b->active));
  wb_job_clear(&b->pending); b->radio=WB_FREE;
}
static void terminate(struct wb_broker* b, enum wb_status s) {
  b->active.status=s; memset(b->active.ip,0,4); wb_job_clear(&b->pending);
  if(b->radio==WB_QUEUED) free_radio(b); else b->radio=WB_DRAINING;
}
void wb_init(struct wb_broker* b, uint64_t now, int proven_offline) {
  memset(b,0,sizeof(*b)); b->next_id=1; b->last_time=now;
  b->radio=proven_offline?WB_FREE:WB_EXTERNAL;
}
void wb_external_idle(struct wb_broker* b) { if(b->radio==WB_EXTERNAL) b->radio=WB_FREE; }
void wb_tick(struct wb_broker* b, uint64_t now) {
  if(now<b->last_time) {
    b->clock_fault=1;
    if(b->radio==WB_QUEUED || b->radio==WB_RUNNING) terminate(b,WB_CLOCK_ERROR);
  }
  if(!b->clock_fault) {
    b->last_time=now;
    if((b->radio==WB_QUEUED || b->radio==WB_RUNNING) && now-b->started>=b->timeout)
      terminate(b,WB_TIMEOUT);
  }
}
struct wb_result wb_begin(struct wb_broker* b, uint64_t owner, const char* ssid,
  size_t slen, const char* password, size_t plen, uint64_t now, uint64_t timeout) {
  struct wb_result r; uint64_t id;
  wb_tick(b,now);
  if(!b->next_id) return result(owner,0,WB_EXHAUSTED);
  id=b->next_id; b->next_id=(id==UINT64_MAX)?0:id+1;
  r=result(owner,id,WB_RECEIVED);
  if(b->clock_fault) r.status=WB_CLOCK_ERROR;
  else if(!owner || !ssid || slen==0 || slen>32 || !timeout ||
          (plen && !password) || plen>63) r.status=WB_INVALID;
  else if(b->radio!=WB_FREE) r.status=WB_BUSY;
  else if(!plen) r.status=WB_UNSUPPORTED;
  else if(plen<8 || memchr(password,0,plen)) r.status=WB_INVALID;
  if(r.status!=WB_RECEIVED) { remember(b,r); return r; }
  wb_job_clear(&b->pending);
  b->pending.owner=owner; b->pending.id=id;
  b->pending.ssid_len=slen; b->pending.password_len=plen;
  memcpy(b->pending.ssid,ssid,slen); memcpy(b->pending.password,password,plen);
  b->active=r; b->started=now; b->timeout=timeout; b->event_sequence=0; b->radio=WB_QUEUED;
  return r;
}
struct wb_result wb_poll(const struct wb_broker* b, uint64_t owner, uint64_t id) {
  unsigned i;
  if(match(b,owner,id)) return b->active;
  for(i=0;i<8;i++) if(id && owner && b->history[i].id==id && b->history[i].owner==owner) return b->history[i];
  return result(owner,id,WB_UNKNOWN);
}
int wb_take_job(struct wb_broker* b, struct wb_job* job, uint64_t now) {
  wb_tick(b,now);
  if(b->radio!=WB_QUEUED) return 0; /* output untouched on failure */
  *job=b->pending; wb_job_clear(&b->pending);
  b->active.status=WB_CONNECTING; b->radio=WB_RUNNING; return 1;
}
void wb_start_failed(struct wb_broker* b, uint64_t owner, uint64_t id, uint64_t now) {
  wb_tick(b,now);
  if(match(b,owner,id) && b->radio==WB_QUEUED) terminate(b,WB_FAILED);
}
void wb_cancel(struct wb_broker* b, uint64_t owner, uint64_t id, uint64_t now) {
  wb_tick(b,now);
  if(match(b,owner,id) && (b->radio==WB_RUNNING || b->radio==WB_QUEUED)) terminate(b,WB_CANCELLED);
}
int wb_release(struct wb_broker* b, uint64_t owner, uint64_t id, uint64_t now) {
  wb_tick(b,now);
  if(!match(b,owner,id) || b->radio!=WB_OWNED) return 0;
  terminate(b,WB_CANCELLED); return 1;
}
int wb_stop_requested(const struct wb_broker* b, uint64_t owner, uint64_t id) {
  return match(b,owner,id) && b->radio==WB_DRAINING;
}
static int valid_ip(const uint8_t ip[4]) {
  return ip[0] && ip[0]!=127 && ip[0]<224 && !(ip[0]==169 && ip[1]==254);
}
void wb_complete(struct wb_broker* b, const struct wb_done* d, uint64_t now) {
  wb_tick(b,now);
  if(!match(b,d->owner,d->id)) return;
  if(b->radio==WB_QUEUED || !d->sequence || d->sequence<=b->event_sequence) return;
  b->event_sequence=d->sequence;
  if(b->radio==WB_DRAINING) {
    if(d->worker_exited && !d->link_up) free_radio(b);
    return;
  }
  if(b->radio==WB_OWNED) {
    if(!d->link_up) {
      terminate(b,WB_FAILED);
      if(d->worker_exited) free_radio(b);
    }
    return;
  }
  if(b->radio!=WB_RUNNING) return;
  if(d->success && d->authenticated && d->dhcp && d->worker_exited && d->link_up && valid_ip(d->ip)) {
    b->active.status=WB_IP_READY; memcpy(b->active.ip,d->ip,4); b->radio=WB_OWNED; return;
  }
  terminate(b,WB_FAILED);
  if(d->worker_exited && !d->link_up) free_radio(b);
}
