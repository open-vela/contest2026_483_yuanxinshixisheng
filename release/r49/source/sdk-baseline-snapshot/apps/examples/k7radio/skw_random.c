/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <nuttx/wireless/bluetooth/bt_buf.h>
#include <nuttx/wireless/bluetooth/bt_hci.h>
#include "bt_hcicore.h"
/* Same controller LE_Rand source used by the native Bluetooth SMP host.
 * Called only from the Wi-Fi task, never the HCI receive/LPWORK thread.
 * No fallback to timer, rand(), MAC address, or deterministic entropy. */
int skw_controller_random(uint8_t *out,size_t len)
{
  if(!out)return -EINVAL;
  while(len)
    {
      struct bt_buf_s *rsp=NULL;
      int ret=bt_hci_cmd_send_sync(BT_HCI_OP_LE_RAND,NULL,&rsp);
      if(ret)return ret;
      if(!rsp || rsp->len<sizeof(struct bt_hci_rp_le_rand_s))
        {if(rsp)bt_buf_release(rsp);return -EPROTO;}
      struct bt_hci_rp_le_rand_s *rp=(void *)rsp->data;
      if(rp->status){bt_buf_release(rsp);return -EIO;}
      size_t copy=len<sizeof(rp->rand)?len:sizeof(rp->rand);
      memcpy(out,rp->rand,copy);memset(rp->rand,0,sizeof(rp->rand));
      bt_buf_release(rsp);out+=copy;len-=copy;
    }
  return 0;
}
