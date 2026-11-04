/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "duplex.h"
#include <stddef.h>
int rx2_pair_ready(uint32_t raw);
#include <string.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#include "input/rockchip_sai.h"
static unsigned count(uint32_t v)
{return (v&63)+((v>>6)&63)+((v>>12)&63)+((v>>18)&63);}
static int rd(const struct dl_port*p,uint32_t o,uint32_t*v)
{return p->read(p->ctx,o,v)?-5:0;}
static int wr(const struct dl_port*p,uint32_t o,uint32_t v)
{return p->write(p->ctx,o,v)?-5:0;}
static int mod(const struct dl_port*p,uint32_t o,uint32_t m,uint32_t x)
{uint32_t v;if(rd(p,o,&v))return -5;return wr(p,o,(v&~m)|(x&m));}
static int expect(const struct dl_port*p,uint32_t o,uint32_t mask,uint32_t want)
{uint32_t v;if(rd(p,o,&v))return -5;return (v&mask)==want?0:-71;}
static int low(const struct dl_port*p)
{uint64_t a=p->us(p->ctx),b;int e=p->amp_low(p->ctx);b=p->us(p->ctx);
 return e?-5:(b<a||b-a>=100?-110:0);}
static int delay20(const struct dl_port*p)
{unsigned i;uint64_t a=p->us(p->ctx),b;for(i=0;i<100000;i++){
 b=p->us(p->ctx);if(b<a)return -5;if(b-a>=20)return 0;}return -110;}
/* Frozen pio stop order generalized to BOTH streams: stop -> both idle ->
 * clear both while CLK/FS remain on -> wait clear -> stop clocks -> FS idle.
 * Pending markers are deliberately discarded, never called transmitted. */
static int stop(const struct dl_port*p)
{
 unsigned i;uint32_t v;uint64_t a=p->us(p->ctx),b;
 if(mod(p,SAI_XFER,12,0))return -5;
 for(i=0;i<10000;i++){if(rd(p,SAI_STATUS,&v))return -5;
  b=p->us(p->ctx);if(b<a||b-a>=1000)return -110;if((v&12)==12)break;}
 if(i==10000)return -110;
 if(mod(p,SAI_CLR,3,3))return -5;
 for(i=0;i<10000;i++){if(rd(p,SAI_CLR,&v))return -5;
  b=p->us(p->ctx);if(b<a||b-a>=1000)return -110;if(!(v&3))break;}
 if(i==10000)return -110;
 if(mod(p,SAI_XFER,3,0))return -5;
 for(i=0;i<10000;i++){if(rd(p,SAI_STATUS,&v))return -5;
  b=p->us(p->ctx);if(b<a||b-a>=2000)return -110;if(v&2)break;}
 if(i==10000)return -110;
 if(expect(p,SAI_XFER,15,0)||expect(p,SAI_TXFIFOLR,0xffffff,0)||
    expect(p,SAI_RXFIFOLR,0xffffff,0))return -71;
 return 0;
}
int dl_numbered_marker(unsigned n,uint32_t*out)
{
 if(!out||n>=288u)return -22;
 *out=0x01000000u|((n&7u)<<20)|(((n>>3)+1u)<<8);
 return 0;
}
static uint32_t marker(unsigned n,int numbered)
{static const uint32_t a[8]={0x01012300,0x02024500,0x03036700,0x04048900,
 0x0505ab00,0x0606cd00,0x0707ef00,0x08081100};
 /* Numbered mode is fixed128 frames; enqueue cap enforces n<288. */
 if(numbered)return 0x01000000u|((n&7u)<<20)|(((n>>3)+1u)<<8);
 return a[n&7];}
