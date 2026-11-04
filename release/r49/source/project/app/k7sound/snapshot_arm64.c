#include "mmu_probe.h"
#include <stddef.h>
#include <string.h>
int mp_snapshot_arm64(struct mp_snapshot*s)
{
 if(!s)return MP_ARGUMENT;
 memset(s,0,sizeof(*s));
#if defined(__aarch64__)
#define READ(reg,dest) __asm__ volatile("mrs %0, " #reg : "=r"(dest) :: "memory")
 READ(CurrentEL,s->el);
 if(s->el!=4)return MP_UNSUPPORTED;
 READ(mpidr_el1,s->mpidr_before);
 READ(sctlr_el1,s->sctlr);READ(tcr_el1,s->tcr);
 READ(ttbr0_el1,s->ttbr0);READ(ttbr1_el1,s->ttbr1);READ(mair_el1,s->mair);
 READ(mpidr_el1,s->mpidr_after);
#undef READ
 return s->mpidr_before==s->mpidr_after?MP_OK:MP_UNSTABLE;
#else
 return MP_UNSUPPORTED;
#endif
}
