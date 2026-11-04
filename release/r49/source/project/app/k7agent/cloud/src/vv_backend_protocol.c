#include "vv_backend_protocol.h"
#include "../../../k7radio/cJSON.h"
#include <ctype.h>
#include <string.h>

static bool string_ok(const char *s, size_t max) {
  if (!s || !*s) return false;
  size_t n=0;
  for (; *s; ++s,++n)
    if (n>=max || (unsigned char)*s<32 || (unsigned char)*s==127) return false;
  return true;
}
static int emit(cJSON *root, char *out, size_t cap, size_t *written) {
  if (written) *written=0;
  if (out && cap) out[0]=0;
  if (!root || !out || !cap || !written) { cJSON_Delete(root); return VV_BACKEND_INVALID; }
  if (cap>VV_BACKEND_JSON_MAX) cap=VV_BACKEND_JSON_MAX;
  int ok=cJSON_PrintPreallocated(root,out,(int)cap,0);
  cJSON_Delete(root);
  if (!ok) { memset(out,0,cap); return VV_BACKEND_LIMIT; }
  *written=strlen(out); return 0;
}
int vv_backend_login_json(const char *credential, const char *version,
  const char *proof, char *out, size_t cap, size_t *written) {
  if (written) *written=0;
  if (out && cap) out[0]=0;
  if (!string_ok(credential,256) || !string_ok(version,20) || !string_ok(proof,512))
    return VV_BACKEND_INVALID;
  if (version[0]=='0' && version[1]) return VV_BACKEND_INVALID;
  for (const char *p=version;*p;++p)
    if (*p<'0' || *p>'9') return VV_BACKEND_INVALID;
  cJSON *root=cJSON_CreateObject();
  if (!root || !cJSON_AddStringToObject(root,"credential",credential) ||
      !cJSON_AddStringToObject(root,"credentialVersion",version) ||
      !cJSON_AddStringToObject(root,"proof",proof)) { cJSON_Delete(root); return VV_BACKEND_LIMIT; }
  return emit(root,out,cap,written);
}
/* Unicode White_Space, independent of host locale. BOM/zero-width space are
 * not whitespace. Count Unicode scalar values, not UTF-8 bytes or graphemes. */
