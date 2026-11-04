/* DHCP-owned resolver configuration. Included under the radio lease lock;
 * no DNS request/network wait is performed here. No fallback server exists. */
static uint32_t lease_dns_address;
static int lease_dns_result=-ENODATA;
static void lease_dns_clear(void)
{
  lease_dns_address=0;lease_dns_result=-ENODATA;
#ifdef CONFIG_NETDB_DNSCLIENT
  /* Actual SDK API is void: revoke the snapshot before clearing resolver. */
  netlib_clear_dnsaddr();
#endif
}
static void lease_dns_apply(const struct in_addr *address)
{
  lease_dns_clear();
  if(!address->s_addr)return; /* DHCP option 6 absent; keep IP-only success. */
  const unsigned char *bytes=(const unsigned char *)&address->s_addr;
  if(!bytes[0]||bytes[0]==127||bytes[0]>=224){lease_dns_result=-EINVAL;return;}
#ifdef CONFIG_NETDB_DNSCLIENT
  if(netlib_set_ipv4dnsaddr(address)<0){lease_dns_result=errno?-errno:-EIO;return;}
  lease_dns_address=address->s_addr;lease_dns_result=0;
#else
  lease_dns_result=-ENOSYS;
#endif
}
