#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define OLLY_VERSION "1.0"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

typedef struct erow {
  int size;
  int rsize;
  int rcap;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  int cx, cy, rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  int rowcap;         /* allocated capacity of row[], grown by doubling */
  int dirty;
  int eol_crlf;       /* write \r\n line endings, as the loaded file used */
  int final_newline;  /* the file ended with a newline (so the save should) */
  erow *row;
  char *filename;
  char statusmsg[128];
  time_t statusmsg_time;
  struct termios orig_termios;
};

#define UC_INSERT 0
#define UC_DELETE 1
#define UC_JOIN 2
#define UC_SPLIT 3
#define UC_NEWROW 4

struct uchange {
  int kind;
  int row;
  int at;
  int back;
  char *text;
  size_t len;
  int cx0, cy0, cx1, cy1;
};

struct undoState {
  struct uchange **undo;
  int undo_n, undo_cap;
  struct uchange **redo;
  int redo_n, redo_cap;
  int locked;
};

static struct editorConfig E;
static struct undoState U;

struct abuf {
  char *b;
  int len;
  int cap;
};

#define ABUF_INIT {NULL, 0, 0}

void die(const char *s);
void editor_set_status_message(const char *fmt, ...);
void write_recovery_file(void);

/* Make room for `extra` more bytes. Capacity doubles, so a frame costs a few
 * reallocs in total; growing by exactly the appended length cost one realloc
 * per append, and the status bar appended its padding a byte at a time. */
static void ab_reserve(struct abuf *ab, int extra) {
  if (ab->b != NULL && ab->len + extra <= ab->cap) return;
  int cap = ab->cap ? ab->cap : 4096;
  while (cap < ab->len + extra) cap *= 2;
  char *new = realloc(ab->b, (size_t)cap);
  if (new == NULL) die("ab_append");
  ab->b = new;
  ab->cap = cap;
}

void ab_append(struct abuf *ab, const char *s, int len) {
  ab_reserve(ab, len);
  memcpy(&ab->b[ab->len], s, len);
  ab->len += len;
}

static void ab_append_fill(struct abuf *ab, char c, int n) {
  if (n <= 0) return;
  ab_reserve(ab, n);
  memset(&ab->b[ab->len], c, (size_t)n);
  ab->len += n;
}

void die(const char *s) {
  write_recovery_file();
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

/* Out of memory is fatal. Routing every allocation through these means it
 * exits through die(), restoring the terminal, rather than dereferencing
 * NULL a moment later. */
static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (p == NULL) die("malloc");
  return p;
}

static void *xrealloc(void *p, size_t n) {
  void *np = realloc(p, n);
  if (np == NULL) die("realloc");
  return np;
}

static char *xstrdup(const char *s) {
  char *p = strdup(s);
  if (p == NULL) die("strdup");
  return p;
}

/* --- UTF-8: the buffer holds raw bytes, but the cursor moves and deletes by
 * whole characters, so editing never splits a multibyte sequence and leaves
 * invalid bytes behind. (On-screen column width for wide or combining glyphs
 * is not yet modelled; a character is treated as one display column.) --- */

static int utf8_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* Bytes in the character starting at index i, validated: a lead byte whose
 * continuation bytes are missing or wrong is treated as a single byte. */
static int utf8_char_bytes(const char *s, int size, int i) {
  if (i < 0 || i >= size) return 1;
  unsigned char c = (unsigned char)s[i];
  int n;
  if (c >= 0xF0) n = 4;
  else if (c >= 0xE0) n = 3;
  else if (c >= 0xC0) n = 2;
  else return 1;
  if (i + n > size) return 1;
  int k;
  for (k = 1; k < n; k++)
    if (!utf8_is_cont((unsigned char)s[i + k])) return 1;
  return n;
}

/* Start index of the character before index i. */
static int utf8_prev(const char *s, int i) {
  if (i <= 0) return 0;
  int j = i - 1;
  while (j > 0 && utf8_is_cont((unsigned char)s[j])) j--;
  return j;
}

/* Decode the character at index i to a code point, and report its byte length.
 * An invalid or truncated sequence decodes as its single lead byte. */
static uint32_t utf8_decode(const char *s, int size, int i, int *nbytes) {
  int n = utf8_char_bytes(s, size, i);
  *nbytes = n;
  unsigned char c = (unsigned char)s[i];
  if (n == 1) return c;
  uint32_t cp = c & (0x7F >> n);
  int k;
  for (k = 1; k < n; k++)
    cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
  return cp;
}

/* --- display width: how many terminal columns a code point occupies. Built
 * in rather than via wcwidth(), so it does not depend on the process locale
 * and the editor stays self-contained. Zero for combining marks, two for the
 * East Asian wide and fullwidth ranges, one otherwise. The interval tables
 * follow Markus Kuhn's reference wcwidth (public domain). --- */

struct interval { uint32_t first, last; };

static int in_table(uint32_t cp, const struct interval *t, int n) {
  int lo = 0, hi = n - 1;
  if (cp < t[0].first || cp > t[hi].last) return 0;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (cp > t[mid].last) lo = mid + 1;
    else if (cp < t[mid].first) hi = mid - 1;
    else return 1;
  }
  return 0;
}

static int olly_wcwidth(uint32_t cp) {
  static const struct interval zero[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x07EB, 0x07F3},
    {0x0816, 0x0823}, {0x0900, 0x0903}, {0x093A, 0x094F}, {0x0951, 0x0957},
    {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20F0}, {0xFE20, 0xFE2F},
    {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF}
  };
  static const struct interval wide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x2753, 0x2755}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
    {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF},
    {0x20000, 0x3FFFD}
  };
  if (cp == 0) return 0;
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 1;  /* control: 1 cell */
  if (in_table(cp, zero, (int)(sizeof(zero) / sizeof(zero[0])))) return 0;
  if (in_table(cp, wide, (int)(sizeof(wide) / sizeof(wide[0])))) return 2;
  return 1;
}

/* Display columns of the character at index i (tabs are the caller's job). */
static int utf8_char_cols(const char *s, int size, int i, int *nbytes) {
  uint32_t cp = utf8_decode(s, size, i, nbytes);
  return olly_wcwidth(cp);
}

/* --- saved-content snapshot: the buffer is only "modified" when it really
 * differs from what is on disk, so undoing back to the saved state clears
 * the dirty flag and the quit warning. --- */

static erow *saved_rows;
static int saved_numrows;

void saved_snapshot_free(void) {
  int i;
  for (i = 0; i < saved_numrows; i++) {
    free(saved_rows[i].render);
    free(saved_rows[i].chars);
  }
  free(saved_rows);
  saved_rows = NULL;
  saved_numrows = 0;
}

