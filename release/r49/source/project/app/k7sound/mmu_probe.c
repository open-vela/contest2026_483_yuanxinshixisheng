#include "mmu_probe.h"
#include <stddef.h>
#include <string.h>
#define PA_MASK UINT64_C(0x0000fffffffff000)
static int permitted(const struct mp_reader*r,uint64_t table)
{
 unsigned i;
 if(table<UINT64_C(0x40400000)||table>UINT64_C(0x48200000)-4096)return 0;
 for(i=0;i<r->nranges;i++)if(table>=r->ranges[i].begin&&
   table<=r->ranges[i].end-4096)return 1;
 return 0;
}
int mp_walk(const struct mp_snapshot*s,const struct mp_reader*r,struct mp_report*out)
{
 static const unsigned shifts[4]={39,30,21,12};
 uint64_t table,d,addr,lowmask;unsigned i,type,shift;
 int rc=MP_DESCRIPTOR;
 if(!out)return MP_ARGUMENT;
 memset(out,0,sizeof(*out));out->result=MP_ARGUMENT;
 if(!s||!r||!r->read64||!r->ranges||!r->nranges||r->nranges>4)return out->result;
 if(!r->current_build_identity_proven)return out->result=MP_UNSUPPORTED;
 for(i=0;i<r->nranges;i++)if(r->ranges[i].begin<UINT64_C(0x40400000)||
   r->ranges[i].end>UINT64_C(0x48200000)||r->ranges[i].begin>=r->ranges[i].end||
   r->ranges[i].end-r->ranges[i].begin<4096||
   ((r->ranges[i].begin|r->ranges[i].end)&4095))return out->result=MP_RANGE;
 if(s->mpidr_before!=s->mpidr_after)return out->result=MP_UNSTABLE;
 /* Narrow known BSP regime only: EL1, MMU on, LE, TG0=4K, T0SZ=16,
  * IPS=48-bit, walks enabled. Reject newer LPA2/DS and E0PD extensions.
  * TTBR1 captured but not walked: target is in low canonical VA range. */
 if(s->el!=4||!(s->sctlr&1)||(s->sctlr&(UINT64_C(1)<<25))||
    (s->tcr&63)!=16||((s->tcr>>14)&3)||((s->tcr>>32)&7)!=5||
    (s->tcr&((UINT64_C(1)<<7)|(UINT64_C(1)<<59)|(UINT64_C(1)<<55)))||
    (s->ttbr0&UINT64_C(0xffe)))return out->result=MP_UNSUPPORTED;
 table=s->ttbr0&PA_MASK;
 for(i=0;i<4;i++){
  struct mp_step*step=&out->step[out->steps++];shift=shifts[i];
  step->level=i;step->table_pa=table;
  if(!permitted(r,table)){rc=MP_RANGE;break;}
  addr=table+((MP_TARGET>>shift)&511)*8;step->entry_pa=addr;
  if(r->read64(r->ctx,addr,&d)){rc=MP_READ;break;}
  step->descriptor=d;type=(unsigned)(d&3);
  if(d&UINT64_C(0x0003000000000000)){rc=MP_UNSUPPORTED;break;}
  if(type==0||type==2){rc=MP_DESCRIPTOR;break;}
  if(i<3&&type==3){
   if(d&UINT64_C(0xffc)){rc=MP_UNSUPPORTED;break;}
   table=d&PA_MASK;continue;
  }
  if((i==0)||(i==3&&type!=3)){rc=MP_DESCRIPTOR;break;}
  lowmask=(UINT64_C(1)<<shift)-1;
  if(d&PA_MASK&lowmask){rc=MP_DESCRIPTOR;break;}
  out->terminal_level=i;out->output_pa=(d&PA_MASK&~lowmask)|(MP_TARGET&lowmask);
  out->attr_index=(unsigned)((d>>2)&7);
  out->mair_byte=(unsigned)((s->mair>>(8*out->attr_index))&255);
  out->device=(out->mair_byte==0||out->mair_byte==4||out->mair_byte==8||out->mair_byte==12);
  out->device_ngnrne=out->mair_byte==0;
  out->identity=out->output_pa==MP_TARGET;out->access_flag=(int)((d>>10)&1);
  rc=MP_OK;break;
 }
 out->result=rc;return rc;
}
