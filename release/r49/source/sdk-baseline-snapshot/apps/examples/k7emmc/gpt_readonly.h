#ifndef VELAVISION_GPT_READONLY_H
#define VELAVISION_GPT_READONLY_H
#include <stdint.h>
#define VV_GPT_MAX_PARTITIONS 128
struct vv_gpt_partition { uint64_t first,last,attributes; uint8_t guid[16]; char name[37]; };
struct vv_gpt {
  uint64_t first_usable,last_usable;
  uint32_t count,entries_crc;
  struct vv_gpt_partition partitions[VV_GPT_MAX_PARTITIONS];
};
/* Fixed 512B sectors and 128B entries. Callback must enforce a global deadline.
 * Result valid ONLY on 0; no writes, mounting or recovery of damaged tables.
 */
int vv_gpt_read(void *ctx,int (*read_sector)(void *,uint64_t,uint8_t *),
                uint64_t sectors,struct vv_gpt *out);
#endif
