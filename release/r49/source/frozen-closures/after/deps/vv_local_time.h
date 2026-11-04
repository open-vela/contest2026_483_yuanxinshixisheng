#ifndef VV_LOCAL_TIME_H
#define VV_LOCAL_TIME_H
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Pure, bounded exact-question recognition. This is not a motion/ASR intent
 * parser, and it must receive only the current utterance, never chat history. */
enum vv_local_time_kind { VV_LOCAL_TIME_NONE, VV_LOCAL_TIME_CLOCK,
                         VV_LOCAL_TIME_DATE, VV_LOCAL_TIME_WEEKDAY };
static inline enum vv_local_time_kind vv_local_time_query(const char *text)
{
  if(!text)return VV_LOCAL_TIME_NONE;
  char question[160];size_t n=0;
  while(n<sizeof(question)&&text[n])n++;
  if(n==sizeof(question))return VV_LOCAL_TIME_NONE;
  memcpy(question,text,n+1);
  for(;;){
    if(n&&(question[n-1]==' '||question[n-1]=='\t'||question[n-1]=='\r'||
           question[n-1]=='\n'||question[n-1]=='?'||question[n-1]=='!'||question[n-1]=='.')){
      question[--n]=0;continue;
    }
    if(n>=3&&(!memcmp(question+n-3,"？",3)||!memcmp(question+n-3,"。",3)||
              !memcmp(question+n-3,"！",3)||!memcmp(question+n-3,"　",3))){
      n-=3;question[n]=0;continue;
    }
    break;
  }
  const char *q=question;
  while(*q==' '||*q=='\t'||*q=='\r'||*q=='\n')q++;
  const char *const prefixes[]={"请告诉我","告诉我","请问"};
  for(size_t i=0;i<sizeof(prefixes)/sizeof(*prefixes);i++){
    size_t length=strlen(prefixes[i]);
    if(!strncmp(q,prefixes[i],length)){q+=length;break;}
  }
  const char *const clocks[]={"现在几点","现在几点了","现在几点钟","现在是几点",
    "现在是几点钟","几点了","北京时间","现在的北京时间","现在北京时间",
    "现在北京时间几点","现在北京时间几点了","北京时间是多少","现在北京时间是多少",
    "当前时间","现在时间","现在是什么时间","现在的时间","当前北京时间"};
  const char *const dates[]={"今天几号","今天是几号","今天日期","今天的日期",
    "今天是什么日期","今天是几月几号","今天是几月几日"};
  const char *const weekdays[]={"今天星期几","今天是星期几","今天周几","今天是周几",
    "今天礼拜几","今天是礼拜几","星期几"};
  for(size_t i=0;i<sizeof(clocks)/sizeof(*clocks);i++)if(!strcmp(q,clocks[i]))return VV_LOCAL_TIME_CLOCK;
  for(size_t i=0;i<sizeof(dates)/sizeof(*dates);i++)if(!strcmp(q,dates[i]))return VV_LOCAL_TIME_DATE;
  for(size_t i=0;i<sizeof(weekdays)/sizeof(*weekdays);i++)if(!strcmp(q,weekdays[i]))return VV_LOCAL_TIME_WEEKDAY;
  return VV_LOCAL_TIME_NONE;
}
static inline int vv_local_time_format(enum vv_local_time_kind kind,uint64_t utc,
                                       char *out,size_t capacity)
{
  if(out&&capacity)out[0]=0;
  if(!out||!capacity||kind<VV_LOCAL_TIME_CLOCK||kind>VV_LOCAL_TIME_WEEKDAY||!utc)return -EINVAL;
  /* UTC+8 must remain within Gregorian year 9999. No time_t/local TZ dependency,
   * host clock, compiler date, DST guessing or network fallback is consulted. */
  if(utc>UINT64_C(253402271999))return -ERANGE;
  uint64_t local=utc+UINT64_C(28800),days=local/86400;
  unsigned second=(unsigned)(local%86400),weekday=(unsigned)((days+4)%7);
  unsigned year=1970,month=1;
  for(;;){
    unsigned year_days=365+((year%4==0)&&(year%100!=0||year%400==0));
    if(days<year_days)break;
    days-=year_days;year++;
  }
  const unsigned month_days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  for(;;){
    unsigned length=month_days[month-1]+
      (month==2&&(year%4==0)&&(year%100!=0||year%400==0));
    if(days<length)break;
    days-=length;month++;
  }
  const char *const week[]={"日","一","二","三","四","五","六"};
  int written;
  if(kind==VV_LOCAL_TIME_CLOCK)
    written=snprintf(out,capacity,"现在是北京时间%04u年%u月%u日，星期%s，%02u点%02u分%02u秒。",
                     year,month,(unsigned)days+1,week[weekday],
                     second/3600,(second/60)%60,second%60);
  else if(kind==VV_LOCAL_TIME_DATE)
    written=snprintf(out,capacity,"今天是北京时间%04u年%u月%u日。",
                     year,month,(unsigned)days+1);
  else{
    written=snprintf(out,capacity,"按北京时间，今天是星期%s。",week[weekday]);
  }
  if(written<0||(size_t)written>=capacity){out[0]=0;return -ENOSPC;}
  return 0;
}
#endif
