#include "wifi_dispatch.h"
#include "wifi_scan.h"
#include <string.h>
#define BLE_OWNER UINT64_C(1)
#define VOICE_OWNER UINT64_C(2)
static void lock(struct k7wd_dispatch *d){d->port.lock(d->port.ctx);}
static void unlock(struct k7wd_dispatch *d){d->port.unlock(d->port.ctx);}
static void wipe(void *p,size_t n){volatile unsigned char *b=p;while(n--)*b++=0;}
static struct k7wd_slot *find(struct k7wd_dispatch *d,uint64_t id){
 unsigned i;for(i=0;i<WD_RESULTS;i++)if(id && d->slots[i].view.ticket==id)return &d->slots[i];return NULL;
}
int k7wd_init(struct k7wd_dispatch *d,const struct k7wd_port *p,bool offline){
 if(!d || !p || !p->lock || !p->unlock || !p->now_ms || !p->submit || !p->stop)return -1;
 memset(d,0,sizeof(*d));d->port=*p;d->next_ticket=1;wb_init(&d->broker,p->now_ms(p->ctx),offline);return 0;
}
static enum k7wd_rc begin(struct k7wd_dispatch *d,uint64_t owner,const char *ssid,size_t slen,
                       const char *password,size_t plen,uint64_t timeout,uint64_t *id){
 struct k7wd_slot *slot=NULL;struct k7wd_command *cmd;unsigned i;uint64_t submitted;
 if(!id)return WD_INVALID;
 *id=0;if(!d)return WD_INVALID;
 submitted=d->port.now_ms(d->port.ctx);lock(d);
 if(!d->next_ticket){unlock(d);return WD_EXHAUSTED;}
 *id=d->next_ticket;d->next_ticket=d->next_ticket==UINT64_MAX?0:d->next_ticket+1;
 if(!ssid || !slen || slen>32 || !password || plen<8 || plen>63 ||
    !timeout || memchr(password,0,plen)){unlock(d);return WD_INVALID;}
 if(d->rq_count==WD_REQUESTS){++d->request_full;unlock(d);return WD_FULL;}
 for(i=0;i<WD_RESULTS;i++)if(!d->slots[i].live &&
   (!slot || d->slots[i].view.ticket<slot->view.ticket))slot=&d->slots[i];
 if(!slot){++d->request_full;unlock(d);return WD_FULL;}
 memset(slot,0,sizeof(*slot));slot->owner=owner;slot->live=true;
 slot->view.ticket=*id;
 slot->view.phase=WD_QUEUED;slot->view.status=WB_RECEIVED;slot->view.held=true;
 cmd=&d->requests[(d->rq_head+d->rq_count)%WD_REQUESTS];memset(cmd,0,sizeof(*cmd));
 cmd->ticket=slot->view.ticket;cmd->owner=owner;cmd->submitted=submitted;cmd->timeout=timeout;
 cmd->credentials.ssid_len=slen;cmd->credentials.password_len=plen;
 memcpy(cmd->credentials.ssid,ssid,slen);memcpy(cmd->credentials.password,password,plen);
 ++d->rq_count;*id=slot->view.ticket;unlock(d);return WD_OK;
}
enum k7wd_rc k7wd_ble_begin(struct k7wd_dispatch *d,const char *s,size_t n,const char *p,size_t m,uint64_t t,uint64_t *id){return begin(d,BLE_OWNER,s,n,p,m,t,id);}
enum k7wd_rc k7wd_voice_begin(struct k7wd_dispatch *d,const char *s,size_t n,const char *p,size_t m,uint64_t t,uint64_t *id){return begin(d,VOICE_OWNER,s,n,p,m,t,id);}
static enum k7wd_rc poll(struct k7wd_dispatch *d,uint64_t owner,uint64_t id,struct k7wd_view *out){
 struct k7wd_slot *s;if(!d || !out)return WD_INVALID;lock(d);s=find(d,id);
 if(!s){unlock(d);return WD_UNKNOWN;}if(s->owner!=owner){unlock(d);return WD_FORBIDDEN;}
 *out=s->view;unlock(d);return WD_OK;
}
enum k7wd_rc k7wd_ble_poll(struct k7wd_dispatch *d,uint64_t id,struct k7wd_view *v){return poll(d,BLE_OWNER,id,v);}
enum k7wd_rc k7wd_voice_poll(struct k7wd_dispatch *d,uint64_t id,struct k7wd_view *v){return poll(d,VOICE_OWNER,id,v);}
static enum k7wd_rc flag(struct k7wd_dispatch *d,uint64_t owner,uint64_t id,bool release){
 struct k7wd_slot *s;unsigned i;if(!d)return WD_INVALID;lock(d);s=find(d,id);
 if(!s){unlock(d);return WD_UNKNOWN;}if(s->owner!=owner){unlock(d);return WD_FORBIDDEN;}
 if(release)s->release=true;else s->cancel=true;
 /* Queue cancellation never needs a free command slot. Wipe credentials still
  * owned by the queue; an already dequeued job belongs to the owner/worker. */
 if(!release)for(i=0;i<d->rq_count;i++){
  struct k7wd_command *c=&d->requests[(d->rq_head+i)%WD_REQUESTS];
  if(c->ticket==id)wb_job_clear(&c->credentials);
 }
 unlock(d);return WD_OK;
}
enum k7wd_rc k7wd_ble_cancel(struct k7wd_dispatch *d,uint64_t id){return flag(d,BLE_OWNER,id,false);}
enum k7wd_rc k7wd_voice_cancel(struct k7wd_dispatch *d,uint64_t id){return flag(d,VOICE_OWNER,id,false);}
enum k7wd_rc k7wd_ble_release(struct k7wd_dispatch *d,uint64_t id){return flag(d,BLE_OWNER,id,true);}
enum k7wd_rc k7wd_voice_release(struct k7wd_dispatch *d,uint64_t id){return flag(d,VOICE_OWNER,id,true);}
enum k7wd_rc k7wd_post(struct k7wd_dispatch *d,const struct k7wd_event *e){
 if(!d || !e || (e->kind!=WD_PROGRESS && e->kind!=WD_COMPLETION) ||
    !e->done.owner || !e->done.id || !e->done.sequence)return WD_INVALID;
 lock(d);if(d->ev_count==WD_EVENTS){++d->event_full;unlock(d);return WD_FULL;}
 d->events[(d->ev_head+d->ev_count)%WD_EVENTS]=*e;++d->ev_count;unlock(d);return WD_OK;
}
static void publish(struct k7wd_dispatch *d,uint64_t ticket,struct wb_result r,bool held){
 struct k7wd_slot *s;lock(d);s=find(d,ticket);
 if(s){s->view.phase=WD_BROKER;s->view.status=r.status;s->view.held=held;
  memcpy(s->view.ip,r.ip,4);s->live=held;}
 unlock(d);
}
static bool cancelled(struct k7wd_dispatch *d,uint64_t ticket,bool *release){
 struct k7wd_slot *s;bool cancel=false;*release=false;lock(d);s=find(d,ticket);
 if(s){cancel=s->cancel;*release=s->release;}unlock(d);return cancel;
}
enum k7wd_rc k7wd_pump(struct k7wd_dispatch *d){
 struct k7wd_command cmd;struct k7wd_event event;struct wb_job job;
 bool have_cmd=false,have_event=false,release=false;uint64_t now,old_id=0,old_owner=0;
 if(!d)return WD_INVALID;
 memset(&cmd,0,sizeof(cmd));memset(&event,0,sizeof(event));memset(&job,0,sizeof(job));
 lock(d);if(d->pumping){unlock(d);return WD_BUSY;}d->pumping=true;
 if(d->rq_count){cmd=d->requests[d->rq_head];wipe(&d->requests[d->rq_head],sizeof(cmd));
  d->rq_head=(d->rq_head+1)%WD_REQUESTS;--d->rq_count;have_cmd=true;}
 if(d->ev_count){event=d->events[d->ev_head];memset(&d->events[d->ev_head],0,sizeof(event));
  d->ev_head=(d->ev_head+1)%WD_EVENTS;--d->ev_count;have_event=true;}
 unlock(d);
 now=d->port.now_ms(d->port.ctx);wb_tick(&d->broker,now);
 if(d->active_ticket){
  old_id=d->broker.active.id;old_owner=d->broker.active.owner;
  if(cancelled(d,d->active_ticket,&release))wb_cancel(&d->broker,old_owner,old_id,now);
  if(release)wb_release(&d->broker,old_owner,old_id,now);
 }
 if(have_event && event.done.owner==d->broker.active.owner && event.done.id==d->broker.active.id &&
    event.done.sequence>d->sequence){
  d->sequence=event.done.sequence;
  if(event.kind==WD_COMPLETION)wb_complete(&d->broker,&event.done,now);
 }
 if(d->active_ticket){
  const bool held=d->broker.radio!=WB_FREE;
  publish(d,d->active_ticket,wb_poll(&d->broker,old_owner,old_id),held);
  if(!held){d->active_ticket=0;d->stop_sent=false;}
 }
 if(d->scan)ws_pump(d->scan,now); /* same pump owner as connection broker */
 if(have_cmd){
  struct wb_result r;memset(&r,0,sizeof(r));r.owner=cmd.owner;
  if(cancelled(d,cmd.ticket,&release))r.status=WB_CANCELLED;
  else if(now<cmd.submitted)r.status=WB_CLOCK_ERROR;
  else if(now-cmd.submitted>=cmd.timeout)r.status=WB_TIMEOUT;
  else r=wb_begin(&d->broker,cmd.owner,cmd.credentials.ssid,cmd.credentials.ssid_len,
                  cmd.credentials.password,cmd.credentials.password_len,now,cmd.timeout-(now-cmd.submitted));
  wb_job_clear(&cmd.credentials);
  if(r.status==WB_RECEIVED){
   d->active_ticket=cmd.ticket;d->sequence=0;d->stop_sent=false;
   if(wb_take_job(&d->broker,&job,now)){
    int rc=d->port.submit(d->port.ctx,&job);
    if(rc){struct wb_done failed;memset(&failed,0,sizeof(failed));
     failed.owner=job.owner;failed.id=job.id;failed.sequence=1;failed.worker_exited=1;
     wb_complete(&d->broker,&failed,d->port.now_ms(d->port.ctx));}
    wb_job_clear(&job);
   }
   r=wb_poll(&d->broker,cmd.owner,r.id);
   publish(d,cmd.ticket,r,d->broker.radio!=WB_FREE);
   if(d->broker.radio==WB_FREE)d->active_ticket=0;
  }else publish(d,cmd.ticket,r,false);
 }
 if(d->broker.radio==WB_DRAINING && !d->stop_sent)
  d->stop_sent=d->port.stop(d->port.ctx,d->broker.active.owner,d->broker.active.id)==0;
 wipe(&cmd,sizeof(cmd));wb_job_clear(&job);
 lock(d);d->pumping=false;unlock(d);return WD_OK;
}
