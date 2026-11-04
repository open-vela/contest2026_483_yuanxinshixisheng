#ifndef MMU_COMMAND_CORE_H
#define MMU_COMMAND_CORE_H
#include "mmu_probe.h"
struct mi_args {struct mp_range range[2];};
struct mi_ops {void*ctx;int(*cpu)(void*);int(*snapshot)(void*,struct mp_snapshot*);
 int(*read64)(void*,uint64_t,uint64_t*);};
struct mi_result {int rc;unsigned reads;int cpu_before,cpu_after;
 struct mp_snapshot before,after;struct mp_report walk;};
/* Exactly two 0x-prefixed hexadecimal starts. Sizes fixed 81920 and4096. */
int mi_parse(const char*xlat,const char*base,struct mi_args*);
int mi_allowed(const struct mi_args*,uint64_t pa);
int mi_execute(const struct mi_args*,const struct mi_ops*,struct mi_result*);
#endif