static bool tts_space(unsigned v) {
  return (v>=9 && v<=13) || v==32 || v==0x85 || v==0xa0 || v==0x1680 ||
    (v>=0x2000 && v<=0x200a) || v==0x2028 || v==0x2029 || v==0x202f ||
    v==0x205f || v==0x3000;
}
static int tts_trim(const char *s,char out[4001]) {
  if(!s) return VV_BACKEND_INVALID;
  size_t n=0; while(s[n]) { if(n>=VV_BACKEND_JSON_MAX) return VV_BACKEND_LIMIT; ++n; }
  size_t i=0,first=n,last=0,count=0,last_count=0;
  while(i<n) {
    size_t at=i; unsigned v=(unsigned char)s[i++],more=0,min=0;
    if(v<128) {} else if(v>=0xc2 && v<=0xdf) {more=1;min=0x80;v&=31;}
    else if(v>=0xe0 && v<=0xef) {more=2;min=0x800;v&=15;}
    else if(v>=0xf0 && v<=0xf4) {more=3;min=0x10000;v&=7;}
    else return VV_BACKEND_INVALID;
    if(n-i<more) return VV_BACKEND_INVALID;
    while(more--) { unsigned b=(unsigned char)s[i++]; if((b&0xc0)!=0x80) return VV_BACKEND_INVALID; v=(v<<6)|(b&63); }
    if(v<min || v>0x10ffff || (v>=0xd800 && v<=0xdfff)) return VV_BACKEND_INVALID;
    if((v<32 || v==127) && !tts_space(v)) return VV_BACKEND_INVALID;
    if(first==n && !tts_space(v)) first=at;
    if(first!=n) { ++count; if(!tts_space(v)) {last=i;last_count=count;} }
  }
  if(first==n) return VV_BACKEND_INVALID;
  if(last_count>1000 || last-first>4000) return VV_BACKEND_LIMIT;
  memcpy(out,s+first,last-first);out[last-first]=0;return 0;
}
int vv_backend_tts_json(const char *text, char *out, size_t cap, size_t *written) {
  if (written) *written=0;
  if (out && cap) out[0]=0;
  char trimmed[4001];int rc=tts_trim(text,trimmed);if(rc)return rc;
  cJSON *root=cJSON_CreateObject();
  if (!root || !cJSON_AddStringToObject(root,"text",trimmed)) { cJSON_Delete(root); return VV_BACKEND_LIMIT; }
  return emit(root,out,cap,written);
}
static int inspect(cJSON *v, unsigned depth) {
  if (depth>16) return VV_BACKEND_LIMIT;
  for (cJSON *a=v->child;a;a=a->next) {
    if (cJSON_IsObject(v)) {
      if (!a->string) return VV_BACKEND_INVALID;
      for(cJSON *b=a->next;b;b=b->next)
        if (b->string && !strcmp(a->string,b->string)) return VV_BACKEND_INVALID;
      if (!strcmp(a->string,"mock") && cJSON_IsTrue(a)) return VV_BACKEND_MOCK;
    }
    int rc=inspect(a,depth+1); if(rc) return rc;
  }
  return 0;
}
static int parse(const char *json,size_t size,cJSON **root,cJSON **data) {
  *root=NULL;
  if (!json || !size) return VV_BACKEND_INVALID;
  if (size>VV_BACKEND_JSON_MAX) return VV_BACKEND_LIMIT;
  /* Reject embedded NUL, escaped NUL and excessive nesting before allocation.
   * Escaped backslashes are consumed together, so literal "\\u0000" is safe. */
  bool quoted=false; unsigned depth=0;
  for(size_t i=0;i<size;++i) {
    unsigned char c=(unsigned char)json[i];
    if (!c) return VV_BACKEND_INVALID;
    if (quoted && c=='\\') {
      if (++i>=size) return VV_BACKEND_INVALID;
      if (json[i]=='u' && i+4<size && !memcmp(json+i+1,"0000",4)) return VV_BACKEND_INVALID;
      continue;
    }
    if (c=='"') quoted=!quoted;
    else if (!quoted && (c=='{' || c=='[')) { if (++depth>16) return VV_BACKEND_LIMIT; }
    else if (!quoted && (c=='}' || c==']')) { if(!depth) return VV_BACKEND_INVALID; --depth; }
  }
  const char *end=NULL;
  *root=cJSON_ParseWithLengthOpts(json,size,&end,0);
  if (!*root || !cJSON_IsObject(*root)) return VV_BACKEND_INVALID;
  while (end<json+size && (*end==' ' || *end=='\t' || *end=='\r' || *end=='\n')) ++end;
  if (end!=json+size) return VV_BACKEND_INVALID;
  int rc=inspect(*root,0); if(rc) return rc;
  *data=cJSON_GetObjectItemCaseSensitive(*root,"data");
  return cJSON_IsObject(*data)?0:VV_BACKEND_INVALID;
}
static const char *field(cJSON *obj,const char *name,size_t max) {
  cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,name);
  return cJSON_IsString(v) && string_ok(v->valuestring,max)?v->valuestring:NULL;
}
static bool uuid(const char *s) {
  if (!s || strlen(s)!=36) return false;
  for(unsigned i=0;i<36;++i)
    if(i==8 || i==13 || i==18 || i==23) { if(s[i]!='-') return false; }
    else if(!isxdigit((unsigned char)s[i])) return false;
  return true;
}
static bool expiry(const char *s) {
  if (!s || strlen(s)!=20) return false;
  for(unsigned i=0;i<20;++i) {
    char sep=i==4 || i==7?'-':i==10?'T':i==13 || i==16?':':i==19?'Z':0;
    if(sep?s[i]!=sep:!isdigit((unsigned char)s[i])) return false;
  }
  unsigned year=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
  unsigned month=(s[5]-'0')*10+s[6]-'0', day=(s[8]-'0')*10+s[9]-'0';
  static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(!year || month<1 || month>12) return false;
  unsigned limit=days[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0));
  return day>=1 && day<=limit && (s[11]-'0')*10+s[12]-'0'<24 &&
    (s[14]-'0')*10+s[15]-'0'<60 && (s[17]-'0')*10+s[18]-'0'<60;
}

#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
/* The cloud's explicit ISO profile accepts microseconds and numeric offsets.
 * Floor fractional seconds; never extend the credential lifetime. Keep the
 * public session ABI as strict UTC seconds and use no local-time conversion. */
