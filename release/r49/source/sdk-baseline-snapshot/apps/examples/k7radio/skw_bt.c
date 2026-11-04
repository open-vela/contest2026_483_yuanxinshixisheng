/* SPDX-License-Identifier: GPL-2.0-only */
/* NuttX bt_driver_s bridge to the Seekwave SDIO2 codec. No Linux shim. */
#include "skw_bt.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <nuttx/net/bluetooth.h>

static int ready(struct skw_bt *bt)
{
  int ret = bt->lower->ready(bt->ctx);
  return ret <= 0 ? ret : -EIO;
}

static int skw_bt_open(struct bt_driver_s *dev)
{
  struct skw_bt *bt = dev->priv;
  int ret = nxmutex_lock(&bt->lock);
  if (ret) return ret;
  ret = ready(bt);
  bt->opened = ret == 0;
  nxmutex_unlock(&bt->lock);
  return ret;
}

static void skw_bt_close(struct bt_driver_s *dev)
{
  struct skw_bt *bt = dev->priv;
  if (nxmutex_lock(&bt->lock) < 0) return;
  bt->opened = false;
  nxmutex_unlock(&bt->lock);
}

static int skw_bt_send(struct bt_driver_s *dev, enum bt_buf_type_e type,
                        void *data, size_t length)
{
  struct skw_bt *bt = dev->priv;
  uint8_t h4;
  size_t written;
  int ret;
  if (type == BT_CMD) h4 = 1;
  else if (type == BT_ACL_OUT) h4 = 2;
  else return -ENOTSUP;          /* This NuttX enum has no SCO entry. */
  ret = nxmutex_lock(&bt->lock);
  if (ret) return ret;
  if (!bt->opened) { ret = -ENOTCONN; goto out; }
  ret = ready(bt);
  if (ret) { bt->opened = false; goto out; }
  if (type == BT_CMD && length >= 3)
    {
      const uint8_t *p = data;
      uint16_t op=p[0] | p[1]<<8;
      printf("RADIO HCI TX op=%04x bytes=%zu\n",op,length);
      if (op==0x0c33 && length==10)
        printf("RADIO HCI host buffers ACL_MTU=%u ACL_COUNT=%u\n",p[3]|p[4]<<8,p[6]|p[7]<<8);
      if (op==0x0c31 && length==4)
        printf("RADIO HCI controller-to-host flow=%u\n",p[3]);
    }
  ret = skw_hci_encode(h4, data, length, bt->tx, sizeof(bt->tx), &written);
  if (ret) goto out;
  ret = bt->lower->send(bt->ctx, bt->tx, written);
  if (!ret && type == BT_ACL_OUT)
    {
      const uint8_t *p = data;
      bt->acl_tx++;
      printf("RADIO ACL TX count=%u bytes=%zu\n",bt->acl_tx,length);
      if (length >= 9 && (p[1] & 0x30) != 0x10 && p[6] == 6 && p[7] == 0)
        {
          printf("RADIO SMP TX opcode=%02x bytes=%zu\n",p[8],length);
          if (p[8] == 5 && length >= 10)
            printf("RADIO SMP TX failure reason=%u\n",p[9]);
        }
      if (length >= 9 && (p[1] & 0x30) != 0x10 && p[6] == 4 && p[7] == 0)
        printf("RADIO BLE ATT TX queued=%u opcode=%02x bytes=%zu\n",bt->acl_tx,p[8],length);
    }
  if (ret)
    {
      bt->opened = false;
      if (ret > 0) ret = -EIO;
    }
  /* Match this tree's btuart_send: OK (0), not a byte count. */
out:
  nxmutex_unlock(&bt->lock);
  return ret;
}

int skw_bt_init(struct skw_bt *bt, const struct skw_bt_lower *lower, void *ctx)
{
  int ret;
  if (!bt || !lower || !lower->ready || !lower->send || !lower->link_ack)
    return -EINVAL;
  memset(bt, 0, sizeof(*bt));
  ret = nxmutex_init(&bt->lock);
  if (ret) return ret;
  memset(bt->adv_status, 0xff, sizeof(bt->adv_status));
  bt->ctx = ctx;
  bt->lower = lower;
  bt->driver.open = skw_bt_open;
  bt->driver.close = skw_bt_close;
  bt->driver.send = skw_bt_send;
  bt->driver.priv = bt;
  return 0;
}

