/* RFC6455 sections 4.1, 5, 7, 8. No extensions or implicit reconnect. */
#include "vv_wss_client.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
static int gate(struct vv_wss_client *c,uint64_t d,const struct vv_https_cancel *x){
 if(x&&x->is_cancelled&&x->is_cancelled(x->arg))return -ECANCELED;
 
 return c->now(c->now_arg)>=d?-ETIMEDOUT:0;
 
}
void vv_wss_abort(struct vv_wss_client *c){if(c&&c->open){c->io->close(c->io_arg);
 c->open=false;
 }}
static int fail(struct vv_wss_client *c,int e){vv_wss_abort(c);
 return e;
 }
static int transfer(struct vv_wss_client *c,void *p,size_t n,bool wr,uint64_t d,const struct vv_https_cancel *x){
 uint8_t *b=p;
 while(n){int e=gate(c,d,x);
 if(e)return e;
 
 ssize_t k=wr?c->io->write(c->io_arg,b,n,d,x):c->io->read(c->io_arg,b,n,d,x);
 
 if(k<=0)return k<0?(int)k:-EIO;
 if((size_t)k>n)return -EPROTO;
 b+=k;
 n-=(size_t)k;
 }return 0;
 
}
static void b64(const uint8_t *s,size_t n,char *o){
 static const char a[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 
 size_t j=0;
 for(size_t i=0;i<n;i+=3){unsigned v=(unsigned)s[i]<<16;
 
 if(i+1<n)v|=(unsigned)s[i+1]<<8;
 if(i+2<n)v|=s[i+2];
 
 o[j++]=a[v>>18];
 o[j++]=a[(v>>12)&63];
 o[j++]=i+1<n?a[(v>>6)&63]:'=';
 o[j++]=i+2<n?a[v&63]:'=';
 }o[j]=0;
 
}
static int eq(const char *a,const char *b){while(*a&&*b){if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return 0;
 }return !*a&&!*b;
 }
static int token(char *s,const char *want){char *p=s;
 while(*p){char *q=strchr(p,',');
 if(q)*q=0;
 
 while(*p==' '||*p=='\t')p++;
 char *e=p+strlen(p);
 while(e>p&&(e[-1]==' '||e[-1]=='\t'))*--e=0;
 
 if(eq(p,want))return 1;
 if(!q)break;
 p=q+1;
 }return 0;
 }
static int clean(const char *s,bool spaces){if(!s)return 0;
 for(;*s;s++)if((unsigned char)*s< (spaces?32:33)||(unsigned char)*s>126)return 0;
 return 1;
 }
int vv_wss_open(struct vv_wss_client *c,const char *host,uint16_t port,const char *path,const char *auth,uint64_t d,const struct vv_https_cancel *x){
 if(!c||c->open||!c->io||!c->io->connect||!c->io->read||!c->io->write||!c->io->close||!c->now||!c->random||!c->sha1||!c->max_message||!clean(host,false)||!*host||!clean(path,false)||*path!='/'||(auth&&!clean(auth,true)))return -EINVAL;
 
 char request[4096],response[4096],key[25],expected[29],input[61];
 uint8_t nonce[16],digest[20];
 
 int e=gate(c,d,x);
 if(e)return e;
 if(c->random(c->random_arg,nonce,16))return -EIO;
 b64(nonce,16,key);
 
 snprintf(input,sizeof(input),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
 
 if(c->sha1((const uint8_t *)input,60,digest))return -EIO;
 b64(digest,20,expected);
 
 int len=snprintf(request,sizeof(request),"GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n%s%s%s\r\n",path,host,port?port:443,key,auth?"Authorization: ":"",auth?auth:"",auth?"\r\n":"");
 
 if(len<0||(size_t)len>=sizeof(request)){vv_https_secure_zero(request,sizeof(request));
 return -EMSGSIZE;
 }
 struct vv_https_peer_security sec={0};
 e=c->io->connect(c->io_arg,host,port?port:443,d,x,&sec);
 
 if(e){c->io->close(c->io_arg);vv_https_secure_zero(request,sizeof(request));
 return e;
 }c->open=true;
 
 if(!sec.sni_set||!sec.peer_verified||sec.verify_flags){vv_https_secure_zero(request,sizeof(request));
 return fail(c,VV_HTTPS_ETLS_VERIFY);
 }
 e=transfer(c,request,(size_t)len,true,d,x);
 vv_https_secure_zero(request,sizeof(request));
 if(e)return fail(c,e);
 
 size_t n=0;
 while(n+1<sizeof(response)){e=transfer(c,response+n,1,false,d,x);
 if(e)return fail(c,e);
 if((unsigned char)response[n]==127 || ((unsigned char)response[n]<32 && response[n]!='\r' && response[n]!='\n' && response[n]!='\t'))return fail(c,-EPROTO);
 n++;
 if(n>=4&&!memcmp(response+n-4,"\r\n\r\n",4))break;
 }
 if(n<4||memcmp(response+n-4,"\r\n\r\n",4))return fail(c,-EMSGSIZE);
 response[n]=0;
 
 char *p=strstr(response,"\r\n");
 if(!p)return fail(c,-EPROTO);
 *p=0;
 
 if(strncmp(response,"HTTP/1.1 101",12)|| (response[12]&&response[12]!=' '))return fail(c,-EPROTO);
 
 int up=0,conn=0,accept=0;
 p+=2;
 
 while(*p){char *end=strstr(p,"\r\n");
 if(!end)return fail(c,-EPROTO);
 if(end==p)break;
 *end=0;
 if(*p==' '||*p=='\t')return fail(c,-EPROTO);
 char *v=strchr(p,':');
 if(!v || v==p)return fail(c,-EPROTO);
 for(char *t=p;t<v;t++)if(!isalnum((unsigned char)*t)&&!strchr("!#$%&'*+-.^_`|~",*t))return fail(c,-EPROTO);
 *v++=0;
 for(char *t=v;*t;t++)if((unsigned char)*t<32&&*t!='\t')return fail(c,-EPROTO);
 
 while(*v==' '||*v=='\t')v++;
 char *tail=v+strlen(v);
 while(tail>v&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
 
 if(eq(p,"Upgrade")){if(up++||!eq(v,"websocket"))return fail(c,-EPROTO);
 }
 else if(eq(p,"Connection")){if(conn++||!token(v,"Upgrade"))return fail(c,-EPROTO);
 }
 else if(eq(p,"Sec-WebSocket-Accept")){if(accept++||strcmp(v,expected))return fail(c,-EPROTO);
 }
 else if(eq(p,"Sec-WebSocket-Protocol")||eq(p,"Sec-WebSocket-Extensions"))return fail(c,-EPROTO);
 
 p=end+2;
 }
 return up&&conn&&accept?0:fail(c,-EPROTO);
 
}
static int utf8(const uint8_t *p,size_t n){size_t i=0;
 while(i<n){uint32_t v=p[i++];
 unsigned k;
 uint32_t min;
 
 if(v<128)continue;
 if(v>=194&&v<=223){k=1;
 min=128;
 v&=31;
 }else if(v>=224&&v<=239){k=2;
 min=2048;
 v&=15;
 }else if(v>=240&&v<=244){k=3;
 min=65536;
 v&=7;
 }else return 0;
 
 if(n-i<k)return 0;
 while(k--){unsigned b=p[i++];
 if((b&192)!=128)return 0;
 v=(v<<6)|(b&63);
 }if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
 }return 1;
 }
static int close_ok(const uint8_t *p,size_t n){if(n==1)return 0;
 if(!n)return 1;
 unsigned v=((unsigned)p[0]<<8)|p[1];
 return ((v>=1000&&v<=1014&&v!=1004&&v!=1005&&v!=1006)||(v>=3000&&v<=4999))&&utf8(p+2,n-2);
 }
int vv_wss_send(struct vv_wss_client *c,unsigned op,const void *data,size_t n,uint64_t d,const struct vv_https_cancel *x){
 if(!c||!c->open||(!data&&n)||(op!=1&&op!=2&&op!=8&&op!=9&&op!=10)||n>c->max_message||(op>=8&&n>125))return -EINVAL;
 
 const uint8_t *p=data;
 if((op==1&&!utf8(p,n))||(op==8&&!close_ok(p,n)))return -EINVAL;
 
 uint8_t h[14],mask[4],buf[512];
 size_t hn=2;
 int e=gate(c,d,x);
 if(e)return fail(c,e);
 
 if(c->random(c->random_arg,mask,4))return fail(c,-EIO);
 h[0]=(uint8_t)(128|op);
 
 if(n<126)h[1]=(uint8_t)(128|n);
 else if(n<=65535){h[1]=254;
 h[2]=(uint8_t)(n>>8);
 h[3]=(uint8_t)n;
 hn=4;
 }else{h[1]=255;
 for(unsigned i=0;i<8;i++)h[2+i]=(uint8_t)((uint64_t)n>>(56-8*i));
 hn=10;
 }
 memcpy(h+hn,mask,4);
 hn+=4;
 e=transfer(c,h,hn,true,d,x);
 
 for(size_t off=0;!e&&off<n;){size_t k=n-off;
 if(k>sizeof(buf))k=sizeof(buf);
 for(size_t i=0;i<k;i++)buf[i]=p[off+i]^mask[(off+i)%4];
 e=transfer(c,buf,k,true,d,x);
 off+=k;
 }
 vv_https_secure_zero(buf,sizeof(buf));
 if(e)return fail(c,e);
 if(op==8)vv_wss_abort(c);
 return 0;
 
}
int vv_wss_recv(struct vv_wss_client *c,unsigned *opcode,void *dst,size_t cap,size_t *used,uint64_t d,const struct vv_https_cancel *x){
 if(!c||!c->open||!opcode||!used||(!dst&&cap))return -EINVAL;
 *used=0;
 unsigned message=0;
 
 for(;;){uint8_t h[8];
 int e=transfer(c,h,2,false,d,x);
 if(e)return fail(c,e);
 
 unsigned op=h[0]&15,fin=h[0]&128;
 uint64_t n=h[1]&127;
 
 if((h[0]&112)||(h[1]&128)||(op!=0&&op!=1&&op!=2&&op!=8&&op!=9&&op!=10)||(op>=8&&(!fin||n>125)))return fail(c,-EPROTO);
 
 if(n>=126){unsigned bytes=n==126?2:8;
 e=transfer(c,h,bytes,false,d,x);
 if(e)return fail(c,e);
 if(bytes==8&&(h[0]&128))return fail(c,-EPROTO);
 n=0;
 for(unsigned i=0;i<bytes;i++)n=(n<<8)|h[i];
 if((bytes==2&&n<126)||(bytes==8&&n<=65535))return fail(c,-EPROTO);
 }
 if(op>=8){uint8_t control[125];
 e=transfer(c,control,(size_t)n,false,d,x);
 if(e)return fail(c,e);
 
 if(op==9){e=vv_wss_send(c,10,control,(size_t)n,d,x);
 if(e)return e;
 }else if(op==8){if(!close_ok(control,(size_t)n))return fail(c,-EPROTO);
 e=vv_wss_send(c,8,control,(size_t)n,d,x);
 return e?e:1;
 }continue;
 }
 if((op==0&&!message)||(op!=0&&message))return fail(c,-EPROTO);
 if(op)message=op;
 
 if(n>cap-*used||n>c->max_message-*used)return fail(c,-EMSGSIZE);
 
 e=transfer(c,n?(uint8_t *)dst+*used:NULL,(size_t)n,false,d,x);
 if(e)return fail(c,e);
 *used+=(size_t)n;
 
 if(fin){if(message==1&&!utf8(dst,*used))return fail(c,-EPROTO);
 *opcode=message;
 return 0;
 }}
}

