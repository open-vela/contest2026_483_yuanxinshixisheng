#include "vv_rtc_time.h"
#include <errno.h>
#include <stddef.h>

/* Register facts checked against HYM8563 upstream Linux driver and the K7
 * schematic. This is an independent pure decoder, not a copied Linux driver.
 * https://github.com/torvalds/linux/blob/master/drivers/rtc/rtc-hym8563.c */
static int decimal(uint8_t value) {
  unsigned a = value >> 4, b = value & 15;
  return a > 9 || b > 9 ? -1 : (int)(a * 10 + b);
}
static int leap(unsigned year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
bool vv_rtc_hym8563_same_fields(const uint8_t a[9],const uint8_t b[9]) {
  static const uint8_t masks[9]={0xa8,0,0xff,0x7f,0x3f,0x3f,7,0x1f,0xff};
  if(!a||!b)return false;
  for(unsigned i=0;i<9;i++)if((a[i]&masks[i])!=(b[i]&masks[i]))return false;
  return true;
}
int vv_rtc_hym8563_decode(const uint8_t r[9], uint64_t *utc) {
  if (utc) *utc = 0;
  if (!r || !utc) return -EINVAL;
  if ((r[0] & 0xa8) || (r[2] & 0x80)) return -ENODATA;
  /* Datasheet unused bits are not calendar data. HYM century readback is
   * unreliable; use the same explicit 2000..2099 policy as Linux driver. */
  if ((r[6]&7) > 6) return -ERANGE;
  int second = decimal(r[2]&0x7f), minute = decimal(r[3]&0x7f);
  int hour = decimal(r[4]&0x3f), day = decimal(r[5]&0x3f);
  int month = decimal(r[7]&0x1f), year2 = decimal(r[8]);
  if (second < 0 || second > 59 || minute < 0 || minute > 59 ||
      hour < 0 || hour > 23 || day < 1 || month < 1 || month > 12 || year2 < 0)
    return -ERANGE;
  unsigned year = 2000 + (unsigned)year2;
  static const unsigned lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  unsigned limit = lengths[month - 1] + (month == 2 && leap(year));
  if ((unsigned)day > limit) return -ERANGE;
  uint64_t days = 0;
  for (unsigned y = 1970; y < year; ++y) days += 365 + leap(y);
  for (int m = 1; m < month; ++m) days += lengths[m - 1] + (m == 2 && leap(year));
  days += (unsigned)day - 1;
  *utc = days * 86400 + (unsigned)hour * 3600 + (unsigned)minute * 60 + (unsigned)second;
  return 0;
}
