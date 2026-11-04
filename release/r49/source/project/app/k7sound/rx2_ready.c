#include <stdint.h>
/* Pure candidate readiness predicate ONLY for explicit RX2 diagnostic.
 * Observed bank0/1 layout is a hypothesis; unexpected banks reject the probe.
 * Return1 pair may be attempted,0 keep polling,-1 unsupported/invalid state.
 * Does not prove CPU bank cursor; caller must retain raw per-read trace. */
int rx2_pair_ready(uint32_t raw)
{
 unsigned a=raw&63,b=(raw>>6)&63,c=(raw>>12)&63,d=(raw>>18)&63;
 if(a>32||b>32||c||d)return -1;
 return a>=2&&b>=2;
}
