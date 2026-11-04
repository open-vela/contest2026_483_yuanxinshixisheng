#ifndef RADIO_FENCE_H
#define RADIO_FENCE_H
#include <stdbool.h>
#include <stdint.h>
/* Owner-serialized command outcomes. An attempted OPEN may have executed even
 * if its ACK timed out. A local FIFO-empty observation alone is not firmware
 * proof against firmware bugs. This version follows the frozen official
 * STOP/DONE ordering contract plus a post-CLOSE local RX fence. */
struct rf_facts {
 bool touched,close_ok,stop_ok,scan_done,worker_joined,rx_empty,callbacks_exited;
 bool authenticated,dhcp,maintenance_joined,keys_clean,unjoined;
 uint64_t close_epoch,empty_epoch;
};
void rf_init(struct rf_facts*);
void rf_command(struct rf_facts*,unsigned command,int result,uint64_t rx_epoch);
bool rf_scan_releasable(const struct rf_facts*);
bool rf_connect_releasable(const struct rf_facts*);
#endif
