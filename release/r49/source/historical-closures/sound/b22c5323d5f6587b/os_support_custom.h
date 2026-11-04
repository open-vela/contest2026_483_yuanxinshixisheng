#ifndef K7_SPEEX10_OS_SUPPORT_CUSTOM_H
#define K7_SPEEX10_OS_SUPPORT_CUSTOM_H
/* Speex's documented OS hook. Upstream sources are unchanged and hash pinned.
 * All allocations belong to a checked fixed arena, not the realtime heap.
 * Speex init assumes allocation succeeds; arena exhaustion unwinds its C init
 * to the wrapper before any NULL dereference. There are no external resources.
 */
void *k7_speex10_alloc(int size);
void k7_speex10_free(void *ptr);
void *k7_speex10_realloc(void *ptr, int size);
void k7_speex10_fatal(const char *str, const char *file, int line);
void k7_speex10_warning(const char *str);
void k7_speex10_warning_int(const char *str, int value);
#define OVERRIDE_SPEEX_ALLOC
#define OVERRIDE_SPEEX_ALLOC_SCRATCH
#define OVERRIDE_SPEEX_REALLOC
#define OVERRIDE_SPEEX_FREE
#define OVERRIDE_SPEEX_FREE_SCRATCH
#define OVERRIDE_SPEEX_FATAL
#define OVERRIDE_SPEEX_WARNING
#define OVERRIDE_SPEEX_WARNING_INT
#define OVERRIDE_SPEEX_NOTIFY
#define speex_alloc k7_speex10_alloc
#define speex_alloc_scratch k7_speex10_alloc
#define speex_realloc k7_speex10_realloc
#define speex_free k7_speex10_free
#define speex_free_scratch k7_speex10_free
#define _speex_fatal k7_speex10_fatal
#define speex_warning k7_speex10_warning
#define speex_warning_int k7_speex10_warning_int
#define speex_notify k7_speex10_warning
#endif
