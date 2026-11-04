#ifndef VV_REPLY_JSON_H
#define VV_REPLY_JSON_H
#include <stddef.h>
struct cJSON;
/* Incremental LLM and TTS parse concurrently. The bundled parser has a shared
 * error cursor even when callers do not read it. Serialize parsing only;
 * each returned tree remains independently owned. Input must end at data[n]. */
struct cJSON *vv_reply_json_parse(const char *data,size_t n);
#endif
