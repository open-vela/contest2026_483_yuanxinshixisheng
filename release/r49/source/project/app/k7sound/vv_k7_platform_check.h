#ifndef VV_K7_PLATFORM_CHECK_H
#define VV_K7_PLATFORM_CHECK_H
/* Explicit CLI-only bounded probe. RTC leaf init/read + CRU TRNG gate read;
 * no random request, network, wall-time approval or TLS activation. */
int vv_k7_platform_check(void);
#endif
