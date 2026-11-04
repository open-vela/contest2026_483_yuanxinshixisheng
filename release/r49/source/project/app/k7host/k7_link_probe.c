/* SPDX-License-Identifier: Apache-2.0 */
/* Experimental EP0 and bounded synthetic Bulk IN. No video or motor output.
 * Register facts: pinned Linux DWC3 core.h/gadget.h/ep0.c.
 * VID/PID 1209:0001 is private test only, not unique, not for distribution:
 * https://pid.codes/1209/0001/ . Do not ship this test descriptor.
 */
#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "dwc3_transport_core.h"
#include "k7_video_queue.h"
#define BASE 0x23000000ul
#define ALIGN __attribute__((aligned(64)))
static uint32_t events[256] ALIGN;
static uint32_t trb[2][16] ALIGN;
static uint8_t data[2][128] ALIGN;
static atomic_bool attempted;
static unsigned int phase,three_stage,configured,resets,connects,setups,position;
static int fault;
static unsigned int failed_ep,failed_command;
static uint32_t failed_register;
static uint8_t setup_trace[32][8];
static bool setup_armed;
static unsigned int reset_phase[16],connect_speed[16];
static uint8_t bulk_data[16384] ALIGN;
static uint32_t bulk_trb[16] ALIGN;
static unsigned int bulk_count;
static unsigned int bulk_notready,bulk_last_event;
static bool bulk_active,bulk_enabled;
static bool video_mode;
static int video_slot=-1;
static const uint8_t *video_wire;
static size_t video_size,video_offset;
static unsigned int video_frames;
static uint32_t rd(void *p,uint32_t off){(void)p;return *(volatile uint32_t *)(BASE+off);}
static void wr(void *p,uint32_t off,uint32_t v){(void)p;*(volatile uint32_t *)(BASE+off)=v;}
static uint64_t now(void *p)
{(void)p;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static void delay(void *p,unsigned int us)
{(void)p;struct timespec t={us/1000000,(long)(us%1000000)*1000};nanosleep(&t,NULL);}
static const struct k7_dwc3_bus bus={NULL,rd,wr,now,delay};
static void barrier(void){__asm__ volatile("dsb sy" ::: "memory");}
static void clean(void *p,size_t n){up_clean_dcache((uintptr_t)p,(uintptr_t)p+n);barrier();}
static void invalidate(void *p,size_t n){up_invalidate_dcache((uintptr_t)p,(uintptr_t)p+n);barrier();}
static void maskwrite(uintptr_t address,uint32_t mask,uint32_t value)
{*(volatile uint32_t *)address=(mask<<16)|(value&mask);barrier();}
static int usb2_prepare(void)
{
 /* PHY0 analog power is already on in the measured boot state. Do not
  * invent a power/reset sequence when that prerequisite is absent. */
 if(*(volatile uint32_t *)0x2602e010ul&(1u<<13))return -EHOSTDOWN;
 wr(NULL,0xc110,rd(NULL,0xc110)|(1u<<11));
 wr(NULL,0xc200,rd(NULL,0xc200)|(1u<<31));
 wr(NULL,0xc2c0,rd(NULL,0xc2c0)|(1u<<31));
 /* RK3576 PHY0 USB2-only PIPE status, separate from camera's PHY1. */
 maskwrite(0x2601e030ul,0xffff,0x0189);
 maskwrite(0x2602e008ul,1,0); /* Enable 480 MHz output. */
 maskwrite(0x2602e000ul,0x01ff,0); /* Exit PHY suspend. */
 delay(NULL,2000);
 /* DT phy_type=utmi_wide, no-freeclk and no-SLPM quirks. */
 uint32_t cfg=rd(NULL,0xc200);
 cfg&=~((1u<<30)|(1u<<8)|(1u<<6)|(1u<<4)|(15u<<10));
 cfg|=(1u<<3)|(5u<<10);wr(NULL,0xc200,cfg);
 delay(NULL,100000);
 wr(NULL,0xc200,rd(NULL,0xc200)&~(1u<<31));
 wr(NULL,0xc2c0,rd(NULL,0xc2c0)&~((1u<<31)|(1u<<17)));
 delay(NULL,100000);
 wr(NULL,0xc110,rd(NULL,0xc110)&~(1u<<11));
 delay(NULL,10000);
 return 0;
}
static int command(unsigned int ep,unsigned int cmd,uint32_t a,uint32_t b,uint32_t c)
{
 uint32_t args[]={a,b,c},done;
 int ret=k7_dwc3_ep_command(&bus,ep,cmd,0,args,&done);
 if(ret && !fault)
  {fault=ret;failed_ep=ep;failed_command=cmd;failed_register=rd(NULL,0xc80c+16u*ep);}
 return ret;
}
static int waitbit(uint32_t off,uint32_t mask,uint32_t value)
{
 uint64_t start=now(NULL);
 for(unsigned int i=0;i<1000;i++)
  {if((rd(NULL,off)&mask)==value)return 0;if(now(NULL)-start>100000)break;delay(NULL,100);}
 return -ETIMEDOUT;
}
static int transfer(unsigned int ep,unsigned int kind,unsigned int bytes)
{
 if(ep>1 || bytes>sizeof(data[ep]))return -EINVAL;
 memset(trb[ep],0,sizeof(trb[ep]));
 trb[ep][0]=(uint32_t)(uintptr_t)data[ep];trb[ep][1]=(uintptr_t)data[ep]>>32;
 trb[ep][2]=bytes;trb[ep][3]=1u|2u|(kind<<4)|(1u<<10)|(1u<<11);
 clean(data[ep],sizeof(data[ep]));clean(trb[ep],sizeof(trb[ep]));
 return command(ep,6,(uintptr_t)trb[ep]>>32,(uint32_t)(uintptr_t)trb[ep],0);
}
static void setup_next(void)
{
 if(setup_armed)return;
 phase=0;three_stage=0;
 if(!transfer(0,2,8))setup_armed=true;
}
static const uint8_t device_desc[]={18,1,0,2,0,0,0,64,9,0x12,1,0,0,1,1,2,0,1};
static const uint8_t config_desc[]={9,2,25,0,1,1,0,0x80,50,9,4,0,0,1,0xff,0,0,0,
                                  7,5,0x81,2,0,2,0};
static void bulk_next(void)
{
 if(fault || bulk_active || !bulk_enabled || (!video_mode && bulk_count>=1024))return;
 if(video_mode)
  {
   if(video_slot<0)
    {
     video_slot=k7_video_queue_take(&video_wire,&video_size);
     video_offset=0;
     if(video_slot<0)return;
    }
   memcpy(bulk_data,video_wire+video_offset,sizeof(bulk_data));
  }
 else
  {
   memcpy(bulk_data,"K7B1",4);
   for(unsigned int i=0;i<4;i++)bulk_data[4+i]=(bulk_count>>(8*i))&255;
   for(unsigned int i=8;i<sizeof(bulk_data);i++)bulk_data[i]=(i+bulk_count)&255;
  }
 memset(bulk_trb,0,sizeof(bulk_trb));
 bulk_trb[0]=(uint32_t)(uintptr_t)bulk_data;
 bulk_trb[1]=(uintptr_t)bulk_data>>32;
 bulk_trb[2]=sizeof(bulk_data);
 bulk_trb[3]=1u|2u|(1u<<4)|(1u<<11);
 clean(bulk_data,sizeof(bulk_data));clean(bulk_trb,sizeof(bulk_trb));
 if(!command(3,6,(uintptr_t)bulk_trb>>32,(uint32_t)(uintptr_t)bulk_trb,0))
  bulk_active=true;
}
static int bulk_enable(void)
{
 /* This first payload experiment is HS-only and supports one configuration
  * lifetime per boot. Static DMA buffers are retained after a bus reset. */
 if((rd(NULL,0xc70c)&7u)!=0)return -ENOTSUP;
 if(bulk_enabled)return 0;
 if(command(3,1,(2u<<1)|(512u<<3)|(1u<<17),
            (3u<<25)|(1u<<8)|(1u<<10),0))return fault;
 wr(NULL,0xc720,rd(NULL,0xc720)|(1u<<3));
 bulk_enabled=true;
 if(video_mode)k7_video_queue_open();
 return 0;
}
static unsigned int word(const uint8_t *p){return p[0]|((unsigned int)p[1]<<8);}
static void setup_handle(void)
{
 invalidate(data[0],sizeof(data[0]));
 if(setups<32)memcpy(setup_trace[setups],data[0],8);
 setups++;
 const uint8_t *s=data[0];unsigned int val=word(s+2),index=word(s+4),len=word(s+6);
 const uint8_t *reply=NULL;size_t size=0;
 if(s[0]==0xc0 && s[1]==0x5a && val==0 && index==0)
  {
   invalidate(bulk_trb,sizeof(bulk_trb));
   uint32_t values[]={0x3144374b,phase,configured,bulk_enabled,bulk_active,
                     bulk_count,bulk_trb[2],bulk_trb[3],rd(NULL,0xc83c),
                     rd(NULL,0xc720),rd(NULL,0xc700),rd(NULL,0xc70c),
                     rd(NULL,0xc304),rd(NULL,0xc15c),resets,connects,
                     bulk_notready,bulk_last_event,rd(NULL,0xc838),
                     rd(NULL,0xc834),(uint32_t)(uintptr_t)bulk_trb,
                     (uint32_t)(uintptr_t)bulk_data,bulk_trb[0],bulk_trb[1]};
   for(size_t i=0;i<sizeof(values)/sizeof(values[0]);i++)
    for(unsigned int j=0;j<4;j++)data[1][4*i+j]=(values[i]>>(8*j))&255;
   reply=data[1];size=sizeof(values);
  }
 else if(s[0]==0x80 && s[1]==6)
  {
   if(val==0x100 && index==0){reply=device_desc;size=sizeof(device_desc);}
   else if(val==0x200 && index==0){reply=config_desc;size=sizeof(config_desc);}
   else if((val>>8)==3)
    {
     if((val&255)==0){data[1][0]=4;data[1][1]=3;data[1][2]=9;data[1][3]=4;size=4;}
     else if(((val&255)==1 || (val&255)==2) && (index==0x409 || index==0))
      {
       const char *text=(val&255)==1?"K7 development":"openvela link probe";
       size=2+2*strlen(text);data[1][0]=size;data[1][1]=3;
       for(size_t i=0;i<strlen(text);i++){data[1][2+2*i]=text[i];data[1][3+2*i]=0;}
      }
     reply=data[1];
    }
  }
 else if(s[0]==0x80 && s[1]==8 && val==0 && index==0 && len==1)
  {data[1][0]=configured;reply=data[1];size=1;}
 else if(s[0]==0x80 && s[1]==0 && val==0 && index==0 && len==2)
  {data[1][0]=0;data[1][1]=0;reply=data[1];size=2;}
 else if(s[0]==0 && s[1]==5 && val<128 && index==0 && len==0)
  {wr(NULL,0xc700,(rd(NULL,0xc700)&~(127u<<3))|(val<<3));phase=3;return;}
 else if(s[0]==0 && s[1]==9 && val<=1 && index==0 && len==0)
  {
   if(val==0 && bulk_enabled){fault=-ENOTSUP;return;}
   if(val==1){int ret=bulk_enable();if(ret){fault=ret;return;}}
   configured=val;phase=3;return;
  }
 if(reply && size && len)
  {
   if(size>len)size=len;
   if(reply!=data[1])memcpy(data[1],reply,size);
   phase=1;three_stage=1;transfer(1,5,size);return;
  }
 command(0,4,0,0,0);setup_next();
}
static void event(uint32_t v)
{
 if(v&1)
  {
   if(((v>>1)&127)!=0)return;
   unsigned int type=(v>>8)&15;
   if(type==1)
    {
     if(resets<16)reset_phase[resets]=phase;
     resets++;configured=0;
     if(bulk_enabled){fault=-ENOTSUP;return;}
     /* A pending SETUP remains armed across reset. Reissuing STARTTRANSFER
      * for it fails with an active resource. Non-SETUP recovery also needs
      * ENDTRANSFER resource/completion handling; stop this diagnostic
      * instead of overwriting live DMA state before that is implemented. */
     if(phase!=0)
      {
       fault=-ENOTSUP;
       return;
      }
     wr(NULL,0xc700,rd(NULL,0xc700)&~(127u<<3));
    }
   else if(type==2)
    {
     if(connects<16)connect_speed[connects]=rd(NULL,0xc70c)&7u;
     connects++;setup_next();
    }
   else if(type==0){configured=0;fault=-ENOTCONN;}
   return;
  }
 unsigned int ep=(v>>1)&31,type=(v>>6)&15,status=(v>>12)&15;
 if(ep==3)
  {
   bulk_last_event=v;
   if(type==3)bulk_notready++;
   if(type==1 && bulk_active)
    {
     invalidate(bulk_trb,sizeof(bulk_trb));
     bulk_active=false;
     if((status&1u) || (bulk_trb[2]&0x00ffffffu)){fault=-EIO;return;}
     bulk_count++;
     if(video_mode)
      {
       video_offset+=sizeof(bulk_data);
       if(video_offset==video_size)
        {k7_video_queue_release(video_slot);video_slot=-1;video_frames++;}
      }
     bulk_next();
    }
   return;
  }
 if(ep>1)return;
 if(type==1)
  {
   invalidate(trb[ep],sizeof(trb[ep]));
   if(phase==0 && ep==0)
    {
     setup_armed=false;
     if((trb[0][2]&0x00ffffffu)!=0){fault=-EPROTO;return;}
     setup_handle();
    }
   else if(phase==1 && ep==1)phase=2;
   else if(phase==4){setup_next();if(configured)bulk_next();}
  }
 else if(type==3 && status==2)
  {
   if((phase==2 && ep==0)||(phase==3 && ep==1))
    {phase=4;transfer(ep,three_stage?4:3,0);}
  }
}
static int link_run(bool video)
{
 if(atomic_load(&attempted))return -EALREADY;
 /* Same read-only prerequisites previously validated by k7usb otg. */
 uint32_t power=*(volatile uint32_t *)0x27380570ul;
 uint32_t idle=*(volatile uint32_t *)0x27380128ul;
 uint32_t ack=*(volatile uint32_t *)0x27380120ul;
 uint32_t gates=*(volatile uint32_t *)0x272008bcul;
 uint32_t reset=*(volatile uint32_t *)0x27200abcul;
 if(!(power&(1u<<16)) || ((idle|ack)&(1u<<10)) ||
    (gates&((1u<<1)|(1u<<2)|(7u<<5))) || (reset&(1u<<5)))return -EHOSTDOWN;
 if((rd(NULL,0xc120)>>16)!=0x5533 || ((rd(NULL,0xc110)>>12)&3)!=2)return -ENODEV;
 if(!(rd(NULL,0xc70c)&(1u<<22)))return -EBUSY;
 if(k7_dwc3_dma_range((uintptr_t)events,sizeof(events),CONFIG_RAM_START,CONFIG_RAM_SIZE,64) ||
    k7_dwc3_dma_range((uintptr_t)trb,sizeof(trb),CONFIG_RAM_START,CONFIG_RAM_SIZE,64) ||
    k7_dwc3_dma_range((uintptr_t)data,sizeof(data),CONFIG_RAM_START,CONFIG_RAM_SIZE,64) ||
    k7_dwc3_dma_range((uintptr_t)bulk_data,sizeof(bulk_data),CONFIG_RAM_START,CONFIG_RAM_SIZE,64) ||
    k7_dwc3_dma_range((uintptr_t)bulk_trb,sizeof(bulk_trb),CONFIG_RAM_START,CONFIG_RAM_SIZE,64))return -EFAULT;
 if(atomic_exchange(&attempted,true))return -EALREADY;
 video_mode=video;
 int prepared=usb2_prepare();
 if(prepared)return prepared;
 wr(NULL,0xc704,rd(NULL,0xc704)|(1u<<30));
 if(waitbit(0xc704,1u<<30,0))return -ETIMEDOUT;
 memset(events,0,sizeof(events));clean(events,sizeof(events));
 wr(NULL,0xc700,rd(NULL,0xc700)&~(7u|(127u<<3)));
 wr(NULL,0xc400,(uint32_t)(uintptr_t)events);wr(NULL,0xc404,(uintptr_t)events>>32);
 wr(NULL,0xc408,sizeof(events)|(1u<<31));
 wr(NULL,0xc40c,rd(NULL,0xc40c)&0xfffcu);
 wr(NULL,0xc708,7);
 command(0,9,0,0,0);
 for(unsigned int ep=0;ep<2 && !fault;ep++)
  {
   if(command(ep,1,64u<<3,(1u<<8)|(1u<<10)|(ep<<25),0))break;
   if(command(ep,2,1,0,0))break;
  }
 /* Reserve in ascending endpoint order, after the control endpoints.
  * Keep resource assignment persistent for the configuration lifetime. */
 if(!fault)command(2,2,1,0,0);
 if(!fault)command(3,2,1,0,0);
 if(!fault)
  {
   wr(NULL,0xc720,3);wr(NULL,0xc704,rd(NULL,0xc704)|(1u<<31));
   /* Product preview remains available while the board is running. Only the
    * explicit bulk diagnostic has a time/block limit. Faults still stop DMA. */
   printf("LINK running %s mode=%s, private VID/PID 1209:0001\n",
          video_mode?"continuous":"600s", video_mode?"JPEG video":"Bulk test");
   uint64_t start=now(NULL);
   while(!fault && (video_mode ||
          (now(NULL)-start<UINT64_C(600000000) && bulk_count<1024)))
    {
     size_t bytes=0;fault=k7_dwc3_event_bytes(rd(NULL,0xc40c),sizeof(events),&bytes);
     if(fault)break;
     if(bytes)
      {
       invalidate(events,sizeof(events));
       for(size_t i=0;i<bytes/4 && !fault;i++)
        {uint32_t v=events[position];position=(position+1)%256;event(v);}
       wr(NULL,0xc40c,bytes);
      }
     delay(NULL,1000);
     if(configured && video_mode)bulk_next();
    }
  }
 wr(NULL,0xc708,0);wr(NULL,0xc704,rd(NULL,0xc704)&~(1u<<31));
 int stopped=waitbit(0xc70c,1u<<22,1u<<22);
 k7_video_queue_close();
 /* DMA uses bulk_data, never the camera queue's storage. */
 if(video_slot>=0){k7_video_queue_release(video_slot);video_slot=-1;}
 /* Static DMA buffers are retained even if stop fails. Reboot before retry. */
 if(fault)
  {
   printf("LINK failure ep=%u cmd=%u reg=%08lx result=%d\n",
          failed_ep,failed_command,(unsigned long)failed_register,fault);
   delay(NULL,10000);
  }
 printf("LINK END reset=%u connect=%u setup=%u config=%u result=%d stop=%d\n",
        resets,connects,setups,configured,fault,stopped);
 delay(NULL,10000);printf("BULK blocks=%u bytes=%lu\n",bulk_count,
                          (unsigned long)bulk_count*sizeof(bulk_data));
 delay(NULL,10000);printf("VIDEO frames=%u\n",video_frames);
 for(unsigned int i=0;i<resets && i<16;i++)
  {delay(NULL,10000);printf("RESET %u phase=%u\n",i,reset_phase[i]);}
 for(unsigned int i=0;i<connects && i<16;i++)
  {delay(NULL,10000);printf("CONNECT %u speed=%u\n",i,connect_speed[i]);}
 for(unsigned int i=0;i<setups && i<32;i++)
  {
   char line[96];
   int n=snprintf(line,sizeof(line),"SETUP %02u %02x %02x %04x %04x %04x\n",i,
      setup_trace[i][0],setup_trace[i][1],word(setup_trace[i]+2),
      word(setup_trace[i]+4),word(setup_trace[i]+6));
   for(int j=0;j<n && (size_t)j<sizeof(line);j++)
    {putchar(line[j]);fflush(stdout);delay(NULL,1000);}
  }
 return fault?fault:stopped;
}
int k7_link_probe(void){return link_run(false);}
int k7_link_video(void){return link_run(true);}
