/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pio.h"
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#include "input/rockchip_sai.h"
/* Vendor rockchip_sai_get_fifo_count sums all four fields irrespective of
 * configured serial lane count. Do not equate CSR with FIFO bank layout.
 * Reserved high bits are not part of occupancy. */
unsigned pio_fifo_count(uint32_t v)
{
  return ((v&SAI_FIFOLR_XFL0_MASK)>>SAI_FIFOLR_XFL0_SHIFT)+
    ((v&SAI_FIFOLR_XFL1_MASK)>>SAI_FIFOLR_XFL1_SHIFT)+
    ((v&SAI_FIFOLR_XFL2_MASK)>>SAI_FIFOLR_XFL2_SHIFT)+
    ((v&SAI_FIFOLR_XFL3_MASK)>>SAI_FIFOLR_XFL3_SHIFT);
}
static int fifo_each_at_least(uint32_t v,unsigned level)
{
  return ((v&SAI_FIFOLR_XFL0_MASK)>>SAI_FIFOLR_XFL0_SHIFT)>=level &&
    ((v&SAI_FIFOLR_XFL1_MASK)>>SAI_FIFOLR_XFL1_SHIFT)>=level &&
    ((v&SAI_FIFOLR_XFL2_MASK)>>SAI_FIFOLR_XFL2_SHIFT)>=level &&
    ((v&SAI_FIFOLR_XFL3_MASK)>>SAI_FIFOLR_XFL3_SHIFT)>=level;
}
int pio_summarize(const uint32_t *data,unsigned frames,struct pio_stats*out)
{
  unsigned i,ch;int64_t v;
  if(!data||!out||!frames||frames>PIO_FRAMES)return -22;
  memset(out,0,sizeof *out);
  for(i=0;i<frames;i++)for(ch=0;ch<2;ch++) {
    uint32_t w=data[2*i+ch];
    v=w<=INT32_MAX?(int64_t)w:-1-(int64_t)(UINT32_MAX-w);
    if(!i||v<out->min[ch])out->min[ch]=(int32_t)v;
    if(!i||v>out->max[ch])out->max[ch]=(int32_t)v;
    out->sum[ch]+=v;if(w)out->nonzero[ch]++;
  }
  return 0;
}
static int rd(const struct pio_port*p,uint32_t o,uint32_t*v){return p->read(p->ctx,o,v)?-5:0;}
static int wr(const struct pio_port*p,uint32_t o,uint32_t v){return p->write(p->ctx,o,v)?-5:0;}
static void trace_begin(const struct pio_port*p,struct pio_rx_trace*t,uint64_t start)
{
 static const uint32_t regs[6]={SAI_MONO_CR,SAI_RX_SHIFT,SAI_RXCR,SAI_PATH_SEL,SAI_DMACR,SAI_XFER};
 unsigned i;t->start_us=start;t->window_started=1;
 for(i=0;i<6;i++)t->init_rc[i]=rd(p,regs[i],&t->init[i]);
}
static int rx_word(const struct pio_port*p,uint32_t*destination,struct pio_rx_trace*t)
{
 struct pio_rx_trace_entry*e;int rc;
 if(t->count==PIO_RX_TRACE_MAX)return rd(p,SAI_RXDR,destination);
 e=&t->entry[t->count++];
 e->time_us=p->us(p->ctx);
 e->before_rc=rd(p,SAI_RXFIFOLR,&e->fifo_before);
 rc=rd(p,SAI_RXDR,destination);e->word_rc=rc;
 if(!rc)e->word=*destination;
 e->after_rc=rd(p,SAI_RXFIFOLR,&e->fifo_after);
 /* Extra diagnostic errors invalidate that observation only; original
  * RXDR error still aborts. Never substitute or discard a captured zero. */
 return rc;
}
static int mod(const struct pio_port*p,uint32_t o,uint32_t mask,uint32_t val)
{uint32_t v;if(rd(p,o,&v))return -5;return wr(p,o,(v&~mask)|(val&mask));}
/* No silent reset on idle/clear timeout. 2307 uses STATUS bits3/2/1. */
static int stop(const struct pio_port*p,int tx)
{
  uint32_t v,bit=tx?SAI_XFER_TXS_MASK:SAI_XFER_RXS_MASK;
  uint32_t idle=tx?SAI_STATUS_TX_IDLE:SAI_STATUS_RX_IDLE;
  uint32_t clr=tx?SAI_CLR_TXC:SAI_CLR_RXC;
  uint64_t start=p->us(p->ctx),now;
  unsigned i;
  if(mod(p,SAI_XFER,bit,0))return -5;
  for(i=0;i<10000;i++) {
    if(rd(p,SAI_STATUS,&v))return -5;
    now=p->us(p->ctx);if(now<start || now-start>=1000)return -110;
    if(v&idle)break;
  }
  if(i==10000)return -110;
  if(mod(p,SAI_CLR,clr,clr))return -5;
  for(i=0;i<10000;i++) {
    if(rd(p,SAI_CLR,&v))return -5;
    now=p->us(p->ctx);if(now<start || now-start>=1000)return -110;
    if(!(v&clr))break;
  }
  if(i==10000)return -110;
  if(mod(p,SAI_XFER,SAI_XFER_CLK_MASK|SAI_XFER_FSS_MASK,0))return -5;
  for(i=0;i<10000;i++) {
    if(rd(p,SAI_STATUS,&v))return -5;
    now=p->us(p->ctx);if(now<start || now-start>=2000)return -110;
    if(v&SAI_STATUS_FS_IDLE)break;
  }
  if(i==10000)return -110;
  /* Keep external mclk running. Caller must allow >=2 BCLK before gating. */
  return 0;
}
/* Explicit opt-in vendor DIV-to-clock settling interval; no MMIO here. */
static int clockwait20(const struct pio_port*p)
{
 uint64_t start=p->us(p->ctx),prev=start,now;unsigned i;
 for(i=0;i<100000;i++){
  now=p->us(p->ctx);
  if(now<prev)return -5;
  if(now-start>=20)return 0;
  prev=now;
 }
 return -110;
}
static uint64_t transfer_budget_us(unsigned frames,int tx,int buffered)
{
 uint64_t nominal=(uint64_t)frames*1000000u/16000u,slack=100000u;
 /* Buffered speech may accumulate scheduling gaps during concurrent vision.
  * Allow 10%, bounded to 100ms..1s; capture and diagnostic tones are unchanged.
  * This is a total wall-clock cap, not a retry or proof of acoustic playback. */
 if(tx && buffered) {
  slack=nominal/10;
  if(slack<100000u)slack=100000u;
  if(slack>1000000u)slack=1000000u;
 }
 return nominal+slack;
}
static int run(const struct pio_port*p,int prepared,uint32_t mclk,int tx,
            uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out,
            const uint32_t *source,const int16_t *source16,unsigned source_words,
            int buffered,int mono16,int clockwait,int pause250,
             unsigned bits,int dma_request,unsigned rx_lanes,int route_all,int mono,
             int grouped,pio_sample_sink sink,void *sink_arg,
             pio_sample_source pull,void *pull_arg)
{
  uint32_t v,fifo_raw=0,div,word,mask,value,bclk,old_dmacr=0,old_mono=0;
  uint32_t grouped_first=0;
  uint64_t start,now,last_progress=0,budget;
  unsigned progress_frames=0,progress_fifo=0;
  int rc=-5,grouped_half=0,source_eof=0;
  if(!out)return -22;
  memset(out,0,sizeof *out);
  if(!p || !p->read || !p->write || !p->us || !p->clocks_started ||
      !p->streams_started || !p->amp_off_fast ||
      (tx && !buffered && (!p->amplitude || p->amplitude>PIO_MAX_AMPLITUDE)) ||
      (tx!=0 && tx!=1) || (bits!=16 && bits!=32) || (tx && bits!=32) ||
      (dma_request && tx) || (rx_lanes!=1 && rx_lanes!=4) ||
      (tx && (rx_lanes!=1 || route_all || mono || grouped)) ||
      (route_all && rx_lanes!=4) || (mono && (bits!=32 || dma_request ||
       route_all || rx_lanes!=1 || pause250 || grouped)) ||
      (grouped && (bits!=32 || dma_request || !route_all || rx_lanes!=4 ||
       pause250 || mono)) ||
      !frames || frames>(tx && buffered ? PIO_PLAYBACK_FRAMES : sink ? PIO_STREAM_FRAMES : PIO_FRAMES) || (tx&&!buffered&&(frames<8||frames>3200)) ||
      (pull && (!tx || !buffered || !mono16)) ||
      (buffered&&!pull&&((mono16&&(!source16||source_words<frames)) ||
                  (!mono16&&(!source||source_words<2*frames)))) ||
      (sink && (tx || !grouped)) ||
      (!tx && (!capture || capacity<(sink ? 2u : 2*frames))))return out->result=-22;
  if(!prepared)return out->result=-38;
  if(tx && p->cancelled && p->cancelled(p->cancel_arg))
    return out->result=-ECANCELED;
  bclk=32000u*bits;
  if(!mclk || mclk%bclk || !(div=mclk/bclk) || div>4096)return out->result=-22;
  if(rd(p,SAI_VERSION,&v))return out->result=-5;
  if(v!=SAI_VER_2307)return out->result=-38;
  if(rd(p,SAI_XFER,&v))return out->result=-5;
  if(v&0xf)return out->result=-16;
  if(rd(p,SAI_DMACR,&old_dmacr))return out->result=-5;
  if(old_dmacr&(SAI_DMACR_RDE_MASK|SAI_DMACR_TDE_MASK))return out->result=-16;
  if(rd(p,SAI_MONO_CR,&old_mono))return out->result=-5;
  if(rd(p,SAI_STATUS,&v))return out->result=-5;
  if((v&14)!=14)return out->result=-16;
  /* Refuse residue instead of attributing old FIFO data to this capture. */
  if(rd(p,tx?SAI_TXFIFOLR:SAI_RXFIFOLR,&v))return out->result=-5;
  if(pio_fifo_count(v))return out->result=-16;
  out->held=1;
  mask=SAI_XCR_START_SEL_MASK|SAI_XCR_EDGE_SHIFT_MASK|SAI_XCR_CSR_MASK|
    SAI_XCR_SJM_MASK|SAI_XCR_FBM_MASK|SAI_XCR_SNB_MASK|SAI_XCR_VDJ_MASK|
    SAI_XCR_SBW_MASK|SAI_XCR_VDW_MASK;
  /* Vendor CSR macro subtracts one without parenthesizing its argument. */
  value=SAI_XCR_EDGE_SHIFT_1|SAI_XCR_CSR((tx?1:rx_lanes))|SAI_XCR_SNB(2)|
    SAI_XCR_VDJ_L|SAI_XCR_SBW(bits)|SAI_XCR_VDW(bits);
  if(mod(p,tx?SAI_TXCR:SAI_RXCR,mask,value))goto done;
  if(mod(p,tx?SAI_TX_SHIFT:SAI_RX_SHIFT,SAI_XSHIFT_RIGHT_MASK,2))goto done;
  if(mono && mod(p,SAI_MONO_CR,
       SAI_MCR_RX_MONO_SLOT_MASK|SAI_MCR_RX_MONO_MASK,
       SAI_MCR_RX_MONO_SLOT_SEL(1)|SAI_MCR_RX_MONO_EN))goto done;
  if(mod(p,SAI_FSCR,SAI_FSCR_EDGE_MASK|SAI_FSCR_FW_MASK|SAI_FSCR_FPW_MASK,
       SAI_FSCR_EDGE_DUAL|SAI_FSCR_FW(2*bits)|SAI_FSCR_FPW(bits)))goto done;
  if(mod(p,SAI_CKR,SAI_CKR_MDIV_MASK|SAI_CKR_MSS_MASK|SAI_CKR_CKP_MASK|SAI_CKR_FSP_MASK,
       SAI_CKR_MDIV(div)))goto done;
  /* Default lane0 -> data0, explicit source header path bit positions. */
  if(mod(p,SAI_PATH_SEL,tx?3u:(route_all?(0xffu<<8):(3u<<8)),0))goto done;
  if(mod(p,SAI_INTCR,SAI_INTCR_TXUIE_MASK|SAI_INTCR_RXOIE_MASK,0))goto done;
  if(mod(p,SAI_INTCR,SAI_INTCR_TXUIC|SAI_INTCR_RXOIC,
         tx?SAI_INTCR_TXUIC:SAI_INTCR_RXOIC))goto done;
  if(dma_request && mod(p,SAI_DMACR,
       SAI_DMACR_RDE_MASK|SAI_DMACR_RDL_MASK,
       SAI_DMACR_RDE(1)|SAI_DMACR_RDL(16)))goto done;
  if(clockwait){int e=clockwait20(p);if(e){rc=e;goto done;}}
  if(mod(p,SAI_XFER,SAI_XFER_CLK_MASK|SAI_XFER_FSS_MASK,
         SAI_XFER_CLK_EN|SAI_XFER_FSS_EN))goto done;
  start=p->us(p->ctx);
  out->activation_result=p->clocks_started(p->ctx);
  now=p->us(p->ctx);
  if(out->activation_result){rc=-5;goto done;}
  if(now<start||now-start>=100000){rc=-110;goto done;}
  if(tx) {
    unsigned i,pre=frames<8?2*frames:16;
    int16_t pulled=0;
    for(i=0;i<pre;i++){
      if(pull && !(i&1u)){
        if(p->cancelled && p->cancelled(p->cancel_arg)){rc=-ECANCELED;goto done;}
        int got=pull(pull_arg,&pulled);
        if(got<0){rc=got;goto done;}
        if(got>1){rc=-EPROTO;goto done;}
        if(!got){source_eof=1;pre=i;break;}
      }
      uint32_t sample=pull?(uint32_t)((int32_t)pulled*65536):buffered&&mono16?
        (uint32_t)((int32_t)source16[i/2]*65536):
        (buffered?source[i]:p->amplitude);
      if(wr(p,SAI_TXDR,sample))goto done;
      out->prefill_words++;
    }
    if(pull&&!pre){rc=-ENODATA;goto done;}
    if(rd(p,SAI_TXFIFOLR,&v))goto done;
    out->first_fifo_raw=out->last_fifo_raw=v;
    out->max_fifo=pio_fifo_count(v);
    if(out->max_fifo!=pre){rc=-71;goto done;}
    out->frames=pre/2;
  }
  if(mod(p,SAI_XFER,tx?SAI_XFER_TXS_MASK:SAI_XFER_RXS_MASK,
         tx?SAI_XFER_TXS_EN:SAI_XFER_RXS_EN))goto done;
  start=p->us(p->ctx);
  out->start_result=p->streams_started(p->ctx);
  now=p->us(p->ctx);
  if(out->start_result){rc=-5;goto done;}
  if(now<start||now-start>=100){rc=-110;goto done;}
  start=now;
  budget=transfer_budget_us(frames,tx,buffered);
  last_progress=start;progress_frames=out->frames;
  progress_fifo=out->max_fifo;
  out->stream_start_us=start;
  if(!tx)trace_begin(p,&out->rx_trace,start);
  for(out->polls=0;out->polls<frames*4096u;out->polls++) {
    if(tx && p->cancelled && p->cancelled(p->cancel_arg))
      {rc=-ECANCELED;goto done;}
    now=p->us(p->ctx);
    if(now<start || now-start>=budget)
      {rc=-110;goto done;}
    if(rd(p,SAI_INTSR,&v))goto done;
    if(v&(tx?SAI_INTSR_TXUI_ACT:SAI_INTSR_RXOI_ACT)){rc=-75;goto done;}
    if(rd(p,tx?SAI_TXFIFOLR:SAI_RXFIFOLR,&v))goto done;
    fifo_raw=v;
    if(!tx&&!out->polls)out->first_fifo_raw=v;
    out->last_fifo_raw=v;
    if(pause250 && out->frames==8 && !out->pause_end_us) {
      unsigned n;
      out->pause_before=v;out->pause_start_us=p->us(p->ctx);
      for(n=0;n<100000;n++) {
        now=p->us(p->ctx);
        if(now<out->pause_start_us){rc=-5;goto done;}
        if(rd(p,SAI_INTSR,&v))goto done;
        if(v&SAI_INTSR_RXOI_ACT){rc=-75;goto done;}
        if(rd(p,SAI_RXFIFOLR,&out->pause_after))goto done;
        if(now-out->pause_start_us>=250)break;
      }
      if(n==100000){rc=-110;goto done;}
      out->pause_end_us=p->us(p->ctx);
      if(out->pause_end_us<out->pause_start_us ||
         out->pause_end_us-out->pause_start_us>=1000){rc=-110;goto done;}
      /* Retain the first reads AFTER the pause in the existing fixed trace. */
      out->rx_trace.count=0;
      continue;
    }
    v=pio_fifo_count(v);
    if(v>out->max_fifo)out->max_fifo=v;
    if(tx&&v>16){rc=-71;goto done;}
    if(tx && buffered) {
      /* Enlarging a long prompt's total budget must not hide a stuck FIFO.
       * Enqueue progress and final FIFO drain both reset this bounded guard. */
      if(out->frames!=progress_frames || v<progress_fifo)last_progress=now;
      if(now<last_progress || now-last_progress>=100000u){rc=-110;goto done;}
      progress_frames=out->frames;progress_fifo=v;
    }
    if(pull && out->frames==frames && !source_eof){
      int16_t extra;int got=pull(pull_arg,&extra);
      if(got){rc=got<0?got:-EFBIG;goto done;}source_eof=1;
    }
    if(out->frames==frames || source_eof) { if(!tx || !v){rc=0;goto done;}continue; }
    if(tx && v<=14) {
      if(pull){
        int16_t sample;int got=pull(pull_arg,&sample);
        if(got<0){rc=got;goto done;}
        if(got>1){rc=-EPROTO;goto done;}
        if(!got){source_eof=1;continue;}
        uint32_t stream_word=(uint32_t)((int32_t)sample*65536);
        if(wr(p,SAI_TXDR,stream_word)||wr(p,SAI_TXDR,stream_word))goto done;
      }else if(buffered) {
        uint32_t left=mono16?
          (uint32_t)((int32_t)source16[out->frames]*65536):
          source[2*out->frames];
        uint32_t right=mono16?left:source[2*out->frames+1];
        if(wr(p,SAI_TXDR,left)||wr(p,SAI_TXDR,right))goto done;
      } else {
        word=(out->frames/32)%2 ? 0u-p->amplitude : p->amplitude;
        if(wr(p,SAI_TXDR,word)||wr(p,SAI_TXDR,word))goto done;
      }
      out->frames++;
    } else if(!tx && ((grouped && fifo_each_at_least(fifo_raw,1)) ||
                      (!grouped && v>=(mono?1u:2u)))) {
      /* A streaming sink consumes each frame immediately; retained debug
       * storage wraps and is not a complete recording beyond its capacity. */
      unsigned capture_at=sink ? 2*(out->frames%(capacity/2)) : 2*out->frames;
      if(grouped) {
        uint32_t words[8];unsigned n;
        for(n=0;n<8;n++)if(rx_word(p,&words[n],&out->rx_trace))goto done;
        out->grouped_words+=8;
        for(n=2;n<8;n+=2)if(words[n]!=words[0])out->grouped_mismatches++;
        if(!grouped_half) {
          grouped_first=words[0];
          grouped_half=1;
          continue;
        }
        capture[capture_at]=grouped_first;
        capture[capture_at+1]=words[0];
        grouped_half=0;
      } else {
       if(rx_word(p,&capture[capture_at],&out->rx_trace))goto done;
       if(mono)
        capture[capture_at+1]=capture[capture_at];
       else if(rx_word(p,&capture[capture_at+1],&out->rx_trace))goto done;
      }
      if(sink) {
        int sink_rc;
        if(out->grouped_mismatches){rc=-5;goto done;}
        sink_rc=sink(sink_arg,capture[capture_at],capture[capture_at+1]);
        if(sink_rc>0){out->frames++;rc=0;goto done;}
        if(sink_rc<0){rc=sink_rc;goto done;}
      }
      out->frames++;
    }
  }
  rc=-110;
done:
  if(out->rx_trace.window_started)out->rx_trace.end_us=p->us(p->ctx);
  if(dma_request)
    out->dma_restore_result=mod(p,SAI_DMACR,
      SAI_DMACR_RDE_MASK|SAI_DMACR_RDL_MASK,old_dmacr);
  {
    uint64_t before=p->us(p->ctx),after;
    out->amp_off_result=p->amp_off_fast(p->ctx);
    after=p->us(p->ctx);
    if(after<before||after-before>=100)out->amp_off_result=-110;
  }
  out->stop_result=stop(p,tx);
  if(mono && !out->stop_result)
    out->mono_restore_result=mod(p,SAI_MONO_CR,
      SAI_MCR_RX_MONO_SLOT_MASK|SAI_MCR_RX_MONO_MASK,old_mono);
  if(!out->stop_result&&!out->amp_off_result&&!out->dma_restore_result&&
     !out->mono_restore_result)out->held=0;
  out->result=rc?rc:(out->dma_restore_result?out->dma_restore_result:
    (out->amp_off_result?out->amp_off_result:(out->stop_result?out->stop_result:
     out->mono_restore_result)));
  return out->result;
}
int pio_run(const struct pio_port*p,int prepared,uint32_t mclk,int tx,
            uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,tx,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,32,0,1,0,0,0,NULL,NULL,NULL,NULL);}
