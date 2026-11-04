/* SPDX-License-Identifier: Apache-2.0 */
/* Explicit USB-only, read-only initial media diagnostic. No mount or write. */
#include <nuttx/config.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>
#include <sys/mount.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(CONFIG_USBHOST_MSC) || !defined(CONFIG_USBHOST_MSC_READONLY)
#error k7storage requires the checked read-only MSC profile
#endif

extern int rk3576_usbhost_initialize(void);
static mutex_t owner = NXMUTEX_INITIALIZER;
static _Alignas(64) unsigned char first[4096];
static _Alignas(64) unsigned char second[4096];

static uint32_t crc32(const unsigned char *p, size_t n)
{
  uint32_t crc = UINT32_MAX;
  while (n--)
    {
      crc ^= *p++;
      for (unsigned i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
  return ~crc;
}

static int list_usb_nodes(void)
{
  DIR *dir = opendir("/dev");
  struct dirent *entry;
  unsigned count = 0;
  if (!dir) return -errno;
  int rc = 0;
  unsigned visited = 0;
  for (;;)
    {
      /* Conservative bound: reaching 256 entries fails, even if EOF is next. */
      if (visited == 256) { rc = -E2BIG; break; }
      errno = 0;
      entry = readdir(dir);
      if (!entry) { if (errno) rc = -errno; break; }
      ++visited;
      if (strlen(entry->d_name) == 3 && entry->d_name[0] == 's' &&
          entry->d_name[1] == 'd' && entry->d_name[2] >= 'a' &&
          entry->d_name[2] <= 'z')
        {
          printf("USB_STORAGE node=/dev/%s\n", entry->d_name);
          ++count;
        }
    }
  if (closedir(dir) < 0 && rc == 0) rc = errno ? -errno : -EIO;
  printf("USB_STORAGE nodes=%u\n", count);
  return rc;
}

static int read_first_sector(const char *path)
{
  struct inode *inode = NULL;
  struct geometry geometry;
  ssize_t received;
  int rc;
  if (strlen(path) != 8 || memcmp(path, "/dev/sd", 7) ||
      path[7] < 'a' || path[7] > 'z') return -EINVAL;
  rc = open_blockdriver(path, MS_RDONLY, &inode);
  if (rc < 0) return rc;
  if (!inode->u.i_bops || !inode->u.i_bops->geometry || !inode->u.i_bops->read)
    { rc = -ENOSYS; goto done; }
  memset(&geometry, 0, sizeof(geometry));
  rc = inode->u.i_bops->geometry(inode, &geometry);
  if (rc < 0) goto done;
  if (!geometry.geo_available || geometry.geo_writeenabled ||
      geometry.geo_nsectors <= 0 ||
      (geometry.geo_sectorsize != 512 && geometry.geo_sectorsize != 1024 &&
       geometry.geo_sectorsize != 2048 && geometry.geo_sectorsize != 4096))
    { rc = -EIO; goto done; }
  memset(first, 0, sizeof(first));
  memset(second, 0, sizeof(second));
  received = inode->u.i_bops->read(inode, first, 0, 1);
  if (received != 1) { rc = received < 0 ? (int)received : -EIO; goto done; }
  received = inode->u.i_bops->read(inode, second, 0, 1);
  if (received != 1) { rc = received < 0 ? (int)received : -EIO; goto done; }
  if (memcmp(first, second, geometry.geo_sectorsize)) { rc = -EILSEQ; goto done; }
  printf("USB_STORAGE read node=%s sector_size=%u sectors=%" PRIu64
         " readonly=1 lba=0 reads=2 crc32=%08" PRIx32
         " repeated=1 boot_signature=%u exfat_oem=%u\n",
         path, (unsigned)geometry.geo_sectorsize, (uint64_t)geometry.geo_nsectors,
         crc32(first, geometry.geo_sectorsize),
         (unsigned)(first[510] == 0x55 && first[511] == 0xaa),
         (unsigned)(memcmp(first + 3, "EXFAT   ", 8) == 0));
  rc = 0;
done:
  {
    int close_rc = close_blockdriver(inode);
    if (rc == 0 && close_rc < 0) rc = close_rc;
  }
  return rc;
}

int main(int argc, char **argv)
{
  int rc;
  if (argc < 2 || argc > 3 ||
      (argc == 2 && strcmp(argv[1], "start") && strcmp(argv[1], "list")) ||
      (argc == 3 && strcmp(argv[1], "read")))
    { puts("usage: k7storage start|list|read /dev/sdX (USB read-only, no mount)"); return 1; }
  rc = nxmutex_trylock(&owner);
  if (rc < 0) { puts("USB_STORAGE busy"); return 1; }
  if (!strcmp(argv[1], "start")) rc = rk3576_usbhost_initialize();
  else if (!strcmp(argv[1], "list")) rc = list_usb_nodes();
  else rc = read_first_sector(argv[2]);
  printf("USB_STORAGE command=%s result=%d mount_attempted=0\n", argv[1], rc);
  nxmutex_unlock(&owner);
  return rc ? 1 : 0;
}