void saved_snapshot_take(void) {
  int i;
  saved_snapshot_free();
  if (E.numrows == 0) return;
  saved_rows = malloc(sizeof(erow) * (size_t)E.numrows);
  if (saved_rows == NULL) return;
  saved_numrows = E.numrows;
  for (i = 0; i < saved_numrows; i++) {
    saved_rows[i].size = E.row[i].size;
    saved_rows[i].chars = malloc((size_t)E.row[i].size + 1);
    if (saved_rows[i].chars != NULL) {
      memcpy(saved_rows[i].chars, E.row[i].chars, (size_t)E.row[i].size);
      saved_rows[i].chars[E.row[i].size] = '\0';
    } else {
      saved_rows[i].size = 0;
    }
    saved_rows[i].rsize = 0;
    saved_rows[i].render = NULL;
    saved_rows[i].rcap = 0;
  }
}

int editor_matches_saved(void) {
  int i;
  if (E.numrows != saved_numrows) return 0;
  for (i = 0; i < E.numrows; i++) {
    if (saved_rows[i].chars == NULL) return 0;
    if (E.row[i].size != saved_rows[i].size) return 0;
    if (memcmp(E.row[i].chars, saved_rows[i].chars, (size_t)E.row[i].size) != 0)
      return 0;
  }
  return 1;
}

void editor_recompute_dirty(void) {
  E.dirty = editor_matches_saved() ? 0 : 1;
}

/* --- partial screen refresh: rendered rows are cached per screen line, and
 * only lines whose content actually changed are re-emitted (positioned
 * explicitly). Full redraws happen only on start, resize, Ctrl-L or after
 * the help overlay. --- */

static char **cache_lines;
static int *cache_lens;
static int *cache_caps;
static int cache_alloc;
static int force_full;
static int last_wsrows;
static int last_wscols;
static char *welcome_buf;
static int welcome_cap;

void cache_invalidate(void) {
  int i;
  for (i = 0; i < cache_alloc; i++) {
    free(cache_lines[i]);
    cache_lines[i] = NULL;
    cache_lens[i] = -1;
    cache_caps[i] = 0;
  }
}

void cache_ensure(int y) {
  int i, na;
  char **nl;
  int *nlens, *ncaps;
  if (y < cache_alloc) return;
  na = cache_alloc ? cache_alloc * 2 : 32;
  if (na <= y) na = y + 1;
  nl = realloc(cache_lines, sizeof(char *) * (size_t)na);
  nlens = realloc(cache_lens, sizeof(int) * (size_t)na);
  ncaps = realloc(cache_caps, sizeof(int) * (size_t)na);
  if (nl == NULL || nlens == NULL || ncaps == NULL) die("cache_ensure");
  cache_lines = nl;
  cache_lens = nlens;
  cache_caps = ncaps;
  for (i = cache_alloc; i < na; i++) {
    cache_lines[i] = NULL;
    cache_lens[i] = -1;
    cache_caps[i] = 0;
  }
  cache_alloc = na;
}

void cache_line_draw(struct abuf *ab, int y, const char *s, int len, int force) {
  char pos[32];
  char *nb;
  cache_ensure(y);
  if (!force && cache_lens[y] == len &&
      (len == 0 || memcmp(cache_lines[y], s, (size_t)len) == 0))
    return;
  {
    int plen = snprintf(pos, sizeof(pos), "\x1b[%d;1H", y + 1);
    ab_append(ab, pos, plen);
  }
  ab_append(ab, s, len);
  ab_append(ab, "\x1b[K", 3);
  if (cache_caps[y] < len + 1) {
    nb = realloc(cache_lines[y], (size_t)len + 1);
    if (nb == NULL) die("cache_line_draw");
    cache_lines[y] = nb;
    cache_caps[y] = len + 1;
  }
  if (len > 0) memcpy(cache_lines[y], s, (size_t)len);
  cache_lines[y][len] = '\0';
  cache_lens[y] = len;
}

/* --- robust full write: retries on EINTR and loops over partial writes --- */