int pio_run16(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,0,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,16,0,1,0,0,0,NULL,NULL,NULL,NULL);}
int pio_run_rde(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,0,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,32,1,1,0,0,0,NULL,NULL,NULL,NULL);}
int pio_run_rx4(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,0,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,32,0,4,0,0,0,NULL,NULL,NULL,NULL);}
int pio_run_rx4_all(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,0,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,32,0,4,1,0,0,NULL,NULL,NULL,NULL);}
int pio_run_mono(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,0,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,32,0,1,0,1,0,NULL,NULL,NULL,NULL);}
int pio_run_grouped(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,0,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,32,0,4,1,0,1,NULL,NULL,NULL,NULL);}
int pio_play_buffer(const struct pio_port*p,int prepared,uint32_t mclk,
                    const uint32_t *source,unsigned source_words,unsigned frames,
                    struct pio_result*out)
{return run(p,prepared,mclk,1,NULL,0,frames,out,source,NULL,source_words,1,0,0,0,32,0,1,0,0,0,NULL,NULL,NULL,NULL);}

int pio_play_mono16(const struct pio_port*p,int prepared,uint32_t mclk,
                    const int16_t *source,unsigned source_frames,unsigned frames,
                    struct pio_result*out)
{return run(p,prepared,mclk,1,NULL,0,frames,out,NULL,source,source_frames,1,1,0,0,32,0,1,0,0,0,NULL,NULL,NULL,NULL);}

