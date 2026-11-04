#ifndef PROV_PHONE_TIME_H
#define PROV_PHONE_TIME_H
#include <stddef.h>
#include <stdint.h>
/* User-approved Asia/Shanghai phone clock. Encrypted BLE is not MITM authentication. */
struct prov_phone_rx {char text[19];unsigned used,suffix;uint64_t began;};
/* 1 complete date, 0 consumed fragment/suffix, 2 not a date frame, negative reject. */
int prov_phone_time_feed(struct prov_phone_rx *,const void *,size_t,uint64_t,uint64_t *);
int prov_phone_time_parse(const char *,size_t,uint64_t *);
int k7radio_submit_phone_time(uint32_t session,uint64_t utc,uint64_t mono);
void k7radio_phone_time_disconnect(void);
int k7radio_take_phone_time(uint64_t *utc_seconds,uint64_t *received_mono_ms);
void k7radio_close_phone_time(void);
#endif
