#include "command_core.h"
#include <stddef.h>
#include <string.h>
static int hexaddr(const char*s,uint64_t*v)
{unsigned i,d;uint64_t n=0;if(!s||s[0]!='0'||s[1]!='x')return -1;
 for(i=2;s[i];i++){if(i>=10)return -1;
  if(s[i]>='0'&&s[i]<='9')d=s[i]-'0';
  else if(s[i]>='a'&&s[i]<='f')d=s[i]-'a'+10;
  else if(s[i]>='A'&&s[i]<='F')d=s[i]-'A'+10;
  else return -1;
  n=(n<<4)|d;}
 if(i==2)return -1;
 *v=n;return 0;}
static int valid(const struct mi_args*a)
{unsigned i;if(!a)return 0;
 for(i=0;i<2;i++)if(a->range[i].begin<0x40400000||a->range[i].end>0x48200000||
  a->range[i].end<=a->range[i].begin||((a->range[i].begin|a->range[i].end)&4095)||
  a->range[i].end-a->range[i].begin!=(i?4096u:81920u))return 0;
 return a->range[0].end<=a->range[1].begin||a->range[1].end<=a->range[0].begin;}
int mi_parse(const char*xlat,const char*base,struct mi_args*a)
{if(!a)return MP_ARGUMENT;memset(a,0,sizeof(*a));
 if(hexaddr(xlat,&a->range[0].begin)||hexaddr(base,&a->range[1].begin))return MP_ARGUMENT;
 a->range[0].end=a->range[0].begin+81920;
 a->range[1].end=a->range[1].begin+4096;
 return valid(a)?0:MP_RANGE;}
int mi_allowed(const struct mi_args*a,uint64_t pa)
{unsigned i;if(!valid(a)||(pa&7))return 0;
 for(i=0;i<2;i++)if(pa>=a->range[i].begin&&pa<=a->range[i].end-8)return 1;
 return 0;}
struct guarded {const struct mi_args*a;const struct mi_ops*o;unsigned count;};
static int read_guarded(void*c,uint64_t pa,uint64_t*v)
{struct guarded*g=c;if(!mi_allowed(g->a,pa)||g->count>=4)return -1;
 g->count++;return g->o->read64(g->o->ctx,pa,v);}
static int same(const struct mp_snapshot*a,const struct mp_snapshot*b)
{return a->el==b->el&&a->sctlr==b->sctlr&&a->tcr==b->tcr&&a->ttbr0==b->ttbr0&&
 a->ttbr1==b->ttbr1&&a->mair==b->mair&&a->mpidr_before==a->mpidr_after&&
 b->mpidr_before==b->mpidr_after&&a->mpidr_before==b->mpidr_before;}
int mi_execute(const struct mi_args*a,const struct mi_ops*o,struct mi_result*r)
{struct guarded g;struct mp_reader reader;int rc;
 if(!r)return MP_ARGUMENT;
 memset(r,0,sizeof(*r));
 if(!valid(a)||!o||!o->cpu||!o->snapshot||!o->read64)return r->rc=MP_ARGUMENT;
 r->cpu_before=o->cpu(o->ctx);if(r->cpu_before!=0)return r->rc=MP_UNSTABLE;
 rc=o->snapshot(o->ctx,&r->before);if(rc)return r->rc=rc;
 if((r->before.ttbr0&UINT64_C(0x0000fffffffff000))!=a->range[1].begin)return r->rc=MP_RANGE;
 g.a=a;g.o=o;g.count=0;reader.ctx=&g;reader.read64=read_guarded;
 reader.ranges=a->range;reader.nranges=2;reader.current_build_identity_proven=1;
 rc=mp_walk(&r->before,&reader,&r->walk);r->reads=g.count;
 if(o->snapshot(o->ctx,&r->after))return r->rc=MP_READ;
 r->cpu_after=o->cpu(o->ctx);
 if(r->cpu_after!=0||!same(&r->before,&r->after))return r->rc=MP_UNSTABLE;
 return r->rc=rc;
}
