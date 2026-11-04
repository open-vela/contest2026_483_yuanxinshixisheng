#ifndef VELAVISION_RADIO_FIRMWARE_H
#define VELAVISION_RADIO_FIRMWARE_H
#include <stdint.h>
#ifdef CONFIG_EXAMPLES_K7RADIO_EMBEDDED_FW
extern const uint8_t g_k7radio_dram[193224];
extern const uint8_t g_k7radio_iram[356616];
extern const uint8_t g_k7radio_nv[37];
extern const uint8_t g_k7radio_calib[2372];
#define K7_RADIO_DRAM ((uintptr_t)g_k7radio_dram)
#define K7_RADIO_IRAM ((uintptr_t)g_k7radio_iram)
#define K7_RADIO_NV ((uintptr_t)g_k7radio_nv)
#define K7_RADIO_CALIB ((uintptr_t)g_k7radio_calib)
#else
#define K7_RADIO_DRAM ((uintptr_t)0x50000000)
#define K7_RADIO_IRAM ((uintptr_t)0x50100000)
#define K7_RADIO_NV ((uintptr_t)0x50180000)
#define K7_RADIO_CALIB ((uintptr_t)0x50190000)
#endif
#endif
