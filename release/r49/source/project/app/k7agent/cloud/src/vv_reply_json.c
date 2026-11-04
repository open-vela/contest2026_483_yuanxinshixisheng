#include "vv_reply_json.h"
#include "../../../k7radio/cJSON.h"
#include <pthread.h>
#include <stdint.h>
static pthread_mutex_t parse_lock=PTHREAD_MUTEX_INITIALIZER;
struct cJSON *vv_reply_json_parse(const char *data,size_t n){
 if(!data||n==SIZE_MAX||data[n])return NULL;
 if(pthread_mutex_lock(&parse_lock))return NULL;
 const char *end=NULL;
 cJSON *root=cJSON_ParseWithLengthOpts(data,n+1,&end,1);
 if(pthread_mutex_unlock(&parse_lock)){cJSON_Delete(root);return NULL;}
 return root;
}