static unsigned cloud_month_days(unsigned year,unsigned month) {
  static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return days[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0));
}
static bool cloud_expiry(const char *s,char out[21]) {
  if(!s) return false;
  size_t n=strlen(s);if(n<20 || n>32)return false;
  char utc[21];memcpy(utc,s,19);utc[19]='Z';utc[20]=0;
  if(!expiry(utc))return false;
  size_t i=19;
  if(s[i]=='.') {
    size_t start=++i;
    while(i<n && s[i]>='0' && s[i]<='9')++i;
    if(i==start || i-start>6)return false;
  }
  int offset=0;
  if(i+1==n && s[i]=='Z') {}
  else {
    if(n-i!=6 || (s[i]!='+' && s[i]!='-') || s[i+3]!=':')return false;
    unsigned slots[]={1,2,4,5};
    for(unsigned k=0;k<4;k++)if(s[i+slots[k]]<'0'||s[i+slots[k]]>'9')return false;
    unsigned hours=(s[i+1]-'0')*10+s[i+2]-'0',minutes=(s[i+4]-'0')*10+s[i+5]-'0';
    if(hours>23 || minutes>59)return false;
    /* RFC unknown-local-offset spelling is not an authenticated UTC offset. */
    if(s[i]=='-' && !hours && !minutes)return false;
    offset=(int)(hours*60+minutes);if(s[i]=='-')offset=-offset;
  }
  unsigned year=(utc[0]-'0')*1000+(utc[1]-'0')*100+(utc[2]-'0')*10+utc[3]-'0';
  unsigned month=(utc[5]-'0')*10+utc[6]-'0',day=(utc[8]-'0')*10+utc[9]-'0';
  int minutes=((utc[11]-'0')*10+utc[12]-'0')*60+(utc[14]-'0')*10+utc[15]-'0'-offset;
  if(minutes<0) {
    minutes+=1440;
    if(day>1)--day;
    else {if(month>1)--month;else {if(year==1)return false;--year;month=12;}day=cloud_month_days(year,month);}
  } else if(minutes>=1440) {
    minutes-=1440;
    if(day<cloud_month_days(year,month))++day;
    else {day=1;if(month<12)++month;else {if(year==9999)return false;++year;month=1;}}
  }
  utc[0]=(char)('0'+year/1000);utc[1]=(char)('0'+year/100%10);
  utc[2]=(char)('0'+year/10%10);utc[3]=(char)('0'+year%10);
  utc[5]=(char)('0'+month/10);utc[6]=(char)('0'+month%10);
  utc[8]=(char)('0'+day/10);utc[9]=(char)('0'+day%10);
  utc[11]=(char)('0'+minutes/60/10);utc[12]=(char)('0'+minutes/60%10);
  utc[14]=(char)('0'+minutes%60/10);utc[15]=(char)('0'+minutes%60%10);
  memcpy(out,utc,sizeof(utc));return true;
}
#endif
int vv_backend_parse_session(const char *json,size_t size,struct vv_backend_session *out) {
  if(!out) return VV_BACKEND_INVALID;
  memset(out,0,sizeof(*out)); cJSON *root=NULL,*data=NULL;
  int rc=parse(json,size,&root,&data);
  if(!rc) {
#if defined(K7_CLOUD_BUSINESS_PROFILE) && K7_CLOUD_BUSINESS_PROFILE
    const char *token=field(data,"sessionToken",VV_BACKEND_TOKEN_CAP-1), *time=field(data,"expiresAt",32);
    char normalized[21];
    if(!token || !cloud_expiry(time,normalized)) rc=VV_BACKEND_INVALID;
    else { strcpy(out->token,token); memcpy(out->expires_at,normalized,sizeof(normalized)); }
#else
    const char *token=field(data,"sessionToken",VV_BACKEND_TOKEN_CAP-1), *time=field(data,"expiresAt",20);
    if(!token || !expiry(time)) rc=VV_BACKEND_INVALID;
    else { strcpy(out->token,token); strcpy(out->expires_at,time); }
#endif
  }
  cJSON_Delete(root); return rc;
}
int vv_backend_parse_task(const char *json,size_t size,const char *expected,struct vv_backend_task *out) {
  if(!out) return VV_BACKEND_INVALID;
  memset(out,0,sizeof(*out));
  if(!uuid(expected)) return VV_BACKEND_INVALID;
  cJSON *root=NULL,*data=NULL; int rc=parse(json,size,&root,&data);
  struct vv_backend_task tmp={0};
  if(!rc) {
    const char *id=field(data,"taskId",36), *status=field(data,"status",32);
    if(!id || strcmp(id,expected) || !status) { rc=VV_BACKEND_INVALID; goto done; }
    strcpy(tmp.task_id,id);
    if(!strcmp(status,"queued")) tmp.status=VV_TASK_QUEUED;
    else if(!strcmp(status,"analyzing")) tmp.status=VV_TASK_ANALYZING;
    else if(!strcmp(status,"report_ready")) {
      tmp.status=VV_TASK_REPORT_READY; const char *report=field(data,"reportId",36);
      if(!uuid(report)) { rc=VV_BACKEND_INVALID; goto done; } strcpy(tmp.report_id,report);
    } else if(!strcmp(status,"failed")) {
      tmp.status=VV_TASK_FAILED; const char *code=field(data,"failureCode",128);
      if(!code) { rc=VV_BACKEND_INVALID; goto done; } strcpy(tmp.failure_code,code);
    } else if(!strcmp(status,"needs_retake")) {
      tmp.status=VV_TASK_NEEDS_RETAKE;
      cJSON *views=cJSON_GetObjectItemCaseSensitive(data,"requiredViews");
      if(!cJSON_IsArray(views) || !views->child) { rc=VV_BACKEND_INVALID; goto done; }
      for(cJSON *v=views->child;v;v=v->next) {
        if(!cJSON_IsString(v)) { rc=VV_BACKEND_INVALID; goto done; }
        unsigned bit=!strcmp(v->valuestring,"front")?VV_VIEW_FRONT:!strcmp(v->valuestring,"left")?VV_VIEW_LEFT:!strcmp(v->valuestring,"right")?VV_VIEW_RIGHT:0;
        if(!bit || (tmp.required_views&bit)) { rc=VV_BACKEND_INVALID; goto done; }
        tmp.required_views|=bit;
      }
    } else rc=VV_BACKEND_INVALID;
  }
done:
  if(!rc) *out=tmp;
  cJSON_Delete(root); return rc;
}
static bool token_char(unsigned char c) {
  return isalnum(c) || (c && strchr("!#$%&'*+-.^_`|~",c));
}
bool vv_backend_tts_content_type(const char *s) {
  if(!s || strlen(s)>127) return false;
  while(*s==' ' || *s=='\t') ++s;
  const char *base="audio/pcm";
  while(*base) if(tolower((unsigned char)*s++)!=*base++) return false;
  unsigned seen=0;
  for(;;) {
    while(*s==' ' || *s=='\t') ++s;
    if(!*s) return true;
    if(*s++!=';') return false;
    while(*s==' ' || *s=='\t') ++s;
    const char *start=s; while(token_char((unsigned char)*s)) ++s;
    if(s==start || *s++!='=') return false;
    size_t name_len=(size_t)(s-start-1);char name[128],value[128];size_t used=0;
    for(size_t j=0;j<name_len;j++)name[j]=(char)tolower((unsigned char)start[j]);
    name[name_len]=0;
    if(*s=='"') {
      ++s; while(*s && *s!='"') {
        if(*s=='\\') { ++s; if(!*s) return false; }
        if((unsigned char)*s<32 || (unsigned char)*s==127) return false;
        value[used++]=*s++;
      }
      if(*s++!='"') return false;
    } else {
      start=s; while(token_char((unsigned char)*s)) ++s;
      if(s==start) return false;
      used=(size_t)(s-start);memcpy(value,start,used);
    }
    value[used]=0;
    unsigned bit=0;const char *expected=NULL;
    if(!strcmp(name,"rate")){bit=1;expected="16000";}
    else if(!strcmp(name,"channels")){bit=2;expected="1";}
    else if(!strcmp(name,"encoding")){bit=4;expected="signed-int";}
    else if(!strcmp(name,"bits")){bit=8;expected="16";}
    if(bit) {if((seen&bit)||strcmp(value,expected))return false;seen|=bit;}
  }
}