int write_all(int fd, const void *p0, size_t n) {
  const char *p = p0;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (w == 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

/* --- crash recovery: when the editor is about to die with unsaved changes --
 * a fatal signal, or an out-of-memory exit through die() -- the buffer is
 * written to a side file so the work is not simply lost. The path is computed
 * ahead of time, in normal context, because the signal path may run only
 * async-signal-safe code. --- */

static char recovery_path[4096];

void build_recovery_path(void) {
  if (E.filename != NULL)
    snprintf(recovery_path, sizeof(recovery_path), "%s.olly-recover",
        E.filename);
  else
    snprintf(recovery_path, sizeof(recovery_path), "olly-recover.%ld",
        (long)getpid());
}

/* Best effort, and safe to call from a signal handler: only open/write/fsync/
 * close and plain memory reads. Reading E.row while it is mid-mutation can at
 * worst drop or duplicate a few bytes, which beats losing the whole buffer. */
void write_recovery_file(void) {
  if (!E.dirty || recovery_path[0] == '\0') return;
  int fd = open(recovery_path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd == -1) return;
  int j;
  for (j = 0; j < E.numrows; j++) {
    if (write_all(fd, E.row[j].chars, (size_t)E.row[j].size) == -1) break;
    if (write_all(fd, "\n", 1) == -1) break;
  }
  fsync(fd);
  close(fd);
}

/* Runs from atexit, where die() -- which calls exit() again -- would be
 * undefined behaviour. If the terminal is already gone there is nothing left
 * to restore anyway. */
void disable_raw_mode(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

/* A fatal signal bypasses atexit, so closing the terminal window or a plain
 * `kill` used to leave the terminal in raw mode, with no echo and no line
 * editing. Restore it, then re-raise so the process still dies by the signal.
 * Only async-signal-safe calls belong here. */
static void handle_fatal_signal(int sig) {
  static const char reset[] = "\x1b[?25h\x1b[2J\x1b[H";
  write_recovery_file();
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
  write(STDOUT_FILENO, reset, sizeof(reset) - 1);
  raise(sig);
}

static void install_signal_handlers(void) {
  static const int sigs[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
  struct sigaction sa;
  size_t i;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_fatal_signal;
  sigemptyset(&sa.sa_mask);
  /* The handler is reset to the default on entry, so the raise() in it ends
   * the process with the signal's normal action. */
  sa.sa_flags = SA_RESETHAND;
  for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
    sigaction(sigs[i], &sa, NULL);
}

void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disable_raw_mode);
  install_signal_handlers();

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

static int read_byte(unsigned char *c) {
  return read(STDIN_FILENO, c, 1) == 1;
}

/* Escape sequences are consumed in full, to their terminating byte, even when
 * the result is a key we do not map. A sequence that was only partly consumed
 * used to leave its tail in the input stream, where it arrived as ordinary
 * characters and was inserted into the buffer -- pressing Ctrl-Right, for
 * example, typed a literal "5C" into the file. */
int editor_read_key(void) {
  int nread;
  unsigned char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c != '\x1b') return c;

  unsigned char b;
  if (!read_byte(&b)) return '\x1b';

  if (b == '[') {
    /* CSI: parameter bytes 0x30-0x3f, intermediates 0x20-0x2f, then a final
     * byte in 0x40-0x7e. Modifiers ride in the parameters (Ctrl-Right is
     * "\x1b[1;5C"), so the final byte alone selects the key. */
    char params[24];
    size_t n = 0;
    unsigned char final;
    for (;;) {
      if (!read_byte(&b)) return '\x1b';
      if (b >= 0x40 && b <= 0x7e) { final = b; break; }
      if (n < sizeof(params) - 1) params[n++] = (char)b;
    }
    params[n] = '\0';

    switch (final) {
      case 'A': return ARROW_UP;
      case 'B': return ARROW_DOWN;
      case 'C': return ARROW_RIGHT;
      case 'D': return ARROW_LEFT;
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
      case '~':
        switch (atoi(params)) {
          case 1: return HOME_KEY;
          case 3: return DEL_KEY;
          case 4: return END_KEY;
          case 5: return PAGE_UP;
          case 6: return PAGE_DOWN;
          case 7: return HOME_KEY;
          case 8: return END_KEY;
        }
        break;
    }
    return '\x1b';
  }

  if (b == 'O') {
    /* SS3, sent for the cursor keys in application mode. */
    if (!read_byte(&b)) return '\x1b';
    switch (b) {
      case 'A': return ARROW_UP;
      case 'B': return ARROW_DOWN;
      case 'C': return ARROW_RIGHT;
      case 'D': return ARROW_LEFT;
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
    }
  }

  return '\x1b';
}

int get_cursor_position(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int get_window_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return get_cursor_position(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

void editor_update_row(erow *row) {
  int tabs = 0;
  int j;

  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  /* With no tabs, render is byte-for-byte chars, so alias it rather than keep
   * a second copy of every line. rcap > 0 marks an owned render buffer (always
   * a distinct allocation); rcap == 0 marks an alias to chars -- or a stale
   * pointer to a chars block a mutator just reallocated, which is why the
   * alias case must never free it. editor_free_row keys off rcap the same way. */
  if (tabs == 0) {
    if (row->rcap > 0) free(row->render);
    row->render = row->chars;
    row->rsize = row->size;
    row->rcap = 0;
    return;
  }

  if (row->rcap == 0) row->render = NULL;
  {
    int need = row->size + tabs * (KILO_TAB_STOP - 1) + 1;
    if (row->rcap < need) {
      row->render = xrealloc(row->render, (size_t)need);
      row->rcap = need;
    }
  }

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editor_insert_row(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  /* Allocate the new row's text before touching the array. The memmove below
   * duplicates each shifted row's pointers into the next slot for an instant;
   * if an allocation failed after that and died, editor_cleanup would free
   * the duplicated pointer twice. Allocating first means a failure here dies
   * with the array still consistent. */
  char *chars = xmalloc(len + 1);
  memcpy(chars, s, len);
  chars[len] = '\0';

  /* Double the capacity when full, so loading an n-line file costs O(log n)
   * reallocations rather than one per line. */
  if (E.numrows == E.rowcap) {
    E.rowcap = E.rowcap ? E.rowcap * 2 : 32;
    E.row = xrealloc(E.row, sizeof(erow) * (size_t)E.rowcap);
  }
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = chars;
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].rcap = 0;
  editor_update_row(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editor_free_row(erow *row) {
  if (row->rcap > 0) free(row->render);  /* rcap > 0 => render is owned */
  free(row->chars);
}

void editor_delete_row(int at) {
  if (at < 0 || at >= E.numrows) return;
  editor_free_row(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = xrealloc(row->chars, (size_t)row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editor_update_row(row);
  E.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len) {
  row->chars = xrealloc(row->chars, (size_t)row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editor_update_row(row);
}

void editor_row_delete_char(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editor_update_row(row);
  E.dirty++;
}

void editor_row_insert_n(erow *row, int at, const char *s, size_t n) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = xrealloc(row->chars, (size_t)row->size + n + 1);
  memmove(&row->chars[at + n], &row->chars[at], row->size - at + 1);
  memcpy(&row->chars[at], s, n);
  row->size += n;
  editor_update_row(row);
  E.dirty++;
}

void editor_row_delete_n(erow *row, int at, int n) {
  if (at < 0 || at + n > row->size) return;
  memmove(&row->chars[at], &row->chars[at + n], row->size - at - n);
  row->size -= n;
  row->chars[row->size] = '\0';
  editor_update_row(row);
  E.dirty++;
}

struct uchange *uc_new(int kind, int row, int at, const char *text, size_t len) {
  struct uchange *u = xmalloc(sizeof(struct uchange));
  u->kind = kind;
  u->row = row;
  u->at = at;
  u->back = 0;
  u->len = len;
  u->text = xmalloc(len + 1);
  if (len) memcpy(u->text, text, len);
  u->text[len] = '\0';
  u->cx0 = 0;
  u->cy0 = 0;
  u->cx1 = 0;
  u->cy1 = 0;
  return u;
}

void undo_push(struct uchange *u) {
  if (U.undo_n == U.undo_cap) {
    U.undo_cap = U.undo_cap ? U.undo_cap * 2 : 32;
    U.undo = xrealloc(U.undo, sizeof(struct uchange *) * (size_t)U.undo_cap);
  }
  U.undo[U.undo_n++] = u;
}

void redo_push(struct uchange *u) {
  if (U.redo_n == U.redo_cap) {
    U.redo_cap = U.redo_cap ? U.redo_cap * 2 : 32;
    U.redo = xrealloc(U.redo, sizeof(struct uchange *) * (size_t)U.redo_cap);
  }
  U.redo[U.redo_n++] = u;
}

void clear_redo(void) {
  int i;
  for (i = 0; i < U.redo_n; i++) {
    free(U.redo[i]->text);
    free(U.redo[i]);
  }
  U.redo_n = 0;
}

void editor_free_undo_redo(void) {
  int i;
  for (i = 0; i < U.undo_n; i++) {
    free(U.undo[i]->text);
    free(U.undo[i]);
  }
  for (i = 0; i < U.redo_n; i++) {
    free(U.redo[i]->text);
    free(U.redo[i]);
  }
  free(U.undo);
  free(U.redo);
  U.undo = NULL;
  U.redo = NULL;
  U.undo_n = 0;
  U.undo_cap = 0;
  U.redo_n = 0;
  U.redo_cap = 0;
  U.locked = 0;
}

void uc_free(struct uchange *u) {
  free(u->text);
  free(u);
}

void uc_do_insert(struct uchange *u) {
  editor_row_insert_n(&E.row[u->row], u->at, u->text, u->len);
}

void uc_do_delete(struct uchange *u) {
  editor_row_delete_n(&E.row[u->row], u->at, u->len);
}

void uc_do_join(struct uchange *u) {
  editor_row_append_string(&E.row[u->row], E.row[u->row + 1].chars,
      E.row[u->row + 1].size);
  editor_delete_row(u->row + 1);
}

void uc_do_split(struct uchange *u) {
  erow *row = &E.row[u->row];
  editor_insert_row(u->row + 1, row->chars + u->at, row->size - u->at);
  row = &E.row[u->row];
  row->size = u->at;
  row->chars[row->size] = '\0';
  editor_update_row(row);
}

void uc_do_newrow(struct uchange *u) {
  editor_insert_row(u->row, "", 0);
}

void uc_do_delrow(struct uchange *u) {
  editor_delete_row(u->row);
}

void uc_apply(struct uchange *u, int redo) {
  if (redo) {
    switch (u->kind) {
      case UC_INSERT: uc_do_insert(u); break;
      case UC_DELETE: uc_do_delete(u); break;
      case UC_JOIN: uc_do_join(u); break;
      case UC_SPLIT: uc_do_split(u); break;
      case UC_NEWROW: uc_do_newrow(u); break;
    }
    E.cx = u->cx1;
    E.cy = u->cy1;
  } else {
    switch (u->kind) {
      case UC_INSERT: uc_do_delete(u); break;
      case UC_DELETE: uc_do_insert(u); break;
      case UC_JOIN: uc_do_split(u); break;
      case UC_SPLIT: uc_do_join(u); break;
      case UC_NEWROW: uc_do_delrow(u); break;
    }
    E.cx = u->cx0;
    E.cy = u->cy0;
  }
  editor_recompute_dirty();
}

void editor_undo(void) {
  if (U.undo_n == 0) {
    editor_set_status_message("Nothing to undo");
    return;
  }
  struct uchange *u = U.undo[--U.undo_n];
  uc_apply(u, 0);
  redo_push(u);
  U.locked = 1;
  editor_set_status_message("Undo");
}

void editor_redo(void) {
  if (U.redo_n == 0) {
    editor_set_status_message("Nothing to redo");
    return;
  }
  struct uchange *u = U.redo[--U.redo_n];
  uc_apply(u, 1);
  undo_push(u);
  U.locked = 1;
  editor_set_status_message("Redo");
}

/* Display column of the cursor at byte offset cx: tabs advance to the next
 * tab stop, other characters advance by their display width (0 for combining
 * marks, 2 for wide glyphs). This is what positions the on-screen cursor and
 * drives horizontal scrolling. */
int editor_row_cx_to_rx(erow *row, int cx) {
  int rx = 0, j = 0;
  while (j < cx) {
    if (row->chars[j] == '\t') {
      rx += KILO_TAB_STOP - (rx % KILO_TAB_STOP);
      j++;
    } else {
      int nb;
      rx += utf8_char_cols(row->chars, row->size, j, &nb);
      j += nb;
    }
  }
  return rx;
}

/* Byte offset of the cursor within render (tabs expand to spaces there). The
 * search matches on render bytes, so it needs the cursor as a render byte
 * offset, not as a display column. */
static int editor_row_cx_to_rbyte(erow *row, int cx) {
  int rb = 0, j = 0;
  while (j < cx) {
    if (row->chars[j] == '\t') {
      rb += KILO_TAB_STOP - (rb % KILO_TAB_STOP);
      j++;
    } else {
      int nb = utf8_char_bytes(row->chars, row->size, j);
      rb += nb;
      j += nb;
    }
  }
  return rb;
}

/* Inverse of the above: map a render byte offset (where a search matched) back
 * to a byte offset in chars. */
static int editor_render_byte_to_cx(erow *row, int rbyte) {
  int rb = 0, cx = 0;
  while (cx < row->size) {
    int nb;
    if (row->chars[cx] == '\t') {
      rb += KILO_TAB_STOP - (rb % KILO_TAB_STOP);
      nb = 1;
    } else {
      nb = utf8_char_bytes(row->chars, row->size, cx);
      rb += nb;
    }
    if (rb > rbyte) return cx;
    cx += nb;
  }
  return row->size;
}

void editor_insert_char(int c) {
  int pre_cx = E.cx, pre_cy = E.cy;
  if (E.cy == E.numrows) {
    editor_insert_row(E.numrows, "", 0);
    struct uchange *ur = uc_new(UC_NEWROW, E.cy, 0, "", 0);
    ur->cx0 = pre_cx;
    ur->cy0 = pre_cy;
    ur->cx1 = pre_cx;
    ur->cy1 = pre_cy;
    undo_push(ur);
  }
  editor_row_insert_char(&E.row[E.cy], E.cx, c);
  E.cx++;

  struct uchange *top = U.undo_n ? U.undo[U.undo_n - 1] : NULL;
  if (!U.locked && top && top->kind == UC_INSERT && top->row == E.cy &&
      top->at + (int)top->len == E.cx - 1) {
    top->text = xrealloc(top->text, top->len + 2);
    top->text[top->len++] = c;
    top->text[top->len] = '\0';
    top->cx1 = E.cx;
    top->cy1 = E.cy;
  } else {
    char ch = c;
    struct uchange *u = uc_new(UC_INSERT, E.cy, E.cx - 1, &ch, 1);
    u->cx0 = pre_cx;
    u->cy0 = pre_cy;
    u->cx1 = E.cx;
    u->cy1 = E.cy;
    undo_push(u);
    clear_redo();
  }
  U.locked = 0;
}

void editor_insert_newline(void) {
  if (E.cx == 0) {
    struct uchange *u = uc_new(UC_NEWROW, E.cy, 0, "", 0);
    u->cx0 = 0;
    u->cy0 = E.cy;
    u->cx1 = 0;
    u->cy1 = E.cy + 1;
    undo_push(u);
    editor_insert_row(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    struct uchange *u =
        uc_new(UC_SPLIT, E.cy, E.cx, row->chars + E.cx, row->size - E.cx);
    u->cx0 = E.cx;
    u->cy0 = E.cy;
    u->cx1 = 0;
    u->cy1 = E.cy + 1;
    undo_push(u);
    editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editor_update_row(row);
  }
  E.cy++;
  E.cx = 0;
  clear_redo();
  U.locked = 0;
}

void editor_delete_char(void) {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  int pre_cx = E.cx, pre_cy = E.cy;
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    int at = utf8_prev(row->chars, E.cx);
    int nbytes = E.cx - at;
    struct uchange *top = U.undo_n ? U.undo[U.undo_n - 1] : NULL;
    /* A run of single-byte backspaces coalesces into one undo step. A
     * multibyte character is deleted as its own step -- simpler, and rare
     * enough that grouping it buys little. */
    if (nbytes == 1 && !U.locked && top && top->kind == UC_DELETE &&
        top->row == E.cy && top->at == E.cx && top->back) {
      char *nt = xmalloc(top->len + 2);
      nt[0] = row->chars[at];
      memcpy(nt + 1, top->text, top->len);
      nt[top->len + 1] = '\0';
      free(top->text);
      top->text = nt;
      top->len++;
      top->at--;
      top->cx1 = at;
      top->cy1 = E.cy;
    } else {
      struct uchange *u = uc_new(UC_DELETE, E.cy, at, row->chars + at,
                                 (size_t)nbytes);
      u->back = 1;
      u->cx0 = pre_cx;
      u->cy0 = pre_cy;
      u->cx1 = at;
      u->cy1 = E.cy;
      undo_push(u);
      clear_redo();
    }
    editor_row_delete_n(row, at, nbytes);
    E.cx = at;
  } else {
    erow *prev = &E.row[E.cy - 1];
    struct uchange *u =
        uc_new(UC_JOIN, E.cy - 1, prev->size, row->chars, row->size);
    u->cx0 = pre_cx;
    u->cy0 = pre_cy;
    u->cx1 = prev->size;
    u->cy1 = E.cy - 1;
    undo_push(u);
    clear_redo();
    E.cx = prev->size;
    editor_row_append_string(prev, row->chars, row->size);
    editor_delete_row(E.cy);
    E.cy--;
  }
  U.locked = 0;
}

void editor_del_char(void) {
  if (E.cy >= E.numrows) return;
  int pre_cx = E.cx, pre_cy = E.cy;
  erow *row = &E.row[E.cy];
  if (E.cx < row->size) {
    int nbytes = utf8_char_bytes(row->chars, row->size, E.cx);
    struct uchange *top = U.undo_n ? U.undo[U.undo_n - 1] : NULL;
    if (nbytes == 1 && !U.locked && top && top->kind == UC_DELETE &&
        top->row == E.cy && top->at == E.cx && !top->back) {
      size_t at = E.cx;
      size_t l = top->len;
      top->text = xrealloc(top->text, l + 2);
      top->text[l] = row->chars[at];
      top->text[l + 1] = '\0';
      top->len++;
      top->cx1 = E.cx;
      top->cy1 = E.cy;
    } else {
      struct uchange *u = uc_new(UC_DELETE, E.cy, E.cx, row->chars + E.cx,
                                 (size_t)nbytes);
      u->back = 0;
      u->cx0 = pre_cx;
      u->cy0 = pre_cy;
      u->cx1 = E.cx;
      u->cy1 = E.cy;
      undo_push(u);
      clear_redo();
    }
    editor_row_delete_n(row, E.cx, nbytes);
  } else if (E.cy < E.numrows - 1) {
    struct uchange *u =
        uc_new(UC_JOIN, E.cy, row->size, E.row[E.cy + 1].chars,
            E.row[E.cy + 1].size);
    u->cx0 = pre_cx;
    u->cy0 = pre_cy;
    u->cx1 = row->size;
    u->cy1 = E.cy;
    undo_push(u);
    clear_redo();
    editor_row_append_string(row, E.row[E.cy + 1].chars, E.row[E.cy + 1].size);
    editor_delete_row(E.cy + 1);
  }
  U.locked = 0;
}

void editor_move_cursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx = utf8_prev(row->chars, E.cx);
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx += utf8_char_bytes(row->chars, row->size, E.cx);
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) E.cy--;
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) E.cy++;
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  if (row) {
    if (E.cx > row->size) E.cx = row->size;
    /* A vertical move can land the byte offset inside a character on the new
     * line; step back to the character boundary. */
    while (E.cx > 0 && E.cx < row->size &&
           utf8_is_cont((unsigned char)row->chars[E.cx]))
      E.cx = utf8_prev(row->chars, E.cx);
  } else {
    E.cx = 0;
  }
}

void editor_scroll(void) {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) E.rowoff = E.cy;
  if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
  if (E.rx < E.coloff) E.coloff = E.rx;
  if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

/* Scratch buffer for the bytes of one drawn row (a horizontal slice, possibly
 * with a leading pad space where a wide glyph is cut by the scroll edge). */
static char *drawbuf;
static int drawbuf_cap;

static void drawbuf_ensure(int n) {
  if (drawbuf_cap >= n) return;
  drawbuf_cap = drawbuf_cap ? drawbuf_cap * 2 : 256;
  if (drawbuf_cap < n) drawbuf_cap = n;
  drawbuf = xrealloc(drawbuf, (size_t)drawbuf_cap);
}

/* Draw one text row, honouring display width: horizontal scroll (coloff) and
 * the screen width are measured in columns, so a slice starts and ends on
 * character boundaries. A wide glyph split by either edge is dropped and the
 * gap shown as a space, keeping every following column aligned. */
static void editor_draw_text_row(struct abuf *ab, int y, erow *row) {
  const char *r = row->render;
  int rs = row->rsize;
  int col = 0, i = 0;

  /* Skip whole characters that lie entirely left of the viewport. */
  while (i < rs && col < E.coloff) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    if (col + w > E.coloff) break;
    col += w;
    i += nb;
  }
  /* A wide glyph straddling the left edge is dropped; its visible right half
   * becomes that many leading spaces. */
  int left_pad = 0;
  if (i < rs && col < E.coloff) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    left_pad = (col + w) - E.coloff;
    i += nb;
  }

  int start = i;
  int cols = left_pad;
  while (i < rs && cols < E.screencols) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    if (cols + w > E.screencols) break;  /* would overflow the right edge */
    cols += w;
    i += nb;
  }

  int slice = i - start;
  drawbuf_ensure(left_pad + slice + 1);
  int p = 0;
  while (p < left_pad) drawbuf[p++] = ' ';
  if (slice > 0) memcpy(drawbuf + p, r + start, (size_t)slice);
  p += slice;
  cache_line_draw(ab, y, drawbuf, p, 0);
}

void editor_draw_rows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char text[96];
        int welcomelen = snprintf(text, sizeof(text),
                                  "Olly editor -- version %s", OLLY_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int pad = (E.screencols - welcomelen) / 2;
        if (pad > 0) {
          int need = pad + welcomelen + 1;
          if (welcome_cap < need) {
            char *nb = realloc(welcome_buf, (size_t)need);
            if (nb == NULL) {
              cache_line_draw(ab, y, text, welcomelen, 1);
              continue;
            }
            welcome_buf = nb;
            welcome_cap = need;
          }
          {
            int n = 0;
            welcome_buf[n++] = '~';
            while (n < pad) welcome_buf[n++] = ' ';
            memcpy(welcome_buf + n, text, (size_t)welcomelen);
            n += welcomelen;
            cache_line_draw(ab, y, welcome_buf, n, 1);
          }
        } else {
          cache_line_draw(ab, y, text, welcomelen, 1);
        }
      } else {
        cache_line_draw(ab, y, "~", 1, 0);
      }
    } else {
      editor_draw_text_row(ab, y, &E.row[filerow]);
    }
  }
}

