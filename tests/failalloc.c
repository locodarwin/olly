/* LD_PRELOAD shim for tests/oom_sweep.py: makes exactly one allocation fail.
 *
 * OLLY_FAIL_AT=N fails the Nth malloc, calloc or realloc call (counting from
 * 1; 0 or unset fails nothing). OLLY_FAIL_HIT names a file that is created
 * when that call happens, so the sweep can tell when N has run past the last
 * allocation the session makes. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);

static long fail_at;
static long calls;
static const char *hit_path;

__attribute__((constructor)) static void failalloc_init(void) {
  const char *s = getenv("OLLY_FAIL_AT");
  hit_path = getenv("OLLY_FAIL_HIT");
  if (s) fail_at = atol(s);
}

static int should_fail(void) {
  if (fail_at <= 0 || ++calls != fail_at) return 0;
  if (hit_path) {
    int fd = open(hit_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd != -1) close(fd);
  }
  errno = ENOMEM;
  return 1;
}

void *malloc(size_t n) {
  return should_fail() ? NULL : __libc_malloc(n);
}

void *calloc(size_t n, size_t size) {
  return should_fail() ? NULL : __libc_calloc(n, size);
}

void *realloc(void *p, size_t n) {
  return should_fail() ? NULL : __libc_realloc(p, n);
}