static int word(const struct dl_port*p,int tx,uint32_t*w,struct dl_result*r)
{
 struct dl_word*e=NULL;unsigned *n=tx?&r->ntx:&r->nrx;
 uint32_t fifo=tx?SAI_TXFIFOLR:SAI_RXFIFOLR;int rc;
 if(*n<DL_TRACE){e=tx?&r->tx[(*n)++]:&r->rx[(*n)++];e->us=p->us(p->ctx);
  e->rc=rd(p,fifo,&e->before);if(e->rc)return e->rc;}
 rc=tx?wr(p,SAI_TXDR,*w):rd(p,SAI_RXDR,w);
 if(e){e->rc=rc;if(!rc){e->word=*w;e->rc=rd(p,fifo,&e->after);}
  return e->rc;}
 return rc;
}
static int run_route(const struct dl_port*p,int prepared,int common,uint32_t mclk,
 uint32_t*capture,unsigned capacity,unsigned frames,int route,int numbered,unsigned rx_lanes,int guarded,struct dl_result*r)
{
 uint32_t v,t,x,mask,value,rxvalue,pathmask;unsigned i;uint64_t now;
 int rc=-5,clocks_attempted=0,path_saved=0,rx_saved=0,ready=0;
 if(!r)return -22;
 memset(r,0,sizeof(*r));
 if(guarded&&rx_lanes!=2)return r->result=-22;
 if(rx_lanes!=1&&rx_lanes!=2)return r->result=-22;
 if(rx_lanes==2&&(!numbered||frames!=128||route!=DL_ROUTE_DEFAULT))return r->result=-22;
 if(numbered&&(frames!=128||route!=DL_ROUTE_DEFAULT))return r->result=-22;
 if(route!=DL_ROUTE_DEFAULT&&route!=DL_ROUTE_RX_ALL_SDI0)return r->result=-22;
 if(route==DL_ROUTE_RX_ALL_SDI0&&frames!=128)return r->result=-22;
 pathmask=route==DL_ROUTE_RX_ALL_SDI0?0x00fcff03u:0x00fc0303u;
 if(!p||!p->read||!p->write||!p->us||!p->amp_low||!capture||
    frames<8||frames>DL_MAX_FRAMES||capacity<2*frames)return r->result=-22;
 if(!prepared||!common||mclk!=4096000)return r->result=-38;
 r->amp_rc=low(p);if(r->amp_rc){r->held=1;return r->result=r->amp_rc;}
 /* Hardware state has not yet been established: refusal/read error must
  * not grant the caller permission to power down a possibly active SAI. */
 r->held=1;
 if(expect(p,SAI_VERSION,UINT32_MAX,SAI_VER_2307)||
    expect(p,SAI_XFER,UINT32_MAX,0)||expect(p,SAI_STATUS,14,14)||
    expect(p,SAI_DMACR,UINT32_MAX,0)||expect(p,SAI_INTCR,0x30003,0)||
    expect(p,SAI_TXFIFOLR,0xffffff,0)||expect(p,SAI_RXFIFOLR,0xffffff,0)||
    expect(p,SAI_MONO_CR,UINT32_MAX,0))return r->result=-16;
 for(i=0x3c;i<=0x58;i+=4)if(expect(p,i,UINT32_MAX,0))return r->result=-16;
 if(rd(p,SAI_PATH_SEL,&r->path_before))return r->result=-5;
 if(r->path_before&0x30000)return r->result=-38;
 /* Numbered experiment requires the same default RX route as baseline. */
 if(numbered&&(r->path_before&0xff00u)!=0xe400u)return r->result=-38;
 if(rx_lanes==2){
  if(rd(p,SAI_RXCR,&r->rxcr_before))return r->result=-5;
  rx_saved=1;
 }
 path_saved=1;r->held=1;
 mask=SAI_XCR_START_SEL_MASK|SAI_XCR_EDGE_SHIFT_MASK|SAI_XCR_CSR_MASK|
  SAI_XCR_SJM_MASK|SAI_XCR_FBM_MASK|SAI_XCR_SNB_MASK|SAI_XCR_VDJ_MASK|
  SAI_XCR_SBW_MASK|SAI_XCR_VDW_MASK;
 value=SAI_XCR_EDGE_SHIFT_1|SAI_XCR_CSR(1)|SAI_XCR_SNB(2)|
  SAI_XCR_VDJ_L|SAI_XCR_SBW(32)|SAI_XCR_VDW(32);
 rxvalue=(value&~SAI_XCR_CSR_MASK)|SAI_XCR_CSR(rx_lanes);
 if(mod(p,SAI_TXCR,mask,value)||mod(p,SAI_RXCR,mask,rxvalue)||
    mod(p,SAI_TX_SHIFT,SAI_XSHIFT_RIGHT_MASK,2)||
    mod(p,SAI_RX_SHIFT,SAI_XSHIFT_RIGHT_MASK,2)||
    mod(p,SAI_FSCR,0x1ffffff,0x101f03f)||mod(p,SAI_CKR,0x7fff,0x18)||
    mod(p,SAI_PATH_SEL,pathmask,0x40000)||
    mod(p,SAI_INTCR,SAI_INTCR_TXUIC|SAI_INTCR_RXOIC,
        SAI_INTCR_TXUIC|SAI_INTCR_RXOIC))goto done;
 if(expect(p,SAI_TXCR,mask,value)||expect(p,SAI_RXCR,mask,rxvalue)||
    expect(p,SAI_TX_SHIFT,UINT32_MAX,2)||expect(p,SAI_RX_SHIFT,UINT32_MAX,2)||
    expect(p,SAI_FSCR,0x1ffffff,0x101f03f)||expect(p,SAI_CKR,0x7fff,0x18)||
    rd(p,SAI_PATH_SEL,&r->path_active))goto done;
 if(r->path_active!=((r->path_before&~pathmask)|0x40000u)){rc=-71;goto done;}
 if(rx_lanes==2&&rd(p,SAI_RXCR,&r->rxcr_active))goto done;
 if((rc=delay20(p)))goto done;
 rc=-5;clocks_attempted=1;
 if(mod(p,SAI_XFER,3,3)||expect(p,SAI_XFER,15,3))goto done;
 for(i=0;i<16;i++){x=marker(r->tx_words,numbered);if(word(p,1,&x,r))goto done;r->tx_words++;}
 if(rd(p,SAI_TXFIFOLR,&t))goto done;
 r->max_tx=count(t);if(r->max_tx!=16){rc=-71;goto done;}
 if(mod(p,SAI_XFER,12,12)||expect(p,SAI_XFER,15,15))goto done;
 r->start_us=p->us(p->ctx);
 for(r->polls=0;r->polls<frames*4096u;r->polls++){
  now=p->us(p->ctx);
  if(now<r->start_us||now-r->start_us>=(uint64_t)frames*1000000/16000+5000){rc=-110;goto done;}
  if(rd(p,SAI_INTSR,&v))goto done;
  if(v&(SAI_INTSR_TXUI_ACT|SAI_INTSR_RXOI_ACT)){rc=-75;goto done;}
  if(rd(p,SAI_TXFIFOLR,&t)||rd(p,SAI_RXFIFOLR,&v))goto done;
  ready=guarded?rx2_pair_ready(v):(count(v)>=2);
  if(ready<0){rc=-71;goto done;}
  t=count(t);v=count(v);
  if(t>r->max_tx)r->max_tx=t;
  if(v>r->max_rx)r->max_rx=v;
  if(t>16||v>128){rc=-71;goto done;}
  if(t<=14){
   if(r->tx_words>=2*(frames+16u)){rc=-71;goto done;}
   for(i=0;i<2;i++){x=marker(r->tx_words,numbered);if(word(p,1,&x,r))goto done;r->tx_words++;}}
  if(ready){for(i=0;i<2;i++)if(word(p,0,&capture[2*r->frames+i],r))goto done;
   if(++r->frames==frames){rc=0;goto done;}}
 }
 rc=-110;
done:
 r->end_us=p->us(p->ctx);r->amp_rc=low(p);
 if(clocks_attempted)r->stop_rc=stop(p);
 else if(expect(p,SAI_XFER,15,0)||expect(p,SAI_STATUS,14,14))r->stop_rc=-16;
 if(!r->stop_rc&&path_saved){
  r->restore_rc=mod(p,SAI_PATH_SEL,pathmask,r->path_before);
  if(!r->restore_rc)r->restore_rc=rd(p,SAI_PATH_SEL,&r->path_restored);
  if(!r->restore_rc&&(r->path_restored&pathmask)!=(r->path_before&pathmask))r->restore_rc=-71;
 }
 if(!r->stop_rc&&rx_saved){
  r->rx_restore_rc=mod(p,SAI_RXCR,SAI_XCR_CSR_MASK,r->rxcr_before);
  if(!r->rx_restore_rc)r->rx_restore_rc=rd(p,SAI_RXCR,&r->rxcr_restored);
  if(!r->rx_restore_rc&&(r->rxcr_restored&SAI_XCR_CSR_MASK)!=(r->rxcr_before&SAI_XCR_CSR_MASK))r->rx_restore_rc=-71;
 }
 if(!r->stop_rc&&!r->restore_rc&&!r->rx_restore_rc&&!r->amp_rc)r->held=0;
 r->result=rc?rc:(r->stop_rc?r->stop_rc:(r->restore_rc?r->restore_rc:(r->rx_restore_rc?r->rx_restore_rc:r->amp_rc)));
 return r->result;
}

