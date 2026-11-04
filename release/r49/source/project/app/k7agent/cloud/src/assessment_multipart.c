#include "assessment_multipart.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int identifier(const char *s)
{
  if(!s||!*s)return 0;
  size_t n=0;
  for(;s[n];n++) {
    unsigned char c=s[n];
    if(n>=128 || !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                  c=='-'||c=='_'||c==':'||c=='.'))return 0;
  }
  return 1;
}
int k7_assessment_body_init(struct k7_assessment_body *b,const char *session,
                           const char *consent,const char *token,
                           const struct k7_upload_image images[3])
{
  if(!b)return -EINVAL;
  memset(b,0,sizeof(*b));
  if(!identifier(session)||!identifier(consent)||!token||strlen(token)!=32||!images)return -EINVAL;
  for(unsigned i=0;i<32;i++)if(!((token[i]>='0'&&token[i]<='9')||(token[i]>='a'&&token[i]<='f')))return -EINVAL;
  for(unsigned i=0;i<3;i++) {
    const unsigned char *p=images[i].data;size_t n=images[i].bytes;
    if(!p||n<4||n>1024*1024||p[0]!=255||p[1]!=216||p[n-2]!=255||p[n-1]!=217)return -EINVAL;
    for(size_t j=0;j+32<=n;j++)if(!memcmp(p+j,token,32))return -EEXIST;
  }
  if(strstr(session,token)||strstr(consent,token))return -EEXIST;
  const char *names[]={"metadata","front","left","right"};
  snprintf(b->content_type,sizeof(b->content_type),"multipart/form-data; boundary=K7-%s",token);
  snprintf(b->metadata,sizeof(b->metadata),"{\"photoVersion\":\"1\",\"captureSessionId\":\"%s\",\"consentEvidenceRef\":\"%s\"}",session,consent);
  for(unsigned i=0;i<4;i++) {
    if(!i)snprintf(b->headers[i],sizeof(b->headers[i]),
      "--K7-%s\r\nContent-Disposition: form-data; name=\"metadata\"\r\nContent-Type: application/json\r\n\r\n",token);
    else snprintf(b->headers[i],sizeof(b->headers[i]),
      "--K7-%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n",token,names[i],names[i]);
    b->segments[i*3]=b->headers[i];b->lengths[i*3]=strlen(b->headers[i]);
    b->segments[i*3+1]=i?(const void *)images[i-1].data:(const void *)b->metadata;
    b->lengths[i*3+1]=i?images[i-1].bytes:strlen(b->metadata);
    b->segments[i*3+2]="\r\n";b->lengths[i*3+2]=2;
  }
  snprintf(b->trailer,sizeof(b->trailer),"--K7-%s--\r\n",token);
  b->segments[12]=b->trailer;b->lengths[12]=strlen(b->trailer);
  for(unsigned i=0;i<13;i++)b->total+=b->lengths[i];
  return 0;
}
ssize_t k7_assessment_body_read(void *arg,size_t offset,void *dst,size_t capacity)
{
  struct k7_assessment_body *b=arg;
  if(!b||!b->total||(!dst&&capacity)||offset>b->total)return -EINVAL;
  unsigned char *out=dst;size_t copied=0;
  for(unsigned i=0;i<13&&copied<capacity;i++) {
    if(offset>=b->lengths[i]){offset-=b->lengths[i];continue;}
    size_t n=b->lengths[i]-offset;if(n>capacity-copied)n=capacity-copied;
    memcpy(out+copied,(const unsigned char *)b->segments[i]+offset,n);
    copied+=n;offset=0;
  }
  return (ssize_t)copied;
}
