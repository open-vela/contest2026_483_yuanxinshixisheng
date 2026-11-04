#include "radio_fence.h"
#include <string.h>
void rf_init(struct rf_facts*f){memset(f,0,sizeof(*f));}
void rf_command(struct rf_facts*f,unsigned cmd,int rc,uint64_t epoch){
 if(cmd==3){f->touched=true;f->stop_ok=true;f->close_ok=false;f->rx_empty=false;f->callbacks_exited=false;}
 if(cmd==5){f->stop_ok=false;f->scan_done=false;}
 if(cmd==6)f->stop_ok=rc==0;
 if(cmd==4){f->close_ok=rc==0;f->close_epoch=epoch;f->rx_empty=false;f->callbacks_exited=false;}
 if(cmd==9){f->unjoined=false;f->keys_clean=false;}
 if(cmd==17)f->unjoined=rc==0;
}
bool rf_scan_releasable(const struct rf_facts*f){
 return f->worker_joined && (!f->touched || (f->close_ok && (f->stop_ok||f->scan_done)
   && f->rx_empty && f->empty_epoch>f->close_epoch && f->callbacks_exited));
}
bool rf_connect_releasable(const struct rf_facts*f){
 return rf_scan_releasable(f) && f->maintenance_joined && f->keys_clean && f->unjoined;
}
