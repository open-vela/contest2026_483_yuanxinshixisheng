/* SPDX-License-Identifier: Apache-2.0 */
/* Native NuttX Ethernet endpoint. No Linux shim and no fabricated IP state.
 * RX only copies into a bounded queue; network/SDIO calls run in Wi-Fi task.
 */
#include <nuttx/config.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/net.h>
#include <nuttx/net/ip.h>
#include <nuttx/mm/iob.h>
#include <nuttx/mutex.h>
#include <netutils/netlib.h>
#include <netutils/dhcpc.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "skw_wifi_data.h"
#include "skw_netdev.h"
static struct net_driver_s device;
static mutex_t queue_lock=NXMUTEX_INITIALIZER,lease_lock=NXMUTEX_INITIALIZER;
static bool registered,up,accepting;
static struct {size_t len;uint8_t bytes[SKW_NET_FRAME_MAX];} queue[8],current;
static unsigned head,count,rx_count,tx_count,drops;
static unsigned arp_rx_request,arp_rx_reply,arp_tx_request,arp_tx_reply;
static uint8_t tx_frame[SKW_NET_FRAME_MAX];static size_t tx_length;
static unsigned tx_attempts,tx_drops;
static bool tx_reset;
/* Only the Wi-Fi owner changes pending bytes; interface callbacks invalidate. */
static void clear_tx(void)
{memset(tx_frame,0,sizeof(tx_frame));tx_length=0;tx_attempts=0;}
static void *dhcp;
static struct dhcpc_state lease;
static int lease_state;
static struct dhcpc_state applied_lease;
static bool lease_applied;
#include "skw_lease_dns.inc"
static int interface_up(struct net_driver_s *dev)
{(void)dev;__atomic_store_n(&up,true,__ATOMIC_RELEASE);return 0;}
static int interface_down(struct net_driver_s *dev)
{(void)dev;__atomic_store_n(&tx_reset,true,__ATOMIC_RELEASE);__atomic_store_n(&up,false,__ATOMIC_RELEASE);return 0;}
static int tx_available(struct net_driver_s *dev)
{(void)dev;return 0;} /* Wi-Fi task polls every <=5 ms. */
static int collect_tx(struct net_driver_s *dev)
{
  if(!dev->d_len)return 0;
  if(dev->d_len>sizeof(tx_frame) || tx_length)
    {NETDEV_TXERRORS(dev);dev->d_len=0;return 1;}
  if(iob_copyout(tx_frame,dev->d_iob,dev->d_len,-NET_LL_HDRLEN(dev))==dev->d_len)
    tx_length=dev->d_len;
  dev->d_len=0;return 1;
}
int skw_netdev_open(const uint8_t mac[6])
{
  if(dhcp || accepting)return -EBUSY;
  if(!registered)
    {
      memset(&device,0,sizeof(device));strcpy(device.d_ifname,"wlan%d");
      device.d_ifup=interface_up;device.d_ifdown=interface_down;device.d_txavail=tx_available;
      device.d_pktsize=SKW_NET_FRAME_MAX;memcpy(device.d_mac.ether.ether_addr_octet,mac,6);
      int ret=netdev_register(&device,NET_LL_ETHERNET);if(ret)return ret;
      registered=true;
    }
  nxmutex_lock(&queue_lock);head=count=rx_count=tx_count=drops=0;accepting=true;nxmutex_unlock(&queue_lock);
  clear_tx();tx_drops=0;__atomic_store_n(&tx_reset,false,__ATOMIC_RELEASE);
  arp_rx_request=arp_rx_reply=arp_tx_request=arp_tx_reply=0;
  int ret=netlib_ifup(device.d_ifname);
  if(ret){nxmutex_lock(&queue_lock);accepting=false;nxmutex_unlock(&queue_lock);return -errno;}
  netdev_carrier_on(&device);
  printf("WIFI netdev=%s MTU=%u up=1\n",device.d_ifname,SKW_NET_FRAME_MAX-14);return 0;
}
int skw_netdev_rx(const uint8_t *frame,size_t length)
{
  if(!frame || length<14 || length>SKW_NET_FRAME_MAX)return -EMSGSIZE;
  nxmutex_lock(&queue_lock);
  if(!accepting){nxmutex_unlock(&queue_lock);return -ENOTCONN;}
  if(count==8){drops++;nxmutex_unlock(&queue_lock);return -ENOSPC;}
  unsigned tail=(head+count)%8;memcpy(queue[tail].bytes,frame,length);queue[tail].len=length;count++;rx_count++;
  nxmutex_unlock(&queue_lock);return 0;
}
int skw_netdev_poll(int (*transmit)(const uint8_t *,size_t))
{
  bool received=false;
  if(__atomic_exchange_n(&tx_reset,false,__ATOMIC_ACQ_REL))clear_tx();
  if(!__atomic_load_n(&up,__ATOMIC_ACQUIRE)){clear_tx();return -ENETDOWN;}
  if(!transmit){clear_tx();return -EINVAL;}
  /* Retried bytes must not be overwritten by RX replies or devif_poll. */
  if(tx_length)goto send_pending;
  nxmutex_lock(&queue_lock);
  if(count){current=queue[head];head=(head+1)%8;count--;received=true;}
  nxmutex_unlock(&queue_lock);
  net_lock();
  if(received && !netdev_iob_prepare(&device,false,0))
    {
      device.d_buf=NULL; /* IOB mode, as in SDK's Ethernet TAP driver. */
      if(iob_trycopyin(device.d_iob,current.bytes,current.len,-NET_LL_HDRLEN(&device),false)>=0)
        {
          device.d_len=current.len;NETDEV_RXPACKETS(&device);
          if(current.bytes[12]==8 && current.bytes[13]==0)
            {NETDEV_RXIPV4(&device);ipv4_input(&device);}
          else if(current.bytes[12]==8 && current.bytes[13]==6)
            {
             if(current.len>=42 && current.bytes[20]==0)
               {
                if(current.bytes[21]==1)__atomic_add_fetch(&arp_rx_request,1,__ATOMIC_RELAXED);
                if(current.bytes[21]==2)__atomic_add_fetch(&arp_rx_reply,1,__ATOMIC_RELAXED);
               }
             NETDEV_RXARP(&device);arp_input(&device);
            }
          else device.d_len=0;
          if(device.d_len)collect_tx(&device);
        }
      netdev_iob_release(&device);
    }
  if(!tx_length)
    {devif_poll(&device,collect_tx);netdev_iob_release(&device);}
  net_unlock();
send_pending:
  if(tx_length)
    {
      bool arp=tx_length>=42 && tx_frame[12]==8 && tx_frame[13]==6 && tx_frame[20]==0;
      unsigned opcode=arp?tx_frame[21]:0;
      int ret=transmit(tx_frame,tx_length);tx_attempts++;
      if(ret)
        {
          bool rejected=ret==-EAGAIN &&
            __atomic_load_n(&up,__ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&tx_reset,__ATOMIC_ACQUIRE);
          bool retry=rejected && tx_attempts<3 &&
            __atomic_load_n(&up,__ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&tx_reset,__ATOMIC_ACQUIRE);
          printf("WIFI frame_tx attempt=%u rc=%d retry=%u bytes=%zu\n",
                 tx_attempts,ret,retry?1u:0u,tx_length);
          /* Count failed frames once, not each transmission attempt. */
          if(retry)return 0;
          NETDEV_TXERRORS(&device);
          /* Only the explicit firmware-rejected data frame is discarded.
           * TCP still owns its unacked write buffer; do not replay a POST or
           * tear down the IP lease. Two later 5ms polls bound local retention. */
          if(rejected){tx_drops++;printf("WIFI frame_tx drop=1 total=%u rc=%d\n",tx_drops,ret);}
          clear_tx();return rejected?0:ret;
        }
      if(tx_attempts>1)
        printf("WIFI frame_tx attempt=%u rc=0 retry=0 bytes=%zu\n",tx_attempts,tx_length);
      clear_tx();
      if(opcode==1)__atomic_add_fetch(&arp_tx_request,1,__ATOMIC_RELAXED);
      if(opcode==2)__atomic_add_fetch(&arp_tx_reply,1,__ATOMIC_RELAXED);
      tx_count++;NETDEV_TXPACKETS(&device);NETDEV_TXDONE(&device);
    }
  return 0;
}
static void dhcp_callback(struct dhcpc_state *state)
{
  nxmutex_lock(&lease_lock);
  /* Revoke resolver eligibility immediately; publish only after application. */
  if(!state || lease_state!=1 || memcmp(state,&lease,sizeof(lease)))
    {lease_dns_address=0;lease_dns_result=-ENODATA;}
  if(state && state->ipaddr.s_addr && state->ipaddr.s_addr!=INADDR_BROADCAST)
    {lease=*state;lease_state=1;}
  else {memset(&lease,0,sizeof(lease));lease_state=-ENETUNREACH;}
  nxmutex_unlock(&lease_lock);
}
int skw_netdev_dhcp_start(void)
{
  if(dhcp)return -EBUSY;
  lease_applied=false;
  nxmutex_lock(&lease_lock);memset(&lease,0,sizeof(lease));lease_state=0;lease_dns_clear();nxmutex_unlock(&lease_lock);
  dhcp=dhcpc_open(device.d_ifname,device.d_mac.ether.ether_addr_octet,6);
  if(!dhcp)return -errno;
  if(dhcpc_request_async(dhcp,dhcp_callback))
    {int ret=-errno;dhcpc_close(dhcp);dhcp=NULL;return ret;}
  return 0;
}
int skw_netdev_dhcp_status(char ip[16],char gateway[16])
{
  struct dhcpc_state state;int result;
  nxmutex_lock(&lease_lock);result=lease_state;state=lease;nxmutex_unlock(&lease_lock);
  if(result!=1){if(result<0){nxmutex_lock(&lease_lock);lease_dns_clear();nxmutex_unlock(&lease_lock);}return result;}
  if(!lease_applied || memcmp(&state,&applied_lease,sizeof(state)))
    {
      if(netlib_set_ipv4addr(device.d_ifname,&state.ipaddr) ||
     netlib_set_ipv4netmask(device.d_ifname,&state.netmask) ||
     netlib_set_dripv4addr(device.d_ifname,&state.default_router))return -errno;
      nxmutex_lock(&lease_lock);
      if(lease_state!=1 || memcmp(&state,&lease,sizeof(state)))
        {nxmutex_unlock(&lease_lock);return 0;}
      lease_dns_apply(&state.dnsaddr);
      applied_lease=state;lease_applied=true;
      int dns_result=lease_dns_result;
      nxmutex_unlock(&lease_lock);
      printf("WIFI DHCP resolver_result=%d source=option6\n",dns_result);
    }
  if(!inet_ntop(AF_INET,&state.ipaddr,ip,16) || !inet_ntop(AF_INET,&state.default_router,gateway,16))return -errno;
  return 1;
}
int skw_netdev_dns_snapshot(uint32_t *ipv4_be)
{
  if(!ipv4_be)return -EINVAL;
  nxmutex_lock(&lease_lock);
  int rc=lease_state==1?lease_dns_result:-ENETDOWN;
  *ipv4_be=rc?0:lease_dns_address;
  nxmutex_unlock(&lease_lock);return rc;
}
void skw_netdev_close(void)
{
  clear_tx();__atomic_store_n(&tx_reset,false,__ATOMIC_RELEASE);
  if(dhcp){dhcpc_close(dhcp);dhcp=NULL;}
  nxmutex_lock(&queue_lock);accepting=false;head=count=0;memset(queue,0,sizeof(queue));nxmutex_unlock(&queue_lock);
  if(registered)
    {
      struct in_addr zero={0};netlib_set_ipv4addr(device.d_ifname,&zero);
      netdev_carrier_off(&device);
      netlib_set_dripv4addr(device.d_ifname,&zero);netlib_set_ipv4netmask(device.d_ifname,&zero);
      netlib_ifdown(device.d_ifname);net_lock();netdev_iob_release(&device);net_unlock();
    }
  nxmutex_lock(&lease_lock);lease_state=0;memset(&lease,0,sizeof(lease));lease_dns_clear();nxmutex_unlock(&lease_lock);
}
void skw_netdev_stats(void)
{
 printf("WIFI frame_tx drops=%u\n",tx_drops);
 printf("WIFI netdev rx=%u tx=%u dropped=%u\n",rx_count,tx_count,drops);
 printf("WIFI ARP rx_request=%u rx_reply=%u tx_request=%u tx_reply=%u wait_ms=%u retries=%u\n",
  __atomic_load_n(&arp_rx_request,__ATOMIC_RELAXED),__atomic_load_n(&arp_rx_reply,__ATOMIC_RELAXED),
  __atomic_load_n(&arp_tx_request,__ATOMIC_RELAXED),__atomic_load_n(&arp_tx_reply,__ATOMIC_RELAXED),
  CONFIG_ARP_SEND_DELAYMSEC,CONFIG_ARP_SEND_MAXTRIES);
}
