#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static ssize_t (*real_write)(int, const void *, size_t);

__attribute__((constructor)) static void failwrite_init(void) {
  const char *s = getenv("OLLY_FAIL_WRITE");
  failures = s ? atoi(s) : 0;
  real_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
}

ssize_t write(int fd, const void *buf, size_t count) {
  if (fd > STDERR_FILENO && failures > 0) {
    failures--;
    errno = (getenv("OLLY_FAIL_WRITE_ERR") != NULL &&
             strcmp(getenv("OLLY_FAIL_WRITE_ERR"), "EINTR") == 0)
        ? EINTR : ENOSPC;
    return -1;
  }
  return real_write(fd, buf, count);
}