void editor_draw_status_bar(struct abuf *ab) {
  char rp[32];
  int rplen = snprintf(rp, sizeof(rp), "\x1b[%d;1H", E.screenrows + 1);
  ab_append(ab, rp, rplen);
  ab_append(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
      E.filename ? E.filename : "[No Name]", E.numrows,
      E.dirty ? "(modified)" : "");
  /* Report the display column, so the number matches where the cursor sits:
   * E.rx already accounts for tab stops and character widths (a leading tab
   * puts the cursor at column 9, not column 2). A plain character count
   * disagreed with the cursor on any line with a tab or a wide character. */
  int rlen = snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d",
      E.cy + 1, E.rx + 1);
  if (len > E.screencols) len = E.screencols;
  ab_append(ab, status, len);
  /* Right-align the cursor position when it fits; otherwise pad to the edge. */
  if (E.screencols - len >= rlen) {
    ab_append_fill(ab, ' ', E.screencols - len - rlen);
    ab_append(ab, rstatus, rlen);
  } else {
    ab_append_fill(ab, ' ', E.screencols - len);
  }
  ab_append(ab, "\x1b[m", 3);
  ab_append(ab, "\x1b[K", 3);
}

void editor_draw_message_bar(struct abuf *ab) {
  char rp[32];
  int rplen = snprintf(rp, sizeof(rp), "\x1b[%d;1H", E.screenrows + 2);
  ab_append(ab, rp, rplen);
  ab_append(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    ab_append(ab, E.statusmsg, msglen);
}

void editor_refresh_screen(void) {
  int wsrows, wscols;
  /* On a failed size query, keep drawing at the last known size rather than
   * with whatever half-set values the query left behind. */
  if (get_window_size(&wsrows, &wscols) == -1) {
    wsrows = last_wsrows;
    wscols = last_wscols;
  }
  if (wsrows != last_wsrows || wscols != last_wscols) {
    last_wsrows = wsrows;
    last_wscols = wscols;
    force_full = 1;
  }
  E.screenrows = wsrows - 2;
  if (E.screenrows < 1) E.screenrows = 1;
  E.screencols = wscols;
  editor_scroll();

  struct abuf ab = ABUF_INIT;
  ab_append(&ab, "\x1b[?25l", 6);
  if (force_full) {
    cache_invalidate();
    ab_append(&ab, "\x1b[2J", 4);
    ab_append(&ab, "\x1b[H", 3);
    force_full = 0;
  }

  editor_draw_rows(&ab);
  editor_draw_status_bar(&ab);
  editor_draw_message_bar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
      (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  ab_append(&ab, buf, strlen(buf));

  ab_append(&ab, "\x1b[?25h", 6);
  write_all(STDOUT_FILENO, ab.b, (size_t)ab.len);
  free(ab.b);
}

void editor_set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = xmalloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editor_set_status_message(prompt, buf);
    editor_refresh_screen();

    int c = editor_read_key();

    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) {
        buflen = (size_t)utf8_prev(buf, (int)buflen);
        buf[buflen] = '\0';
      }
    } else if (c == '\x1b') {
      editor_set_status_message("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (callback) callback(buf, c);
      return buf;
    } else if (c >= 32 && c != 127 && c < 256) {
      /* Any byte that is not a control code or one of the >=1000 key codes,
       * so a pasted or typed UTF-8 character enters the prompt intact. */
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = xrealloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) callback(buf, c);
  }
}

