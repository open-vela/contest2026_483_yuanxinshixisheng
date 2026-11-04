#include "vv_wss_incremental.h"
#include <errno.h>
#include <string.h>
static void wipe(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
void vv_wsi_abort(struct vv_wsi *s){if(s&&s->active){s->active=false;s->io.close(s->io.arg);wipe(s->tx,sizeof(s->tx));wipe(s->pending,sizeof(s->pending));if(s->used)wipe(s->message,s->used);}}
static int fail(struct vv_wsi *s,int rc){vv_wsi_abort(s);return rc;}
static int utf8(const uint8_t *p,size_t n){size_t i=0;while(i<n){uint32_t v=p[i++],min;unsigned k;
 if(v<128)continue;
 if(v>=194&&v<=223){k=1;min=128;v&=31;}else if(v>=224&&v<=239){k=2;min=2048;v&=15;}else if(v>=240&&v<=244){k=3;min=65536;v&=7;}else return 0;
 if(n-i<k)return 0;
 while(k--){unsigned b=p[i++];if((b&192)!=128)return 0;v=(v<<6)|(b&63);}
 if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;}return 1;}
static int valid_close(const uint8_t *p,size_t n){if(n==1)return 0;if(!n)return 1;unsigned v=((unsigned)p[0]<<8)|p[1];return ((v>=1000&&v<=1014&&v!=1004&&v!=1005&&v!=1006)||(v>=3000&&v<=4999))&&utf8(p+2,n-2);}
int vv_wsi_init(struct vv_wsi *s,const struct vv_wsi_ops *io,void *storage,size_t cap,uint64_t now,uint64_t deadline){
 if(!s||!io||!io->read||!io->write||!io->random||!io->close||!io->message||!storage||!cap||deadline<=now)return -EINVAL;
 /* Fresh zero-initialized or previously closed storage only. */
 if(s->active)return -EBUSY;
 memset(s,0,sizeof(*s));s->io=*io;s->message=storage;s->capacity=cap;s->active=true;s->need=2;s->last_now=now;s->deadline=deadline;return 0;
}
int vv_wsi_queue(struct vv_wsi *s,unsigned op,const void *data,size_t n){
 if(!s||!s->active||s->close_after||s->tx_close)return -EINVAL;
 if(s->tx_length)return -EAGAIN;
 if((!data&&n)||n>VV_WSI_TX_CAP||(op!=1&&op!=2&&op!=8&&op!=9&&op!=10)||(op>=8&&n>125))return -EINVAL;
 if((op==1&&!utf8(data,n))||(op==8&&!valid_close(data,n)))return -EINVAL;
 uint8_t mask[4];if(s->io.random(s->io.arg,mask,4))return fail(s,-EIO);
 size_t h=2;s->tx[0]=(uint8_t)(128|op);s->tx[1]=(uint8_t)(128|n);
 if(n>=126){s->tx[1]=254;s->tx[2]=(uint8_t)(n>>8);s->tx[3]=(uint8_t)n;h=4;}
 memcpy(s->tx+h,mask,4);h+=4;
 for(size_t i=0;i<n;i++)s->tx[h+i]=((const uint8_t *)data)[i]^mask[i%4];
 s->tx_length=h+n;s->tx_offset=0;s->tx_close=op==8;return 0;
}
static void reset_frame(struct vv_wsi *s){s->have=0;s->need=2;s->parsed=false;s->frame_read=0;s->frame_length=0;}
static int finish(struct vv_wsi *s){
 if(s->opcode>=8){if(s->opcode==8&&!valid_close(s->control,(size_t)s->frame_length))return fail(s,-EPROTO);
 if(s->opcode==8||s->opcode==9){s->pending_opcode=s->opcode==9?10:8;s->pending_length=(size_t)s->frame_length;memcpy(s->pending,s->control,s->pending_length);}
 }else{if(s->fin){if(s->message_opcode==1&&!utf8(s->message,s->used))return fail(s,-EPROTO);
 int rc=s->io.message(s->io.arg,s->message_opcode,s->message,s->used);if(rc)return fail(s,rc<0?rc:-EIO);
 wipe(s->message,s->used);s->used=0;s->message_opcode=0;}}
 reset_frame(s);return 0;
}
static int parse(struct vv_wsi *s){uint8_t *h=s->header;unsigned op=h[0]&15;uint64_t n=h[1]&127;
 if((h[0]&112)||(h[1]&128)||(op!=0&&op!=1&&op!=2&&op!=8&&op!=9&&op!=10)||(op>=8&&(!(h[0]&128)||n>125)))return fail(s,-EPROTO);
 if(n>=126){unsigned bytes=n==126?2:8;if(s->need==2){s->need=2+bytes;return 0;}if(bytes==8&&(h[2]&128))return fail(s,-EPROTO);n=0;for(unsigned i=0;i<bytes;i++)n=(n<<8)|h[2+i];if((bytes==2&&n<126)||(bytes==8&&n<=65535))return fail(s,-EPROTO);}
 if(op<8){if((op==0&&!s->message_opcode)||(op!=0&&s->message_opcode))return fail(s,-EPROTO);if(op)s->message_opcode=op;if(n>s->capacity-s->used)return fail(s,-EMSGSIZE);}
 s->opcode=op;s->fin=(h[0]&128)!=0;s->frame_length=n;s->parsed=true;return n?0:finish(s);
}
int vv_wsi_pump(struct vv_wsi *s,uint64_t now,bool cancelled,unsigned budget){
 if(!s||!s->active)return -EINVAL;
 if(cancelled)return fail(s,-ECANCELED);
 if(now<s->last_now)return fail(s,-EINVAL);
 s->last_now=now;if(now>=s->deadline)return fail(s,-ETIMEDOUT);
 for(unsigned i=0;i<budget&&s->active;i++){
  if(!s->tx_length&&s->pending_opcode){unsigned op=s->pending_opcode;s->pending_opcode=0;int e=vv_wsi_queue(s,op,s->pending,s->pending_length);if(e)return fail(s,e);if(op==8)s->close_after=true;}
  bool progress=false,write_blocked=false;
  if(s->tx_length){size_t remain=s->tx_length-s->tx_offset;ssize_t k=s->io.write(s->io.arg,s->tx+s->tx_offset,remain);
   if(k==VV_WSI_BLOCK){write_blocked=true;}else if(k<=0||(size_t)k>remain)return fail(s,k<0?(int)k:-EIO);else{s->tx_offset+=(size_t)k;progress=true;if(s->tx_offset==s->tx_length){wipe(s->tx,s->tx_length);s->tx_length=s->tx_offset=0;if(s->tx_close){vv_wsi_abort(s);return 1;}}}}
  /* Do not consume another control frame until the previous reply is queued. */
  if(!write_blocked&&!s->pending_opcode&&!s->close_after&&!s->tx_close){void *dst;size_t want;
   if(!s->parsed){dst=s->header+s->have;want=s->need-s->have;}else{want=(size_t)(s->frame_length-s->frame_read);dst=s->opcode>=8?s->control+s->frame_read:s->message+s->used;}
   ssize_t k=s->io.read(s->io.arg,dst,want);
   if(k==VV_WSI_BLOCK){}else if(k<=0||(size_t)k>want)return fail(s,k<0?(int)k:-EIO);else{progress=true;int e=0;if(!s->parsed){s->have+=(size_t)k;if(s->have==s->need)e=parse(s);}else{s->frame_read+=(size_t)k;if(s->opcode<8)s->used+=(size_t)k;if(s->frame_read==s->frame_length)e=finish(s);}if(e)return e;}}
  if(!progress)break;
 }
 return 0;
}