int pio_run_clockwait(const struct pio_port*p,int prepared,uint32_t mclk,int tx,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out)
{return run(p,prepared,mclk,tx,capture,capacity,frames,out,NULL,NULL,0,0,0,1,0,32,0,1,0,0,0,NULL,NULL,NULL,NULL);}
int pio_run_pause250(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,struct pio_result*out)
{return run(p,prepared,mclk,0,capture,capacity,128,out,NULL,NULL,0,0,0,0,1,32,0,1,0,0,0,NULL,NULL,NULL,NULL);}

int pio_run_grouped_sink(const struct pio_port*p,int prepared,uint32_t mclk,
 uint32_t *capture,unsigned capacity,unsigned frames,struct pio_result*out,
 pio_sample_sink sink,void *arg)
{
 if(!sink)return -22;
 return run(p,prepared,mclk,0,capture,capacity,frames,out,NULL,NULL,0,0,0,0,0,32,0,4,1,0,1,sink,arg,NULL,NULL);
}

int pio_play_stream16(const struct pio_port*p,int prepared,uint32_t mclk,
                     pio_sample_source pull,void *arg,struct pio_result*out)
{
 if(!pull)return -EINVAL;
 return run(p,prepared,mclk,1,NULL,0,PIO_PLAYBACK_FRAMES,out,NULL,NULL,0,
            1,1,0,0,32,0,1,0,0,0,NULL,NULL,pull,arg);
}