static char *last_query = NULL;

static void editor_set_last_query(const char *q) {
  if (last_query) free(last_query);
  last_query = q ? xstrdup(q) : NULL;
}

/* dir = 1 search forward, -1 search backward. exclude_current causes the
 * search to skip a match exactly at the cursor, and wraps around the file. */
static int editor_search_string(const char *query, int qlen, int dir,
    int exclude_current) {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  int n = E.numrows;
  int k;
  if (n == 0) return 0;
  /* The cursor may sit on the phantom line past the last row; anchor the
   * sweep to a real row in that case. */
  int cur = (E.cy >= n) ? 0 : E.cy;
  /* The cursor's position as a render byte offset -- render is what strncasecmp
   * scans, and the cursor's display column (E.rx) is no longer the same thing
   * once a line holds wide or combining characters. */
  int cur_rb = editor_row_cx_to_rbyte(&E.row[cur], E.cx);
  /* k runs to n inclusive, so the starting row is visited a second time at
   * the end of the wrap. Only then is it scanned from its beginning, which
   * is what makes a match earlier on the cursor's own line reachable. */
  for (k = 0; k <= n; k++) {
    int i = dir > 0 ? (cur + k) % n : ((cur - k) % n + n) % n;
    erow *row = &E.row[i];
    int maxstart = row->rsize - qlen;
    int on_start_row = (i == E.cy) && (k == 0);
    if (dir > 0) {
      int start = on_start_row ? (exclude_current ? cur_rb + 1 : cur_rb) : 0;
      int col;
      for (col = start; col <= maxstart; col++) {
        if (strncasecmp(row->render + col, query, qlen) == 0) {
          E.cy = i;
          E.cx = editor_render_byte_to_cx(row, col);
          return 1;
        }
      }
    } else {
      int start = on_start_row ? (exclude_current ? cur_rb - 1 : cur_rb)
                               : maxstart;
      if (start > maxstart) start = maxstart;
      int col;
      for (col = start; col >= 0; col--) {
        if (strncasecmp(row->render + col, query, qlen) == 0) {
          E.cy = i;
          E.cx = editor_render_byte_to_cx(row, col);
          return 1;
        }
      }
    }
  }

  E.cx = saved_cx;
  E.cy = saved_cy;
  E.coloff = saved_coloff;
  E.rowoff = saved_rowoff;
  return 0;
}

