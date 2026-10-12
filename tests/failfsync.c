#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;
static int (*real_fsync)(int);

__attribute__((constructor)) static void failfsync_init(void) {
  const char *s = getenv("OLLY_FAIL_FSYNC");
  failures = s ? atoi(s) : 0;
  real_fsync = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
}

int fsync(int fd) {
  if (failures > 0) {
    failures--;
    errno = EIO;
    return -1;
  }
  return real_fsync(fd);
}
