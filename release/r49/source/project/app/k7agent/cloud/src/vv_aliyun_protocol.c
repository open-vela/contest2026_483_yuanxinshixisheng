#include "vv_aliyun_protocol.h"
#include "vv_backend_protocol.h"
#include "../../../k7radio/cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static void clear(char*p,size_t cap,size_t*n){if(n)*n=0;if(p&&cap)memset(p,0,cap);}
static int id(const char*s){size_t n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(n>=128||!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'||*s=='.'||*s==':'))return 0;return 1;}
static void u16(unsigned char*p,unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void u32(unsigned char*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t r32(const unsigned char*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned r16(const unsigned char*p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
int vv_aliyun_asr_json(const char*model,const unsigned char*pcm,size_t n,char*out,size_t cap,size_t*written){
 clear(out,cap,written);if(!out||!written||!id(model)||!pcm||!n||n%2||n>VV_ALIYUN_ASR_PCM_MAX)return -EINVAL;
 char prefix[320];int p=snprintf(prefix,sizeof(prefix),"{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,",model);
 const char suffix[]="\"}}]}],\"stream\":false}";size_t bytes=n+44,encoded=((bytes+2)/3)*4;
 if(p<0||(size_t)p>=sizeof(prefix)||(size_t)p+encoded+sizeof(suffix)>cap)return -ENOSPC;
 unsigned char wav[44]={0};memcpy(wav,"RIFF",4);u32(wav+4,(uint32_t)n+36);memcpy(wav+8,"WAVEfmt ",8);u32(wav+16,16);u16(wav+20,1);u16(wav+22,1);u32(wav+24,16000);u32(wav+28,32000);u16(wav+32,2);u16(wav+34,16);memcpy(wav+36,"data",4);u32(wav+40,(uint32_t)n);
 static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";size_t pos=(size_t)p;memcpy(out,prefix,pos);
 for(size_t i=0;i<bytes;i+=3){unsigned v=0;size_t count=bytes-i<3?bytes-i:3;for(unsigned j=0;j<3;j++){size_t at=i+j;unsigned x=j<count?(at<44?wav[at]:pcm[at-44]):0;v=(v<<8)|x;}out[pos++]=alphabet[(v>>18)&63];out[pos++]=alphabet[(v>>12)&63];out[pos++]=count>1?alphabet[(v>>6)&63]:'=';out[pos++]=count>2?alphabet[v&63]:'=';}
 memcpy(out+pos,suffix,sizeof(suffix));*written=pos+sizeof(suffix)-1;return 0;
}
static int utf8(const unsigned char*s,size_t n){for(size_t i=0;i<n;){unsigned v=s[i++],more=0,min=0;if(v<128){if(!v)return 0;continue;}if(v>=0xc2&&v<=0xdf){more=1;min=0x80;v&=31;}else if(v>=0xe0&&v<=0xef){more=2;min=0x800;v&=15;}else if(v>=0xf0&&v<=0xf4){more=3;min=0x10000;v&=7;}else return 0;if(n-i<more)return 0;while(more--){unsigned c=s[i++];if((c&0xc0)!=0x80)return 0;v=(v<<6)|(c&63);}if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;}return 1;}
static int audit(cJSON*v){for(cJSON*a=v->child;a;a=a->next){if(cJSON_IsObject(v)){if(!a->string)return -EPROTO;for(cJSON*b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return -EPROTO;if(!strcmp(a->string,"mock")&&cJSON_IsTrue(a))return -EPROTO;}int r=audit(a);if(r)return r;}return 0;}
/* Numeric parse diagnostics only; never emit response text or field values. */
static cJSON *parse_detail(const char*s,size_t n,unsigned max_depth,int *why){
 *why=1;if(!s||!n||n>VV_ALIYUN_JSON_MAX)return NULL;
 *why=2;if(!utf8((const unsigned char*)s,n))return NULL;
 unsigned depth=0;int quoted=0;
 for(size_t i=0;i<n;i++){
  char c=s[i];
  if(quoted&&c=='\\'){
   *why=3;if(++i>=n)return NULL;
   *why=4;if(s[i]=='u'&&i+4<n&&!memcmp(s+i+1,"0000",4))return NULL;
   continue;
  }
  if(c=='"')quoted=!quoted;
  else if(!quoted&&(c=='{'||c=='[')){*why=5;if(++depth>max_depth)return NULL;}
  else if(!quoted&&(c=='}'||c==']')){*why=6;if(!depth)return NULL;depth--;}
 }
 *why=6;if(depth||quoted)return NULL;
 const char*end=NULL;cJSON*r=cJSON_ParseWithLengthOpts(s,n,&end,0);
 *why=7;if(!r||!end){cJSON_Delete(r);return NULL;}
 *why=8;if(!cJSON_IsObject(r)){cJSON_Delete(r);return NULL;}
 while(end<s+n&&(*end==' '||*end=='\n'||*end=='\r'||*end=='\t'))end++;
 *why=9;if(end!=s+n){cJSON_Delete(r);return NULL;}
 *why=10;if(audit(r)){cJSON_Delete(r);return NULL;}
 *why=11;if(cJSON_GetObjectItemCaseSensitive(r,"error")){cJSON_Delete(r);return NULL;}
 *why=12;if(cJSON_GetObjectItemCaseSensitive(r,"code")){cJSON_Delete(r);return NULL;}
 *why=0;return r;
}
static cJSON *parse(const char*s,size_t n){int why;return parse_detail(s,n,4,&why);}
/* Shape enum: absent=0, null=1, string=2, array=3, object=4, other=5. */
static int shape(const cJSON*v){return !v?0:cJSON_IsNull(v)?1:cJSON_IsString(v)?2:cJSON_IsArray(v)?3:cJSON_IsObject(v)?4:5;}
int vv_aliyun_asr_text(const char*s,size_t n,char*out,size_t cap){
 clear(out,cap,NULL);if(!out||!cap)return -EINVAL;
 int why=0,rc=-EPROTO,stage=1,finish=0,choices_count=-1;
 cJSON*r=parse_detail(s,n,16,&why);
 cJSON*choices=cJSON_GetObjectItemCaseSensitive(r,"choices");
 cJSON*one=cJSON_GetArrayItem(choices,0);
 cJSON*message=cJSON_GetObjectItemCaseSensitive(one,"message");
 cJSON*text=cJSON_GetObjectItemCaseSensitive(message,"content");
 cJSON*reason=cJSON_GetObjectItemCaseSensitive(one,"finish_reason");
 if(!r)goto done;
 stage=2;
 if(!cJSON_IsArray(choices))goto done;
 choices_count=cJSON_GetArraySize(choices);
 if(choices_count!=1||!cJSON_IsObject(message)||!cJSON_IsString(text))goto done;
 /* Legacy aliyun_speech.py does not require finish_reason. Accept absent,
  * null and empty optional metadata, but never explicit truncated output. */
 finish=!reason?0:cJSON_IsNull(reason)?1:cJSON_IsString(reason)?
   (!reason->valuestring[0]?2:!strcmp(reason->valuestring,"stop")?3:4):5;
 stage=3;if(finish>=4)goto done;
 const char*p=text->valuestring;size_t len=strlen(p);
 stage=4;if(!utf8((const unsigned char*)p,len))goto done;
 while(len&&(*p==' '||*p=='\r'||*p=='\n'||*p=='\t')){p++;len--;}
 while(len&&(p[len-1]==' '||p[len-1]=='\r'||p[len-1]=='\n'||p[len-1]=='\t'))len--;
 stage=5;if(len>=cap||len>2048)goto done;
 stage=6;
 /* Preserve legacy internal whitespace; the existing command normalizer
  * explicitly supports CR/LF/tab. Reject other control characters. */
 for(size_t i=0;i<len;i++){
  unsigned char c=(unsigned char)p[i];
  if((c<32&&c!='\r'&&c!='\n'&&c!='\t')||c==127)goto done;
 }
 memcpy(out,p,len);out[len]=0;rc=0;stage=0;
 done:
 printf("ALIYUN_ASR_SHAPE rc=%d stage=%d parse=%d choices=%d content=%d finish_type=%d finish=%d\n",
        rc,stage,why,choices_count,shape(text),shape(reason),finish);
 cJSON_Delete(r);return rc;
}
int vv_aliyun_tts_json(const char*model,const char*voice,const char*text,char*out,size_t cap,size_t*written){clear(out,cap,written);if(!out||!written||!id(model)||!id(voice)||cap>2147483647u)return -EINVAL;char small[8192];size_t n=0;int rc=vv_backend_tts_json(text,small,sizeof(small),&n);if(rc)return rc;cJSON*input=cJSON_Parse(small),*root=cJSON_CreateObject();if(!input||!root){cJSON_Delete(input);cJSON_Delete(root);return -ENOMEM;}if(!cJSON_AddStringToObject(root,"model",model)||!cJSON_AddItemToObject(root,"input",input)){cJSON_Delete(input);cJSON_Delete(root);return -ENOMEM;}if(!cJSON_AddStringToObject(input,"voice",voice)||!cJSON_AddStringToObject(input,"format","wav")||!cJSON_AddNumberToObject(input,"sample_rate",16000)||!cJSON_PrintPreallocated(root,out,(int)cap,0)){cJSON_Delete(root);clear(out,cap,written);return -ENOSPC;}*written=strlen(out);cJSON_Delete(root);return 0;}
int vv_aliyun_tts_download_policy(const char*s,size_t n,const char*const*hosts,size_t count,bool compat,struct vv_aliyun_download*out){if(!out)return -EINVAL;memset(out,0,sizeof(*out));if(!hosts||!count)return -EINVAL;cJSON*r=parse(s,n);if(!r)return -EPROTO;cJSON*output=cJSON_GetObjectItemCaseSensitive(r,"output"),*audio=cJSON_GetObjectItemCaseSensitive(output,"audio"),*url=cJSON_GetObjectItemCaseSensitive(audio,"url");int rc=-EPROTO;if(cJSON_IsString(url)){const char*u=url->valuestring;bool plain=!strncmp(u,"http://",7);if(!strncmp(u,"https://",8)||(compat&&plain)){const char*h=u+(plain?7:8),*path=strchr(h,'/');if(path&&path>h&&(size_t)(path-h)<=128&&strlen(path)<=2048){bool ok=true;for(const char*p=h;p<path;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='-'))ok=false;for(const char*p=path;*p;p++)if((unsigned char)*p<=32||(unsigned char)*p>=127||*p=='#'||*p=='\\')ok=false;bool allowed=false;for(size_t i=0;i<count;i++)if(hosts[i]&&strlen(hosts[i])==(size_t)(path-h)&&!memcmp(hosts[i],h,(size_t)(path-h)))allowed=true;const char*proven="dashscope-result-bj.oss-cn-beijing.aliyuncs.com";if(plain&&(strlen(proven)!=(size_t)(path-h)||memcmp(proven,h,(size_t)(path-h))))ok=false;if(ok&&allowed){memcpy(out->host,h,(size_t)(path-h));strcpy(out->path,path);rc=0;}}}}cJSON_Delete(r);return rc;}
static int32_t mono(const unsigned char*p,size_t i,unsigned channels){int32_t a=(int16_t)r16(p+i*channels*2);return channels==1?a:(a+(int16_t)r16(p+i*channels*2+2))/2;}
int vv_aliyun_wav_pcm_policy(const unsigned char*wav,size_t n,unsigned char*out,size_t cap,size_t*written,bool compat){if(written)*written=0;if(wav&&out){uintptr_t a=(uintptr_t)wav,b=(uintptr_t)out;if((a<=b&&b-a<n)||(b<a&&a-b<cap))return -EINVAL;}clear((char*)out,cap,written);static const unsigned char sentinel[44]={0x52,0x49,0x46,0x46,0xbf,0xff,0xff,0x7f,0x57,0x41,0x56,0x45,0x66,0x6d,0x74,0x20,0x10,0,0,0,1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,0x10,0,0x64,0x61,0x74,0x61,0x9b,0xff,0xff,0x7f};bool streaming=compat&&wav&&n>44&&!(n%2)&&!memcmp(wav,sentinel,44);if(!wav||!out||!written||n<44||n>3000000||memcmp(wav,"RIFF",4)||memcmp(wav+8,"WAVE",4)||(!streaming&&(uint64_t)r32(wav+4)+8!=n))return -EINVAL;const unsigned char*fmt=NULL,*data=NULL;size_t bytes=0;for(size_t off=12;off<n;){if(n-off<8)return -EPROTO;size_t len=r32(wav+off+4),start=off+8;if(streaming&&off==36)len=n-start;if(len>n-start||len+(len&1)>n-start)return -EPROTO;if(!memcmp(wav+off,"fmt ",4)){if(fmt||len<16)return -EPROTO;fmt=wav+start;}else if(!memcmp(wav+off,"data",4)){if(data)return -EPROTO;data=wav+start;bytes=len;}off=start+len+(len&1);}if(!fmt||!data||r16(fmt)!=1||r16(fmt+14)!=16)return -EPROTO;unsigned channels=r16(fmt+2),rate=r32(fmt+4);if((channels!=1&&channels!=2)||rate<8000||rate>48000||r16(fmt+12)!=channels*2||r32(fmt+8)!=rate*channels*2||!bytes||bytes%(channels*2))return -EPROTO;size_t frames=bytes/(channels*2);if(frames>(size_t)rate*30)return -EFBIG;size_t count=(frames*16000u+rate/2)/rate;if(!count||count>480000||count*2>cap)return -ENOSPC;for(size_t i=0;i<count;i++){uint64_t pos=count>1?(uint64_t)i*(frames-1):0;size_t denom=count>1?count-1:1,index=(size_t)(pos/denom),frac=(size_t)(pos%denom);int64_t a=mono(data,index,channels),b=mono(data,index+1<frames?index+1:index,channels);int64_t value=a*(int64_t)(denom-frac)+b*(int64_t)frac;value=value>=0?(value+(int64_t)denom/2)/(int64_t)denom:(value-(int64_t)denom/2)/(int64_t)denom;u16(out+2*i,(unsigned)(uint16_t)(int16_t)value);}*written=count*2;return 0;}

int vv_aliyun_tts_download(const char*s,size_t n,const char*const*h,size_t c,struct vv_aliyun_download*o){return vv_aliyun_tts_download_policy(s,n,h,c,false,o);}
int vv_aliyun_wav_pcm(const unsigned char*w,size_t n,unsigned char*o,size_t c,size_t*z){return vv_aliyun_wav_pcm_policy(w,n,o,c,z,false);}