int skw_bt_receive_slot(struct skw_bt *bt, const void *slot, size_t available)
{
  struct skw_packet packet;
  struct skw_hci_packet hci;
  enum bt_buf_type_e type;
  int ret;
  if (!bt || !bt->lower) return -EINVAL;
  ret = skw_sdio2_decode_slot(slot, available, &packet);
  if (ret || packet.eof || packet.discard) return ret;
  ret = skw_hci_decode(&packet, &hci);
  if (ret)
    { printf("RADIO BT HCI decode ret=%d channel=%u length=%zu\n", ret, packet.channel, packet.length); return ret; }
  ret = nxmutex_lock(&bt->lock);
  if (ret) return ret;
  if (!bt->opened) ret = -ENOTCONN;
  else ret = ready(bt);
  if (!ret && !hci.ack && !hci.vendor_log)
    {
      const uint8_t *p = hci.data; size_t n = hci.length;
      if (hci.type == 4 && n >= 6 && p[0] == 0x0e)
        {
          uint16_t op=p[3] | p[4]<<8;
          printf("RADIO HCI complete op=%04x status=%u\n",op,p[5]);
          if (op==0x2002 && n>=9)
            printf("RADIO HCI controller buffers LE_MTU=%u LE_COUNT=%u\n",p[6]|p[7]<<8,p[8]);
          const uint16_t ops[]={0x2008,0x2009,0x2006,0x200a};
          for (unsigned int i=0;i<4;i++) if (op==ops[i])
            { bt->adv_status[i]=p[5]; printf("RADIO BLE advertising HCI op=%04x status=%u\n",op,p[5]); }
        }
      if (hci.type == 4 && n >= 6 && p[0] == 0x0f)
        printf("RADIO HCI command status op=%04x status=%u\n",p[4]|p[5]<<8,p[2]);
      if (hci.type == 4 && n >= 3 && p[0] == 0x3e)
        printf("RADIO HCI LE subevent=%02x bytes=%zu\n",p[2],n);
      if (hci.type == 4 && n >= 6 && p[0] == 0x08)
        {
          bt->encryption_status=p[2]; bt->encryption_enabled=p[5];
          printf("RADIO BLE encryption status=%u handle=%u enabled=%u\n",p[2],p[3]|p[4]<<8,p[5]);
        }
      if (hci.type == 4 && n >= 21 && p[0] == 0x3e && p[2] == 1)
        {
          if (!p[3])
            {
              bt->connected=true; bt->connections++; bt->handle=p[4] | p[5]<<8;
              bt->encryption_status=0xff; bt->encryption_enabled=0;
            }
          printf("RADIO BLE connection status=%u handle=%u role=%u\n",p[3],p[4]|p[5]<<8,p[6]);
        }
      if (hci.type == 4 && n >= 6 && p[0] == 5)
        {
          if (!p[2] && bt->handle==(p[3]|p[4]<<8))
            { bt->connected=false; bt->encryption_enabled=0; }
          bt->disconnections++; bt->disconnect_reason=p[5];
          printf("RADIO BLE disconnected status=%u handle=%u reason=%u\n",p[2],p[3]|p[4]<<8,p[5]);
        }
      if (hci.type == 2)
        {
          bt->acl_rx++;
          printf("RADIO ACL RX count=%u bytes=%zu handle_flags=%04x\n",bt->acl_rx,n,p[0]|p[1]<<8);
          if (n >= 9 && (p[1] & 0x30) != 0x10 && p[6] == 6 && p[7] == 0)
            {
              printf("RADIO SMP RX opcode=%02x bytes=%zu\n",p[8],n);
              if (p[8]==5 && n>=10) printf("RADIO SMP failure reason=%u\n",p[9]);
            }
          if (n >= 9 && (p[1] & 0x30) != 0x10 && p[6] == 4 && p[7] == 0)
            printf("RADIO BLE ATT RX count=%u opcode=%02x bytes=%zu\n",bt->acl_rx,p[8],n);
        }
    }
  nxmutex_unlock(&bt->lock);
  if (ret) return ret;
  if (hci.vendor_log) return 0; /* Same separation as skwbt rx_complete. */
  if (hci.ack)
    {
      ret = bt->lower->link_ack(bt->ctx, packet.channel, hci.sequence);
      return ret <= 0 ? ret : -EIO;
    }
  if (hci.type == 4) type = BT_EVT;
  else if (hci.type == 2) type = BT_ACL_IN;
  else return -ENOTSUP;
  if (!bt->driver.receive) return -ENOTCONN;
  /* Legacy host bt_receive has only DEBUGASSERT before copying into IOB.
   * Enforce its declared frame capacity here, including the H4 reserve.
   */
  if (hci.length > BLUETOOTH_MAX_FRAMELEN - bt->driver.head_reserve ||
      hci.length > CONFIG_IOB_BUFSIZE - bt->driver.head_reserve || hci.length > UINT8_MAX)
    return -EMSGSIZE;
  /* Do not hold the TX mutex while upper-half receive may cause a send. */
  ret = bt_netdev_receive(&bt->driver, type, (void *)hci.data, hci.length);
  if (ret) printf("RADIO BT upper receive ret=%d type=%u length=%zu\n", ret, type, hci.length);
  return ret;
}
