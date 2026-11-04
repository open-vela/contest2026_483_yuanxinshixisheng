/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <sys/mount.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if !defined(CONFIG_FAT_FORCE_READONLY) || !defined(CONFIG_USBHOST_MSC_READONLY) || !defined(CONFIG_FAT_FORCE_INDIRECT)
#error explicit FAT and MSC readonly plus indirect reads required
#endif
static mutex_t owner = NXMUTEX_INITIALIZER;
static int attempted, mounted, directory_uncertain;
static int fail(void) { return errno ? -errno : -EIO; }
static int listing(void)
{
  DIR *d;
  struct dirent *e;
  unsigned n = 0;
  int rc = 0;
  if (!mounted || directory_uncertain) return -EBUSY;
  d = opendir("/mnt/k7model");
  if (!d) return fail();
  for (;;) {
    size_t i, len = 0;
    if (n == 64) { rc = -E2BIG; break; }
    errno = 0;
    e = readdir(d);
    if (!e) { if (errno) rc = fail(); break; }
    ++n;
    while (len < sizeof e->d_name && e->d_name[len]) ++len;
    if (len == sizeof e->d_name) { rc = -EILSEQ; break; }
    /* Hex prevents terminal control/newline injection; at most 64 name bytes. */
    printf("K7FAT entry=%u name_hex=", n);
    for (i = 0; i < len && i < 64; ++i)
      printf("%02x", (unsigned char)e->d_name[i]);
    printf(" truncated=%u\n", (unsigned)(len > 64));
  }
  errno = 0;
  if (closedir(d) < 0) {
    directory_uncertain = 1;
    if (!rc) rc = fail();
  }
  return rc;
}
int main(int argc, char **argv)
{
  int rc;
  if (argc != 2 || (strcmp(argv[1], "mount") &&
      strcmp(argv[1], "list") && strcmp(argv[1], "unmount"))) {
    puts("usage: k7fat mount|list|unmount (fixed USB FAT readonly)"); return 1;
  }
  rc = nxmutex_trylock(&owner);
  if (rc < 0) return 1;
  if (!strcmp(argv[1], "mount")) {
    if (attempted) rc = -EALREADY;
    else {
      attempted = 1; errno = 0;
      rc = mount("/dev/sda", "/mnt/k7model", "vfat", MS_RDONLY, NULL);
      if (rc < 0) rc = fail();
      else { mounted = 1; rc = 0; }
    }
  } else if (!strcmp(argv[1], "list")) rc = listing();
  else if (!mounted) rc = -EINVAL;
  else {
    errno = 0;
    rc = umount2("/mnt/k7model", 0);
    if (rc < 0) rc = fail();
    else { mounted = 0; directory_uncertain = 0; rc = 0; }
  }
  printf("K7FAT command=%s result=%d mounted=%d attempted=%d dir_uncertain=%d\n",
         argv[1], rc, mounted, attempted, directory_uncertain);
  nxmutex_unlock(&owner);
  return rc ? 1 : 0;
}
