#ifndef VV_CHAT_SEARCH_POLICY_H
#define VV_CHAT_SEARCH_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Bounded, local routing only. History must not turn a later ordinary question
 * into a search. The caller already validates UTF-8 and the request limit. */
static inline bool vv_search_has(const char *start,const char *end,const char *word)
{
 const char *p=strstr(start,word);
 return p && p<end && (size_t)(end-p)>=strlen(word);
}
static inline bool vv_chat_wants_search(const char *text)
{
 if(!text)return false;
 const char *start=text,*end=text+strlen(text);
 static const char history[]="以下是本次对话的历史记录，只用于理解上下文，请回答最后的用户问题。\n";
 static const char current[]="当前用户：",tail[]="\n助手：";
 if(!strncmp(text,history,sizeof(history)-1) &&
    (size_t)(end-text)>=sizeof(tail)-1 && !memcmp(end-(sizeof(tail)-1),tail,sizeof(tail)-1)){
  const char *p=text,*last=NULL;
  while((p=strstr(p,current))!=NULL){last=p;p+=sizeof(current)-1;}
  if(last){start=last+sizeof(current)-1;end-=sizeof(tail)-1;}
 }
 static const char *const no_search[]={"不要联网","不用联网","不要搜索","不用搜索"};
 for(size_t i=0;i<sizeof(no_search)/sizeof(*no_search);++i)
  if(vv_search_has(start,end,no_search[i]))return false;
 static const char *const explicit_search[]={"联网搜索","上网查","网上查","帮我查","帮忙查","搜索一下","查一下"};
 for(size_t i=0;i<sizeof(explicit_search)/sizeof(*explicit_search);++i)
  if(vv_search_has(start,end,explicit_search[i]))return true;
 static const char *const creative[]={"讲个","讲一个","编个","编一个","写个","写一个","写一篇","写一首","造句","翻译","童话","故事","笑话","谜语","原理","新闻学","气象学"};
 for(size_t i=0;i<sizeof(creative)/sizeof(*creative);++i)
  if(vv_search_has(start,end,creative[i]))return false;
 bool current_time=false;
 static const char *const timely[]={"今天","明天","后天","这周","本周","最近","近期","现在","今年","实时","最新"};
 for(size_t i=0;i<sizeof(timely)/sizeof(*timely);++i)
  if(vv_search_has(start,end,timely[i])){current_time=true;break;}
 /* Definitions and explanations retain the ordinary answer path. */
 static const char *const explanation[]={"什么是","是什么","是什么意思","是什么原理","为什么","怎么形成","如何形成"};
 for(size_t i=0;i<sizeof(explanation)/sizeof(*explanation);++i)
  if(!current_time && vv_search_has(start,end,explanation[i]))return false;
 static const char *const live_topics[]={"天气","气温","下雨","降雨","新闻","最新","实时","近况",
  "营业时间","开放时间","几点开门","几点关门","几点开馆","几点闭馆","什么时候开门","什么时候关门","今天开","明天开","门票多少钱",
  "开馆吗","闭馆吗","是否开馆","是否闭馆","开馆时间","闭馆时间","什么时候开馆","什么时候闭馆","开闭馆时间","开不开馆",
  "比赛结果","比分","赛程","谁赢了","油价","金价","汇率","股价","限行"};
 for(size_t i=0;i<sizeof(live_topics)/sizeof(*live_topics);++i)
  if(vv_search_has(start,end,live_topics[i]))return true;
 return false;
}
#endif
