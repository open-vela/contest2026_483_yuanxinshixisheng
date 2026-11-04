#include <nuttx/config.h>
#include "mmu_command.h"
#include "command_core.h"
#include <sched.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <inttypes.h>
/* Static lifetime even if timed wait or pthread_join fails. Audio owner is
 * intentionally retained on uncertain worker lifetime; no reuse/teardown. */
struct job {struct mi_args args;struct mi_result result;sem_t done;int post_rc;};
static struct job job;
static int cpu(void*c){(void)c;return sched_getcpu();}
static int snapshot(void*c,struct mp_snapshot*s){(void)c;return mp_snapshot_arm64(s);}
static int read64(void*c,uint64_t pa,uint64_t*v)
{
 struct job*j=c;
 if(!v||!mi_allowed(&j->args,pa))return -1;
 /* Opt-in contract: root MUST bind this command's two addresses to the
  * exact loaded ELF and frozen flat-RAM BSP using bind_elf.py first.
  * No pointer is computed before 8-byte alignment and both ranges checked. */
 *v=*(volatile const uint64_t *)(uintptr_t)pa;
 return 0;
}
static void *worker(void*c)
{
 struct job*j=c;struct mi_ops ops={j,cpu,snapshot,read64};
 mi_execute(&j->args,&ops,&j->result);
 j->post_rc=sem_post(&j->done);
 return NULL;
}
static void print_snapshot(const char*tag,const struct mp_snapshot*s)
{
 printf("MMU %s EL=%" PRIx64 " SCTLR=%016" PRIx64 " TCR=%016" PRIx64
  " TTBR0=%016" PRIx64 " TTBR1=%016" PRIx64 " MAIR=%016" PRIx64
  " MPIDR=%016" PRIx64 "/%016" PRIx64 "\n",tag,s->el,s->sctlr,s->tcr,
  s->ttbr0,s->ttbr1,s->mair,s->mpidr_before,s->mpidr_after);
}
int k7sound_mmu_command(int argc,char**argv,pthread_mutex_t*audio_owner)
{
 pthread_attr_t attr;pthread_t thread;cpu_set_t set;struct timespec end;
 struct mi_args args;int rc,created=0,attr_rc,sem_rc,unlock_rc;unsigned i;
 if(argc!=4||!argv||!audio_owner||mi_parse(argv[2],argv[3],&args)){
  puts("usage: k7sound mmu 0x<current-ELF-xlat> 0x<current-ELF-base>; CPU0");return 1;}
 if(pthread_mutex_trylock(audio_owner)){puts("MMU owner busy");return 1;}
 job.args=args;job.post_rc=0;
 if(sem_init(&job.done,0,0)){pthread_mutex_unlock(audio_owner);return 1;}
 rc=pthread_attr_init(&attr);
 if(!rc){
  CPU_ZERO(&set);CPU_SET(0,&set);
  rc=pthread_attr_setstacksize(&attr,8192);
  if(!rc)rc=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE);
  if(!rc)rc=pthread_attr_setaffinity_np(&attr,sizeof(set),&set);
  if(!rc){rc=pthread_create(&thread,&attr,worker,&job);created=!rc;}
  attr_rc=pthread_attr_destroy(&attr);if(!rc)rc=attr_rc;
 }
 if(!created){sem_rc=sem_destroy(&job.done);
  if(sem_rc){puts("MMU create cleanup failed held=1");return 1;}
  unlock_rc=pthread_mutex_unlock(audio_owner);
  printf("MMU create failed rc=%d sem=%d unlock=%d\n",rc,sem_rc,unlock_rc);return 1;}
 if(clock_gettime(CLOCK_REALTIME,&end)){
  puts("MMU clock failed held=1 static worker context retained");return 1;}
 end.tv_sec++;
 do {sem_rc=sem_timedwait(&job.done,&end);}while(sem_rc&&errno==EINTR);
 if(sem_rc){puts("MMU wait failed held=1 static worker context retained");return 1;}
 attr_rc=pthread_join(thread,NULL);
 if(attr_rc){printf("MMU join failed rc=%d held=1 static context retained\n",attr_rc);return 1;}
 /* Only joined worker output is read or printed; no concurrent result race. */
 print_snapshot("before",&job.result.before);print_snapshot("after",&job.result.after);
 for(i=0;i<job.result.walk.steps;i++){
  const struct mp_step*s=&job.result.walk.step[i];
  printf("MMU step L%u table=%016" PRIx64 " entry=%016" PRIx64 " desc=%016" PRIx64 "\n",
   s->level,s->table_pa,s->entry_pa,s->descriptor);
 }
 printf("MMU rc=%d cpu=%d/%d reads=%u PA=%016" PRIx64 " attr=%u MAIRbyte=%02x device=%d ngnrne=%d identity=%d AF=%d\n",
  job.result.rc,job.result.cpu_before,job.result.cpu_after,job.result.reads,
  job.result.walk.output_pa,job.result.walk.attr_index,job.result.walk.mair_byte,
  job.result.walk.device,job.result.walk.device_ngnrne,job.result.walk.identity,job.result.walk.access_flag);
 sem_rc=sem_destroy(&job.done);
 if(sem_rc){puts("MMU sem destroy failed held=1");return 1;}
 unlock_rc=pthread_mutex_unlock(audio_owner);
 return rc||job.result.rc||job.post_rc||unlock_rc?1:0;
}