void editor_find(void) {
  char *query = editor_prompt("Search: %s (ESC to cancel, Enter to find)", NULL);
  if (query == NULL) return;

  if (query[0] != '\0') {
    editor_set_last_query(query);
  } else if (last_query == NULL) {
    editor_set_status_message("No previous search. Type a term to search");
    free(query);
    return;
  }

  const char *term = query[0] != '\0' ? query : last_query;
  int excl = (query[0] == '\0');
  if (editor_search_string(term, strlen(term), 1, excl))
    editor_set_status_message("Found");
  else
    editor_set_status_message("Not found. Press Ctrl-N to search again");
  free(query);
}

void editor_find_next(void) {
  if (last_query == NULL) {
    editor_set_status_message("No previous search. Press Ctrl-F to search");
    return;
  }
  if (editor_search_string(last_query, strlen(last_query), 1, 1))
    editor_set_status_message("Found (next)");
  else
    editor_set_status_message("Not found. Press Ctrl-P to search backward");
}

void editor_find_prev(void) {
  if (last_query == NULL) {
    editor_set_status_message("No previous search. Press Ctrl-F to search");
    return;
  }
  if (editor_search_string(last_query, strlen(last_query), -1, 1))
    editor_set_status_message("Found (previous)");
  else
    editor_set_status_message("Not found. Press Ctrl-N to search forward");
}

