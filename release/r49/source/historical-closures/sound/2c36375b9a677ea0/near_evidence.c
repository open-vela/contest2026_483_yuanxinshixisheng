#include "near_evidence.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define N 1024u
#define SOURCE UINT32_C(0x52455331)
struct ne_worker {
 struct k7_vad *model;
 struct ne_policy policy;
 struct ne_identity identity;
 int16_t mic[N],ref[N],res[N];
 uint64_t next,ordinal,origin,evidence_first,last_end;
 unsigned ready,consecutive;
};
static int equal(const struct ne_identity *a,const struct ne_identity *b) {
#define EQ(x) (a->x==b->x)
 return EQ(owner)&&EQ(reference_instance)&&EQ(reference_epoch)&&EQ(clock_epoch)&&
 EQ(dialogue_instance)&&EQ(scope)&&EQ(lease)&&EQ(reply)&&EQ(mapping_revision)&&
 EQ(input_epoch)&&EQ(tx_generation);
#undef EQ
}
void ne_invalidate(struct ne_worker *s) {
 if(s){s->ready=0;s->consecutive=0;k7_vad_invalidate(s->model);}
}
int ne_create(struct k7_vad *m,const struct ne_policy *p,struct ne_worker **out) {
 if(!out)return -EINVAL;
 *out=NULL;
 if(!m||!p||!isfinite(p->minimum_residual_power)||p->minimum_residual_power<=0||
 !isfinite(p->maximum_echo_correlation2)||p->maximum_echo_correlation2<0||p->maximum_echo_correlation2>1||
 !isfinite(p->minimum_residual_reference_ratio)||p->minimum_residual_reference_ratio<0||
 !isfinite(p->minimum_probability)||p->minimum_probability<=0||p->minimum_probability>1||
 !p->consecutive_windows||p->consecutive_windows>32)return -EINVAL;
 struct ne_worker *s=calloc(1,sizeof(*s));if(!s)return -ENOMEM;
 s->model=m;s->policy=*p;ne_invalidate(s);*out=s;return 0;
}
int ne_begin(struct ne_worker *s,const struct ne_identity *id,uint64_t first) {
 if(!s||!id)return -EINVAL;
 ne_invalidate(s);
 if(!id->owner||!id->reference_instance||!id->reference_epoch||!id->clock_epoch||
 !id->dialogue_instance||!id->scope||!id->lease||!id->reply||!id->mapping_revision||
 !id->input_epoch||!id->tx_generation)return -EINVAL;
 struct k7_vad_identity v={id->owner,id->input_epoch,SOURCE};
 int rc=k7_vad_reset(s->model,v,0,first);if(rc)return rc;
 s->identity=*id;s->origin=s->next=first;s->ordinal=0;s->last_end=0;s->ready=1;return 0;
}
int ne_feed(struct ne_worker *s,const struct ne_identity *before,const struct ne_identity *after,
 uint64_t first,const int16_t mic[320],const int16_t ref[320],const int16_t res[320],struct ne_report *out) {
 if(!s||!before||!after||!mic||!ref||!res||!out)return -EINVAL;
 if(!s->ready)return -EPIPE;
 if(!equal(before,after)||!equal(before,&s->identity)||first!=s->next||
 first>UINT64_MAX-320||s->ordinal==UINT64_MAX){ne_invalidate(s);return -EPIPE;}
 for(unsigned j=0;j<320;j++){unsigned i=(unsigned)((first+j)%N);s->mic[i]=mic[j];s->ref[i]=ref[j];s->res[i]=res[j];}
 struct k7_vad_frame f={{before->owner,before->input_epoch,SOURCE},s->ordinal,first,res,320};
 struct k7_vad_observation v;int rc=k7_vad_feed(s->model,&f,&v);
 if(rc<0){ne_invalidate(s);return rc;}
 s->ordinal++;s->next=first+320;if(!rc)return 0;
 if(v.identity.stream_id!=before->owner||v.identity.epoch!=before->input_epoch||v.identity.source_tag!=SOURCE||
 v.completing_frame_id!=f.frame_id||v.input_end_sample>s->next||v.input_first_sample<s->origin||
 v.input_end_sample<v.input_first_sample||v.input_end_sample-v.input_first_sample!=576||
 v.advance_end_sample!=v.input_end_sample||v.advance_first_sample!=v.input_end_sample-512||
 s->next-v.input_first_sample>N||!isfinite(v.probability)||v.probability<0||v.probability>1||
 (s->last_end&&v.advance_first_sample!=s->last_end)){ne_invalidate(s);return -EPROTO;}
 struct ne_report r={0};r.identity=*before;r.model=v;r.completing_first=first;r.completing_next=s->next;
 double sm=0,sr=0,se=0,mm=0,rr=0,ee=0,re=0;
 for(uint64_t j=v.advance_first_sample;j<v.advance_end_sample;j++){
  unsigned i=(unsigned)(j%N);double m=s->mic[i],a=s->ref[i],b=s->res[i];
  sm+=m;sr+=a;se+=b;mm+=m*m;rr+=a*a;ee+=b*b;re+=a*b;
  if(s->mic[i]==INT16_MIN||s->mic[i]==INT16_MAX||s->ref[i]==INT16_MIN||s->ref[i]==INT16_MAX||s->res[i]==INT16_MIN||s->res[i]==INT16_MAX)r.clipped++;
 }
 double a=fmax(0,rr-sr*sr/512),b=fmax(0,ee-se*se/512),cross=re-sr*se/512;
 r.microphone_power=fmax(0,mm-sm*sm/512)/512;r.reference_power=a/512;r.residual_power=b/512;
 r.correlation2=a>0&&b>0?fmin(1,cross*cross/(a*b)):0;
 unsigned yes=!r.clipped&&v.probability>=s->policy.minimum_probability&&
 r.residual_power>=s->policy.minimum_residual_power&&r.correlation2<=s->policy.maximum_echo_correlation2&&
 r.residual_power>=r.reference_power*s->policy.minimum_residual_reference_ratio;
 if(yes){if(!s->consecutive)s->evidence_first=v.input_first_sample;if(s->consecutive<32)s->consecutive++;}
 else s->consecutive=0;
 s->last_end=v.advance_end_sample;r.consecutive=s->consecutive;r.candidate=s->consecutive>=s->policy.consecutive_windows;
 if(s->consecutive){r.evidence_first=s->evidence_first;r.evidence_next=v.input_end_sample;}
 /* This numeric classifier CANNOT grant calibrated clean/tail authority. */
 r.qualified=0;*out=r;return 1;
}
void ne_destroy(struct ne_worker *s){if(s){ne_invalidate(s);memset(s,0,sizeof(*s));free(s);}}
