#ifndef AUDIO_MMU_PROBE_H
#define AUDIO_MMU_PROBE_H
#include <stdint.h>
#define MP_TARGET UINT64_C(0x2a610000)
enum {MP_OK=0,MP_ARGUMENT=-1,MP_UNSUPPORTED=-2,MP_RANGE=-3,
 MP_READ=-4,MP_DESCRIPTOR=-5,MP_UNSTABLE=-6};
struct mp_snapshot {uint64_t el,sctlr,tcr,ttbr0,ttbr1,mair,mpidr_before,mpidr_after;};
struct mp_range {uint64_t begin,end;}; /* half open, current-build proven table RAM */
struct mp_reader {
 void *ctx;
 /* Fetch exactly one descriptor through caller's proven PA->readable VA path.
  * No implicit identity mapping. Return 0 only for a successful 64-bit read. */
 int (*read64)(void *,uint64_t pa,uint64_t *value);
 const struct mp_range *ranges;unsigned nranges;
 int current_build_identity_proven;
};
struct mp_step {uint64_t table_pa,entry_pa,descriptor;unsigned level;};
struct mp_report {
 int result;unsigned steps,terminal_level,attr_index,mair_byte;
 int device,device_ngnrne,identity,access_flag;
 uint64_t output_pa;
 struct mp_step step[4];
};
int mp_walk(const struct mp_snapshot *,const struct mp_reader *,struct mp_report *);
/* AArch64 EL1 only; MRS only, no table dereference or AT/PAR/TLB operations. */
int mp_snapshot_arm64(struct mp_snapshot *);
#endif