/* Write every row to an already-open descriptor, ending each with the line
 * ending the file was loaded with (LF, or CRLF), and omitting the final one
 * if the file had no trailing newline. Rows are gathered into a 64 KB buffer
 * and written a chunk at a time: writing each row and then its newline cost
 * two syscalls per line, 100,000 of them for a 50,000-line file. */
static int editor_write_rows(int fd, size_t *total) {
  static char buf[65536];
  const char *eol = E.eol_crlf ? "\r\n" : "\n";
  size_t eollen = E.eol_crlf ? 2 : 1;
  size_t used = 0, n = 0;
  int j;
  for (j = 0; j < E.numrows; j++) {
    size_t len = (size_t)E.row[j].size;
    size_t elen = (j == E.numrows - 1 && !E.final_newline) ? 0 : eollen;
    if (used + len + elen > sizeof(buf)) {
      if (write_all(fd, buf, used) == -1) return -1;
      used = 0;
    }
    if (len + elen > sizeof(buf)) {
      /* Longer than the whole buffer: it was flushed above, so write the
       * row straight through. */
      if (write_all(fd, E.row[j].chars, len) == -1) return -1;
      if (elen && write_all(fd, eol, elen) == -1) return -1;
    } else {
      memcpy(buf + used, E.row[j].chars, len);
      if (elen) memcpy(buf + used + len, eol, elen);
      used += len + elen;
    }
    n += len + elen;
  }
  if (used > 0 && write_all(fd, buf, used) == -1) return -1;
  *total = n;
  return 0;
}

/* A rename is only durable once the directory entry itself is on disk. */
static void fsync_parent_dir(const char *path) {
  char *copy = strdup(path);
  if (copy == NULL) return;

  char *slash = strrchr(copy, '/');
  const char *dir;
  if (slash == NULL) dir = ".";
  else if (slash == copy) dir = "/";
  else { *slash = '\0'; dir = copy; }

  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd != -1) {
    fsync(dfd);
    close(dfd);
  }
  free(copy);
}

/* The mode a newly created file should end up with, honouring the umask. */
static mode_t default_file_mode(void) {
  mode_t um = umask(0);
  umask(um);
  return 0666 & ~um;
}

void editor_save(void) {
  if (E.filename == NULL) {
    char *name = editor_prompt("Save as: %s (ESC to cancel)", NULL);
    if (name == NULL) {
      editor_set_status_message("Save aborted");
      return;
    }
    /* An empty answer must not become the file name: it can never be saved
     * to, and because it is non-NULL it would suppress this prompt forever,
     * stranding the buffer with no way to write it out. */
    if (name[0] == '\0') {
      free(name);
      editor_set_status_message("Save aborted: no file name given");
      return;
    }
    E.filename = name;
    build_recovery_path();
  }

  /* If the name is a symlink, write past it to the real target so atomic
   * rename replaces the file contents rather than the link itself. */
  const char *target = E.filename;
  char *resolved = NULL;
  struct stat st;
  if (lstat(E.filename, &st) == 0 && S_ISLNK(st.st_mode)) {
    resolved = realpath(E.filename, NULL);
    if (resolved == NULL) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      return;
    }
    target = resolved;
  }

  int target_exists = (stat(target, &st) == 0);
  mode_t mode = target_exists ? (st.st_mode & 07777) : default_file_mode();
  uid_t owner_uid = target_exists ? st.st_uid : (uid_t)-1;
  gid_t owner_gid = target_exists ? st.st_gid : (gid_t)-1;

  /* Write to a unique temp file next to the target, then rename over it, so
   * an interrupted save never leaves the real file truncated or empty.
   * mkstemp creates the file exclusively under a name nobody can predict:
   * a fixed "<target>.tmp" both clobbered any real file of that name and
   * let a local attacker win the race between unlink and open, redirecting
   * the write through a symlink of their choosing. */
  size_t tlen = strlen(target);
  char *tmp = malloc(tlen + sizeof(".XXXXXX"));
  if (tmp == NULL) {
    editor_set_status_message("Save failed: out of memory");
    free(resolved);
    return;
  }
  memcpy(tmp, target, tlen);
  memcpy(tmp + tlen, ".XXXXXX", sizeof(".XXXXXX"));

  size_t total = 0;
  int fd = mkstemp(tmp);

  if (fd == -1) {
    /* No room to create a sibling temp file -- typically a writable file in
     * a read-only directory. Fall back to rewriting the file in place, which
     * still saves the user's work, but say so: this write is not atomic. */
    int direct = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                              O_NOFOLLOW, mode);
    if (direct == -1) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      free(tmp);
      free(resolved);
      return;
    }
    int dok = (editor_write_rows(direct, &total) == 0);
    if (dok && fsync(direct) == -1) dok = 0;
    if (close(direct) == -1) dok = 0;
    free(tmp);
    free(resolved);
    if (!dok) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      return;
    }
    editor_set_status_message("%zu bytes written (in place, not atomic)",
        total);
    E.dirty = 0;
    saved_snapshot_take();
    if (recovery_path[0] != '\0') unlink(recovery_path);
    return;
  }

  int ok = 1;
  /* Not fatal if it fails: the file is saved either way, only its mode is off.
   * mkstemp creates at 0600, so the content is never briefly world-readable. */
  (void)fchmod(fd, mode);
  /* Keep the file's owner across the replace when we have the privilege to;
   * an unprivileged process simply cannot, and the save still succeeds. */
  if (target_exists) (void)fchown(fd, owner_uid, owner_gid);

  if (editor_write_rows(fd, &total) == -1) {
    ok = 0;
    editor_set_status_message("Save failed: %s", strerror(errno));
  }

  if (ok && fsync(fd) == -1) {
    ok = 0;
    editor_set_status_message("Save failed: fsync");
  }
  if (close(fd) == -1 && ok) {
    ok = 0;
    editor_set_status_message("Save failed: %s", strerror(errno));
  }

  if (ok && rename(tmp, target) == -1) {
    ok = 0;
    editor_set_status_message("Save failed: %s", strerror(errno));
  }

  if (ok) fsync_parent_dir(target);
  else unlink(tmp);

  free(tmp);
  free(resolved);

  if (!ok) return;

  editor_set_status_message("%zu bytes written", total);
  E.dirty = 0;
  saved_snapshot_take();
  if (recovery_path[0] != '\0') unlink(recovery_path);
}

