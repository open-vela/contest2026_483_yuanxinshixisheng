#ifndef K7_DEVICE_PHOTO_PROMPTS_H
#define K7_DEVICE_PHOTO_PROMPTS_H
#include <stdint.h>
struct k7_photo_prompt_state {uint32_t epoch;unsigned done;};
/* At most four prompts. Caller commits state only after all were played. */
int k7_photo_prompt_plan(const struct k7_photo_prompt_state *,uint32_t,unsigned,const char *keys[4]);
#endif