int dl_run(const struct dl_port*p,int prepared,int common,uint32_t mclk,
 uint32_t*capture,unsigned capacity,unsigned frames,struct dl_result*r)
{return run_route(p,prepared,common,mclk,capture,capacity,frames,DL_ROUTE_DEFAULT,0,1,0,r);}
int dl_run_route(const struct dl_port*p,int prepared,int common,uint32_t mclk,
 uint32_t*capture,unsigned capacity,unsigned frames,int route,struct dl_result*r)
{return run_route(p,prepared,common,mclk,capture,capacity,frames,route,0,1,0,r);}

int dl_run_numbered(const struct dl_port*p,int prepared,int common,uint32_t mclk,
 uint32_t*capture,unsigned capacity,struct dl_result*r)
{return run_route(p,prepared,common,mclk,capture,capacity,128,DL_ROUTE_DEFAULT,1,1,0,r);}

int dl_run_numbered_rx2(const struct dl_port*p,int prepared,int common,uint32_t mclk,
 uint32_t*capture,unsigned capacity,struct dl_result*r)
{return run_route(p,prepared,common,mclk,capture,capacity,128,DL_ROUTE_DEFAULT,1,2,0,r);}

int dl_run_numbered_rx2_guard(const struct dl_port*p,int prepared,int common,uint32_t mclk,
 uint32_t*capture,unsigned capacity,struct dl_result*r)
{return run_route(p,prepared,common,mclk,capture,capacity,128,DL_ROUTE_DEFAULT,1,2,1,r);}
