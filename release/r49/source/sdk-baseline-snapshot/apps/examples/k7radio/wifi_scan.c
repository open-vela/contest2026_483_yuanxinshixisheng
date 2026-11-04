#include "wifi_scan.h"
#include <string.h>
static void lock(struct ws_service*s){s->d->port.lock(s->d->port.ctx);}
static void unlock(struct ws_service*s){s->d->port.unlock(s->d->port.ctx);}
int ws_attach(struct ws_service*s,struct k7wd_dispatch*d,const struct ws_port*p){
 if(!s||!d||!p||!p->submit||!p->stop||d->scan)return -1;
 memset(s,0,sizeof(*s));s->d=d;s->port=*p;s->view.state=VS_NOT_READY;
 sc_init(&s->collector);d->scan=s;return 0;
}
static enum k7wd_rc begin(struct ws_service*s,uint64_t owner,uint64_t timeout,uint64_t*id){
 uint64_t now;if(!s||!id)return WD_INVALID;*id=0;now=s->d->port.now_ms(s->d->port.ctx);lock(s);
 if(!s->d->next_ticket){unlock(s);return WD_EXHAUSTED;}
 *id=s->d->next_ticket;s->d->next_ticket=*id==UINT64_MAX?0:*id+1;
 if(!timeout){unlock(s);return WD_INVALID;}
 if(s->pending||s->view.held){unlock(s);return WD_BUSY;}
 memset(&s->view,0,sizeof(s->view));s->view.ticket=s->view.generation=*id;
 s->view.state=VS_PENDING;s->owner=owner;s->submitted=s->last_time=now;s->timeout=timeout;
 s->pending=true;s->cancel=s->stop_sent=s->event_pending=false;s->sequence=0;
 unlock(s);return WD_OK;
}
static enum k7wd_rc poll(struct ws_service*s,uint64_t owner,uint64_t id,struct vs_snapshot*out){
 if(!s||!out||!id)return WD_INVALID;
 lock(s);
 if(s->view.ticket!=id){unlock(s);return WD_UNKNOWN;}
 if(s->owner!=owner){unlock(s);return WD_FORBIDDEN;}
 *out=s->view;unlock(s);return WD_OK;
}
static enum k7wd_rc cancel(struct ws_service*s,uint64_t owner,uint64_t id){
 if(!s||!id)return WD_INVALID;
 lock(s);
 if(s->view.ticket!=id){unlock(s);return WD_UNKNOWN;}
 if(s->owner!=owner){unlock(s);return WD_FORBIDDEN;}
 if(s->view.state==VS_PENDING){s->cancel=true;s->view.state=VS_CANCELLED;
  if(s->collector.state==SC_COLLECTING)(void)sc_cancel(&s->collector,id);}
 unlock(s);return WD_OK;
}
enum k7wd_rc ws_voice_begin(struct ws_service*s,uint64_t t,uint64_t*i){return begin(s,2,t,i);}
enum k7wd_rc ws_ble_begin(struct ws_service*s,uint64_t t,uint64_t*i){return begin(s,1,t,i);}
enum k7wd_rc ws_voice_poll(struct ws_service*s,uint64_t i,struct vs_snapshot*v){return poll(s,2,i,v);}
enum k7wd_rc ws_ble_poll(struct ws_service*s,uint64_t i,struct vs_snapshot*v){return poll(s,1,i,v);}
enum k7wd_rc ws_voice_cancel(struct ws_service*s,uint64_t i){return cancel(s,2,i);}
enum k7wd_rc ws_ble_cancel(struct ws_service*s,uint64_t i){return cancel(s,1,i);}
enum sc_rc ws_record(struct ws_service*s,uint64_t id,const struct sc_record*r){
 enum sc_rc rc;if(!s)return SC_INVALID;lock(s);
 rc=s->view.held&&s->view.state==VS_PENDING&&s->view.ticket==id?sc_accept(&s->collector,id,r):SC_STALE;
 unlock(s);return rc;
}
enum k7wd_rc ws_post(struct ws_service*s,const struct ws_done*e){
 if(!s||!e||!e->ticket||!e->sequence)return WD_INVALID;
 lock(s);
 if(e->ticket!=s->view.ticket||!s->view.held||e->sequence<=s->sequence){unlock(s);return WD_UNKNOWN;}
 if(s->event_pending){unlock(s);return WD_FULL;}
 s->event=*e;s->event_pending=true;unlock(s);return WD_OK;
}
void ws_pump(struct ws_service*s,uint64_t now){
 bool submit=false,stop_req=false;uint64_t id;struct wb_broker*b=&s->d->broker;
 lock(s);id=s->view.ticket;
 if((s->pending||s->view.held)&&s->view.state==VS_PENDING &&
    (now<s->last_time||now-s->submitted>=s->timeout)){
  s->view.state=now<s->last_time?VS_FAILED:VS_TIMEOUT;s->view.error=now<s->last_time?-5:-110;s->cancel=true;
  if(s->collector.state==SC_COLLECTING)(void)sc_cancel(&s->collector,id);
 }
 s->last_time=now;
 if(s->pending){
  s->pending=false;
  if(s->view.state==VS_PENDING){
   if(b->radio==WB_OWNED)s->view.state=VS_UNSUPPORTED;
   else if(b->radio!=WB_FREE || b->clock_fault)s->view.state=VS_BUSY;
   else if(sc_begin(&s->collector,id)!=SC_OK)s->view.state=VS_FAILED;
   else {b->radio=WB_SCAN;s->view.held=1;submit=true;}
  }
 }
 if(s->event_pending){
  struct ws_done e=s->event;s->event_pending=false;
  if(e.ticket==id&&e.sequence>s->sequence&&s->view.held){
   s->sequence=e.sequence;
   if(!e.success && s->view.state==VS_PENDING)s->view.error=e.error?e.error:-5;
   if(e.stop_ok&&e.close_ok&&e.worker_exited&&e.rx_quiescent&&e.offline){
    if(s->view.state==VS_PENDING){
     if(e.success&&e.scan_done&&sc_freeze(&s->collector,id)==SC_OK){
      s->view.state=VS_DONE;s->view.count=s->collector.count;s->view.truncated=s->collector.truncated;
      for(unsigned i=0;i<s->view.count;i++){
       const struct sc_record*r=&s->collector.records[i];struct vs_ap*a=&s->view.ap[i];
       memcpy(a->ssid,r->ssid,32);a->ssid_len=r->ssid_len;memcpy(a->bssid,r->bssid,6);
       a->rssi=r->rssi;a->security=r->security;a->channel=r->channel;a->band=r->band;
      }
     }else {s->view.state=VS_FAILED;if(!s->view.error)s->view.error=e.error?e.error:-5;}
    }
    if(s->collector.state==SC_COLLECTING)(void)sc_cancel(&s->collector,id);
    s->view.held=0;if(b->radio==WB_SCAN)b->radio=WB_FREE;
   }else if(!e.success){s->view.state=s->view.state==VS_PENDING?VS_FAILED:s->view.state;s->cancel=true;}
  }
 }
 if(s->view.held&&s->cancel&&!s->stop_sent)stop_req=true;
 unlock(s);
 if(submit && s->port.submit(s->port.ctx,id)){
  /* Contract: rejected submit created no worker/interface, therefore no fence owed. */
  lock(s);if(s->view.state==VS_PENDING){s->view.state=VS_FAILED;s->view.error=-5;}
  (void)sc_cancel(&s->collector,id);s->view.held=0;if(b->radio==WB_SCAN)b->radio=WB_FREE;unlock(s);
 }
 if(stop_req){int rc=s->port.stop(s->port.ctx,id);lock(s);if(!rc)s->stop_sent=true;unlock(s);}
}
