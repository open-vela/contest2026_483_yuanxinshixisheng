/* SPDX-License-Identifier: GPL-2.0-only
 * Derived from rk3x_i2c_v1_calc_timings, official frozen i2c-rk3x.c.
 * Original driver: Max Schwarz, based on patches by Rockchip Inc.
 * Unlike upstream, invalid or unrepresentable requests never clamp silently.
 */
#include "i2c3_timing.h"
#include <errno.h>
#include <stddef.h>
static uint64_t ceildiv(uint64_t n,uint64_t d){return n/d+(n%d!=0);}
int k7_i2c3_timing(const struct k7_i2c_timing_request *r,struct k7_i2c_timing *out)
{
 uint64_t khz,total,lo,hi,hold,extra,s,u,p,v;
 struct k7_i2c_timing result;
 if(!r || !out)return -EINVAL;
 if(!r->inputs_known)return -EAGAIN;
 if(!r->input_hz || r->bus_hz<1000 || r->bus_hz>100000)return -EINVAL;
 /* All products fit uint64 for uint32 inputs; additions are promoted first. */
 khz=ceildiv(r->input_hz,1000);
 total=ceildiv(khz,(r->bus_hz/1000u)*8u);
 hi=ceildiv(khz*((uint64_t)r->scl_rise_ns+4000),8000000);
 lo=ceildiv(khz*((uint64_t)r->scl_fall_ns+4700),8000000);
 hi=hi<1?2:hi;lo=lo<1?2:lo;
 hold=lo+hi;
 if(hold<total){extra=total-hold;v=ceildiv(lo*extra,hold);lo+=v;hi+=extra-v;}
 if(lo<1 || hi<1 || lo>65536 || hi>65536)return -ERANGE;
 for(s=3;s>0;s--){
   uint64_t hd=ceildiv((s*lo+1)*1000000,khz);
   uint64_t su=ceildiv(((8-s)*lo+1)*1000000,khz);
   if(hd<3450 && su>250)break;
 }
 if(!s)return -ERANGE; /* No upstream fallback-to-one without proof. */
 v=khz*((uint64_t)r->scl_rise_ns+4700);
 u=v<=1000000?1:ceildiv(v-1000000,8000000*hi);
 v=khz*((uint64_t)r->scl_rise_ns+4000);
 p=v<=1000000?1:ceildiv(v-1000000,8000000*hi);
 if(!u)u=1;
 if(!p)p=1;
 if(u>3 || p>3)return -ERANGE;
 /* Validate exact Hz formulas: rounded-kHz upstream arithmetic must not
  * yield a setting violating the supplied edge times or standard-mode bounds. */
 if(8*lo*UINT64_C(1000000000)<(uint64_t)r->input_hz*((uint64_t)r->scl_fall_ns+4700) ||
    8*hi*UINT64_C(1000000000)<(uint64_t)r->input_hz*((uint64_t)r->scl_rise_ns+4000) ||
    (s*lo+1)*UINT64_C(1000000000)>=(uint64_t)r->input_hz*3450 ||
    ((8-s)*lo+1)*UINT64_C(1000000000)<=(uint64_t)r->input_hz*250 ||
    (8*hi*u+1)*UINT64_C(1000000000)<(uint64_t)r->input_hz*((uint64_t)r->scl_rise_ns+4700) ||
    (8*hi*(u+1)-1)*UINT64_C(1000000000)<(uint64_t)r->input_hz*4000 ||
    (8*hi*p+1)*UINT64_C(1000000000)<(uint64_t)r->input_hz*((uint64_t)r->scl_rise_ns+4000) ||
    (uint64_t)r->input_hz>(uint64_t)r->bus_hz*8*(lo+hi))return -ERANGE;
 result.div_low=(uint16_t)(lo-1);result.div_high=(uint16_t)(hi-1);
 result.clkdiv=((uint32_t)result.div_high<<16)|result.div_low;
 result.tuning=(uint32_t)((s-1)<<8 | (u-1)<<12 | (p-1)<<14);
 result.scl_hz_ceiling=(uint32_t)ceildiv(r->input_hz,8*(lo+hi));
 *out=result;return 0;
}
