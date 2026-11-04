/* SPDX-License-Identifier: Apache-2.0 */
#include "f050008m01.h"
#include <errno.h>
int k7panel_validate(const unsigned char *data, size_t size)
{
  size_t i=0; int count=0;
  if (!data || !size) return -EINVAL;
  while (i<size) {
    if (size-i<3) return -EINVAL;
    unsigned char type=data[i], n=data[i+2]; i+=3;
    if (!n || n>size-i) return -EINVAL;
    if (!((type==0x05 && n==1) || (type==0x15 && n==2) ||
          (type==0x39 && n>=3))) return -EINVAL;
    i+=n; count++;
  }
  return count;
}
int k7panel_run(const unsigned char *data, size_t size, const struct k7panel_io *io)
{
  int rc=k7panel_validate(data,size);
  if (rc<0) return rc;
  if (!io || !io->tx || !io->delay_ms) return -EINVAL;
  for (size_t i=0;i<size;) {
    unsigned char type=data[i], delay=data[i+1], n=data[i+2]; i+=3;
    rc=io->tx(io->arg,type,data+i,n);
    if (rc<0) return rc;
    if (rc!=n) return -EIO;
    if (delay && (rc=io->delay_ms(io->arg,delay))<0) return rc;
    i+=n;
  }
  return 0;
}
