#ifndef VV_PROMPT_CACHE_H
#define VV_PROMPT_CACHE_H
#include <stddef.h>
#include <stdbool.h>
#define VV_PROMPT_CACHE_SLOTS 31u
#define VV_PROMPT_CACHE_ITEM_MAX 320000u
#define VV_PROMPT_CACHE_TOTAL_MAX (2u*1024u*1024u)
/* Zero-init; same immutable model/voice/PCM16 mono16kHz owner only.
 * Caller serializes get/put/clear with the provider lifetime lock.
 * Only exact compiled fixed prompts are eligible. No dynamic report caching.
 * 1=hit/stored, 0=miss/not cached, negative=invalid/cancelled.
 * Call put only after successful complete synthesis AND transport validation.
 * Cache failure must not cause successful network synthesis to be retried. */
struct vv_prompt_cache {
 struct {const unsigned char *pcm;size_t bytes;bool owned;} slot[VV_PROMPT_CACHE_SLOTS];
 size_t bytes;
};
struct vv_prompt_pcm_asset {const char *text;const unsigned char *pcm;size_t bytes;};
/* Static assets remain alive through clear; validated as one transaction.
 * Seed only an empty cache. Const assets are never freed or overwritten. */
int vv_prompt_cache_seed(struct vv_prompt_cache *,const struct vv_prompt_pcm_asset *,size_t);
bool vv_prompt_cache_eligible(const char *);
/* Borrowed immutable view, valid only while caller holds owner lifetime lock. */
int vv_prompt_cache_view(struct vv_prompt_cache *,const char *,const unsigned char **,size_t *);
int vv_prompt_cache_get(struct vv_prompt_cache *,const char *,unsigned char *,size_t,
 size_t *,bool (*)(void *),void *);
int vv_prompt_cache_put(struct vv_prompt_cache *,const char *,const unsigned char *,size_t,
 bool (*)(void *),void *);
void vv_prompt_cache_clear(struct vv_prompt_cache *);
#endif
