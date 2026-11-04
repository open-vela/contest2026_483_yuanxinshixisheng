#ifndef VV_MICROCRYSTAL_FAST_CONNECT_H
#define VV_MICROCRYSTAL_FAST_CONNECT_H
#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <stdbool.h>
#include <nuttx/wireless/bluetooth/bt_core.h>
#if defined(K7_MICROCRYSTAL_FAST_CONNECT_GUARD) && K7_MICROCRYSTAL_FAST_CONNECT_GUARD
extern bool vv_microcrystal_manual_fast_connect_target(FAR const bt_addr_le_t *) weak_function;
#define VV_MICROCRYSTAL_CREATE_SCAN_WINDOW(addr, cp) \
  ((vv_microcrystal_manual_fast_connect_target && \
    vv_microcrystal_manual_fast_connect_target(addr)) ? \
   (cp)->scan_interval : BT_HOST2LE16(0x0030))
#else
#define VV_MICROCRYSTAL_CREATE_SCAN_WINDOW(addr, cp) BT_HOST2LE16(0x0030)
#endif
#endif