void editor_open(char *filename) {
  free(E.filename);
  E.filename = xstrdup(filename);
  build_recovery_path();

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    if (errno == ENOENT) return;
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  int any = 0, saw_cr = 0, had_final_nl = 1;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    any = 1;
    /* Whether this line -- so, once the loop ends, the last line -- carried a
     * trailing newline, and whether the endings were CRLF. Both are preserved
     * on save instead of being silently rewritten. */
    had_final_nl = (linelen > 0 && line[linelen - 1] == '\n');
    int had_cr = 0;
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
        line[linelen - 1] == '\r')) {
      if (line[linelen - 1] == '\r') had_cr = 1;
      linelen--;
    }
    if (had_cr) saw_cr = 1;
    if (linelen > INT_MAX - 1) die("line too long");
    editor_insert_row(E.numrows, line, (size_t)linelen);
  }
  /* getline returns -1 for a read error as well as at end of file. Taking an
   * error for the end would present a partial buffer as the whole file, and
   * the next save would truncate the file on disk. This also refuses a
   * directory, which used to open as an empty buffer that could never be
   * saved. */
  if (!feof(fp)) die(filename);
  if (any) {
    E.eol_crlf = saw_cr;
    E.final_newline = had_final_nl;
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
  saved_snapshot_take();
}

void editor_help(void) {
  const char *lines[] = {
    "Olly - Help",
    "",
    "Movement:   Arrow keys, Page Up/Page Down, Home/End",
    "Editing:    Type characters, Enter inserts a new line",
    "            Backspace/Delete remove characters before/at the cursor",
    "",
    "Commands:",
    "  Ctrl-S       Save the file",
    "  Ctrl-F       Search forward (case-insensitive)",
    "  Ctrl-N       Find the next instance of the search",
    "  Ctrl-P       Find the previous instance of the search",
    "  Ctrl-Z       Undo the last change",
    "  Ctrl-Y       Redo an undone change",
    "  Ctrl-Q       Quit (asks several times if there are unsaved changes)",
    "  Ctrl-?       Show this help screen",
    "",
    "Press any key to return to the editor"
  };
  int nlines = sizeof(lines) / sizeof(lines[0]);
  int start = (E.screenrows - nlines) / 2;
  if (start < 0) start = 0;

  struct abuf ab = ABUF_INIT;
  ab_append(&ab, "\x1b[2J", 4);
  ab_append(&ab, "\x1b[?25l", 6);

  char rowpos[32];
  int i;
  for (i = 0; i < nlines; i++) {
    snprintf(rowpos, sizeof(rowpos), "\x1b[%d;1H", start + i + 1);
    ab_append(&ab, rowpos, strlen(rowpos));
    int len = strlen(lines[i]);
    if (len > E.screencols) len = E.screencols;
    ab_append(&ab, lines[i], len);
    ab_append(&ab, "\x1b[K", 3);
  }

  write_all(STDOUT_FILENO, ab.b, (size_t)ab.len);
  free(ab.b);

  editor_read_key();
  force_full = 1;
}

void editor_process_keypress(void) {
  static int quit_times = KILO_QUIT_TIMES;
  int c = editor_read_key();

  switch (c) {
    case '\r':
      editor_insert_newline();
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editor_set_status_message(
            "WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.",
            quit_times);
        quit_times--;
        return;
      }
      write_all(STDOUT_FILENO, "\x1b[2J", 4);
      write_all(STDOUT_FILENO, "\x1b[H", 3);
      if (recovery_path[0] != '\0') unlink(recovery_path);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editor_save();
      break;

    case CTRL_KEY('z'):
      editor_undo();
      break;

    case CTRL_KEY('y'):
      editor_redo();
      break;

    case CTRL_KEY('f'):
      editor_find();
      break;

    case CTRL_KEY('n'):
      editor_find_next();
      break;

    case CTRL_KEY('p'):
      editor_find_prev();
      break;

    case CTRL_KEY('?'):
      editor_help();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY)
        editor_del_char();
      else
        editor_delete_char();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = E.screenrows;
        while (times--)
          editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;

    case CTRL_KEY('l'):
      force_full = 1;
      break;

    case '\x1b':
      break;

    default:
      /* Tab is a control character, so testing iscntrl() alone threw it
       * away and a tab could not be typed at all. Key codes of 1000 and up
       * are outside iscntrl()'s domain and must not reach it. */
      if (c == '\t' || (c < 256 && !iscntrl(c))) editor_insert_char(c);
      break;
  }

  quit_times = KILO_QUIT_TIMES;
}

void editor_cleanup(void) {
  int i;
  for (i = 0; i < E.numrows; i++) editor_free_row(&E.row[i]);
  free(E.row);
  E.row = NULL;
  E.numrows = 0;
  E.rowcap = 0;
  saved_snapshot_free();
  cache_invalidate();
  free(cache_lines);
  free(cache_lens);
  free(cache_caps);
  cache_lines = NULL;
  cache_lens = NULL;
  cache_caps = NULL;
  cache_alloc = 0;
  free(welcome_buf);
  welcome_buf = NULL;
  welcome_cap = 0;
  free(drawbuf);
  drawbuf = NULL;
  drawbuf_cap = 0;
  free(last_query);
  last_query = NULL;
  editor_free_undo_redo();
  free(E.filename);
  E.filename = NULL;
}

void init_editor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.rowcap = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.dirty = 0;
  E.eol_crlf = 0;
  E.final_newline = 1;
  build_recovery_path();

  if (get_window_size(&E.screenrows, &E.screencols) == -1)
    die("get_window_size");
  E.screenrows -= 2;
  if (E.screenrows < 1) E.screenrows = 1;
  last_wsrows = E.screenrows + 2;
  last_wscols = E.screencols;
  force_full = 1;
}

int main(int argc, char *argv[]) {
  enable_raw_mode();
  atexit(editor_cleanup);
  init_editor();
  if (argc >= 2) editor_open(argv[1]);

  editor_set_status_message(
      "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find | Ctrl-N = next | Ctrl-? = help");

  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }

  return 0;
}