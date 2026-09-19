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
#define OLLY_VERSION "1.1"

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
  PAGE_DOWN,
  WINCH_KEY  /* not a real keystroke -- a SIGWINCH woke the read loop */
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
  int sel_active;     /* a selection is in progress (anchor + cursor) */
  int sx, sy;         /* selection anchor; the cursor (cx,cy) is the far end */
  char *clipboard;    /* in-editor clipboard for Ctrl-C/X/V; kept across files */
  int clipsize;       /* bytes held in clipboard (not necessarily NUL-terminated) */
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
  int group;  /* 0 = standalone; otherwise shared id of a multi-edit group */
};

struct undoState {
  struct uchange **undo;
  int undo_n, undo_cap;
  struct uchange **redo;
  int redo_n, redo_cap;
  int locked;
  int active_group;  /* group id new uchanges are stamped with, 0 = none */
  int group_seq;      /* last group id issued */
};

static struct editorConfig E;
static struct undoState U;

struct abuf {
  char *b;
  int len;
  int cap;
};

#define ABUF_INIT {NULL, 0, 0}

_Noreturn void die(const char *s);
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

_Noreturn void die(const char *s) {
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

/* mtime of the file on disk as of the last open or successful save, used to
 * warn before a save would silently clobber a change made by something
 * else. No baseline exists for a buffer that isn't backed by an existing
 * file yet (a brand-new buffer, or one just given a name via Save As). */
static struct timespec on_disk_mtime;
static int have_disk_mtime;

static int mtime_eq(struct timespec a, struct timespec b) {
  return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

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

/* Set from handle_winch, read from editor_read_key's poll loop. Only a flag
 * write, so it is safe from a signal handler; the actual resize (which
 * reallocates the render cache) happens back in normal context. */
static volatile sig_atomic_t got_winch = 0;

static void handle_winch(int sig) {
  (void)sig;
  got_winch = 1;
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

  /* SA_RESTART: a SIGWINCH landing mid-read transparently restarts the read
   * instead of failing it with EINTR, which editor_read_key would otherwise
   * treat as a fatal error. */
  struct sigaction wsa;
  memset(&wsa, 0, sizeof(wsa));
  wsa.sa_handler = handle_winch;
  sigemptyset(&wsa.sa_mask);
  wsa.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &wsa, NULL);
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

  /* Turn bracketed paste off even if the terminal or a parent shell left it
   * on. Olly does not parse the "\x1b[200~...\x1b[201~" wrappers, so with the
   * mode on a paste would arrive as a stray DEL key (the "\x1b[...~" prefix)
   * plus literal text; off, a paste is just literal keystrokes and inserts
   * cleanly. */
  write_all(STDOUT_FILENO, "\x1b[?2004l", 8);
}

static int read_byte(unsigned char *c) {
  return read(STDIN_FILENO, c, 1) == 1;
}

/* Escape sequences are consumed in full, to their terminating byte, even when
 * the result is a key we do not map. A sequence that was only partly consumed
 * used to leave its tail in the input stream, where it arrived as ordinary
 * characters and was inserted into the buffer -- pressing Ctrl-Right, for
 * example, typed a literal "5C" into the file. */
/* Set by editor_read_key when the key just returned arrived with the Shift
 * modifier (CSI form, e.g. Shift+Right is "\x1b[1;2C"). Read by
 * editor_process_keypress to extend a selection; smuggled out-of-band like
 * WINCH_KEY so the key enum stays a plain int. Reset on every call. */
static int last_key_shift = 0;

int editor_read_key(void) {
  int nread;
  unsigned char c;
  last_key_shift = 0;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
    /* VMIN=0/VTIME=1 already wakes this loop roughly every 100ms even with
     * no key pressed; piggyback on that wakeup to notice a resize instead of
     * waiting for the next real keystroke to redraw at the new size. */
    if (got_winch) {
      got_winch = 0;
      return WINCH_KEY;
    }
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

    /* The modifier, if any, rides after the ';' in the parameters ("\x1b[1;2C"
     * = Shift+Right). The value is one more than a bitfield (shift=1, alt=2,
     * ctrl=4), so a set low bit means Shift was held. Only consumed as a
     * side signal; the returned key is still selected by `final`, and the
     * sequence is always read to its terminator so nothing leaks to the
     * buffer regardless. */
    char *semi = strchr(params, ';');
    if (semi != NULL) {
      int mod = atoi(semi + 1);
      if (mod >= 2) last_key_shift = (mod - 1) & 0x01;
    }

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
  u->group = U.active_group;
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

/* Multi-edit operations (replace-all) push several uchanges that must undo
 * and redo as one step. Every uchange created between begin/end is stamped
 * with the same group id (in uc_new); editor_undo/editor_redo then pop the
 * whole run in one call instead of stopping after the first entry. The undo
 * and redo stacks are LIFO, so popping the undo stack already visits a
 * group in the reverse of its push order -- and pushing those same popped
 * entries onto the redo stack, in that same pop order, leaves the redo
 * stack ready to replay the group in its original forward order. No extra
 * bookkeeping is needed for either direction. */
void undo_group_begin(void) {
  U.group_seq++;
  U.active_group = U.group_seq;
}

void undo_group_end(void) {
  U.active_group = 0;
  /* Without this, the next typed character could coalesce into the group's
   * trailing insert (see the !U.locked check in editor_insert_char),
   * silently pulling an unrelated keystroke inside the group's undo step. */
  U.locked = 1;
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
  int group = u->group;
  uc_apply(u, 0);
  redo_push(u);
  /* Keep popping while the top of the stack belongs to the same group --
   * already in reverse push order, since both stacks are LIFO. */
  while (group != 0 && U.undo_n > 0 && U.undo[U.undo_n - 1]->group == group) {
    u = U.undo[--U.undo_n];
    uc_apply(u, 0);
    redo_push(u);
  }
  U.locked = 1;
  editor_set_status_message("Undo");
}

void editor_redo(void) {
  if (U.redo_n == 0) {
    editor_set_status_message("Nothing to redo");
    return;
  }
  struct uchange *u = U.redo[--U.redo_n];
  int group = u->group;
  uc_apply(u, 1);
  undo_push(u);
  while (group != 0 && U.redo_n > 0 && U.redo[U.redo_n - 1]->group == group) {
    u = U.redo[--U.redo_n];
    uc_apply(u, 1);
    undo_push(u);
  }
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

/* Character index of the cursor at byte offset cx: the count of whole
 * characters before it, regardless of how wide any of them render (a tab or
 * a double-width glyph is one character, not eight or two). This is what
 * the status bar reports as "Col" -- a tab-stop- or wide-glyph-aware display
 * column would jump around in a way that doesn't match "the Nth character
 * in the line," which is what most editors and tools mean by column. */
static int editor_row_cx_to_charcol(erow *row, int cx) {
  int col = 0, j = 0;
  while (j < cx) {
    j += utf8_char_bytes(row->chars, row->size, j);
    col++;
  }
  return col;
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

/* --- selection: an anchor (sx,sy) plus the live cursor (cx,cy). Movement with
 * Shift held extends it (setting the anchor on the first shifted move); any
 * other key drops it. The covered text is highlighted in reverse video by
 * editor_draw_text_row(), and the byte count is surfaced in the status bar. --- */

static void selection_clear(void) {
  E.sel_active = 0;
}

/* Order the anchor and cursor into a top-left / bottom-right pair, clamped to
 * valid rows so the phantom line past the last row never indexes out of
 * bounds. Returns 0 (leaving the outputs undefined) when no selection. */
static int selection_range(int *ax, int *ay, int *bx, int *by) {
  if (!E.sel_active) return 0;
  int y0 = E.sy, y1 = E.cy, x0 = E.sx, x1 = E.cx;
  if (y0 > y1 || (y0 == y1 && x0 > x1)) {
    int ty = y0, tx = x0;
    y0 = y1; x0 = x1; y1 = ty; x1 = tx;
  }
  if (y0 < 0) y0 = 0;
  if (y1 > E.numrows) y1 = E.numrows;
  *ay = y0; *ax = x0; *by = y1; *bx = x1;
  /* The phantom line past the last row has no characters, so a selection that
   * runs off the end of the buffer always ends at column 0 of it. */
  if (*by == E.numrows) *bx = 0;
  return 1;
}

/* Bytes spanned by the selection, counting one per internal line break. Used
 * only for the status-bar count, so whether a line break is "one byte" is a
 * display choice rather than a correctness one. */
static int selection_bytes(void) {
  int ax, ay, bx, by;
  if (!selection_range(&ax, &ay, &bx, &by)) return 0;
  if (ay == by) {
    int sz = (ay < E.numrows) ? E.row[ay].size : 0;
    if (ax < 0) ax = 0;
    if (bx > sz) bx = sz;
    return (bx > ax) ? bx - ax : 0;
  }
  int total = 0;
  if (ay < E.numrows) total += E.row[ay].size - ax;
  int i;
  for (i = ay + 1; i < by && i < E.numrows; i++) total += E.row[i].size;
  total += bx;
  total += (by - ay);
  return total;
}

/* The selected display-column range on row 'filerow' as absolute columns
 * [out0, out1); out1 <= out0 when no text on this row is selected. Selection
 * endpoints are byte offsets into row->chars, mapped through
 * editor_row_cx_to_rx() so the highlight lands on the rendered glyphs (tab
 * stops and wide glyphs already accounted for). The whole interior span of a
 * multi-line selection is covered; the top row starts at the anchor and the
 * bottom row ends at the far edge. */
static void selection_row_rx(int filerow, int *out0, int *out1) {
  *out0 = 0;
  *out1 = 0;
  int ax, ay, bx, by;
  if (!selection_range(&ax, &ay, &bx, &by)) return;
  if (filerow < ay || filerow > by || filerow >= E.numrows) return;
  erow *r = &E.row[filerow];
  if (ay == by) {
    if (bx > ax) {
      *out0 = editor_row_cx_to_rx(r, ax);
      *out1 = editor_row_cx_to_rx(r, bx);
    }
  } else if (filerow == ay) {
    *out0 = editor_row_cx_to_rx(r, ax);
    *out1 = editor_row_cx_to_rx(r, r->size);
  } else if (filerow == by) {
    *out1 = editor_row_cx_to_rx(r, bx);
  } else {
    *out1 = editor_row_cx_to_rx(r, r->size);
  }
  if (*out1 < *out0) *out1 = *out0;
}

/* Copy the selected bytes into a freshly allocated, NUL-terminated buffer
 * (internal line breaks become '\n'), matching selection_bytes()'s count. The
 * caller frees it. Returns NULL with *outlen 0 when the selection is empty. */
static char *selection_dup(int *outlen) {
  int ax, ay, bx, by;
  *outlen = 0;
  if (!selection_range(&ax, &ay, &bx, &by)) return NULL;
  if (ay == by && ax == bx) return NULL;

  int size;
  if (ay == by) {
    size = bx - ax;
  } else {
    size = (E.row[ay].size - ax) + 1;   /* tail of the top row + its newline */
    int i;
    for (i = ay + 1; i < by && i < E.numrows; i++) size += E.row[i].size + 1;
    size += bx;                          /* head of the bottom row */
  }
  char *buf = xmalloc((size_t)size + 1);
  int pos = 0;
  if (ay == by) {
    memcpy(buf + pos, E.row[ay].chars + ax, (size_t)(bx - ax));
    pos += bx - ax;
  } else {
    memcpy(buf + pos, E.row[ay].chars + ax, (size_t)(E.row[ay].size - ax));
    pos += E.row[ay].size - ax;
    buf[pos++] = '\n';
    int i;
    for (i = ay + 1; i < by && i < E.numrows; i++) {
      memcpy(buf + pos, E.row[i].chars, (size_t)E.row[i].size);
      pos += E.row[i].size;
      buf[pos++] = '\n';
    }
    if (by < E.numrows) {
      memcpy(buf + pos, E.row[by].chars, (size_t)bx);
      pos += bx;
    }
  }
  buf[pos] = '\0';
  *outlen = pos;
  return buf;
}

/* Replace the clipboard contents. Allocate before releasing the old buffer:
 * if the allocation failed after the free, die()'s atexit cleanup would free
 * the stale E.clipboard a second time. Allocating first means a failure dies
 * with the clipboard still intact. */
static void clipboard_set(const char *s, int len) {
  char *buf = (len > 0) ? xmalloc((size_t)len) : NULL;
  if (len > 0) memcpy(buf, s, (size_t)len);
  free(E.clipboard);
  E.clipboard = buf;
  E.clipsize = (len > 0) ? len : 0;
}

/* Delete the selected range, leaving the cursor at its start. Walks the far
 * end back to the near end with the tested single-character editor_delete_char
 * (which handles multibyte characters and line joins itself), so the selection
 * is always consumed on character boundaries. The whole run is one undo step.
 * No-op (just clears the selection) when it is empty. */
static void selection_delete(void) {
  int ax, ay, bx, by;
  if (!selection_range(&ax, &ay, &bx, &by)) return;
  selection_clear();
  if (ay == by && ax == bx) return;

  undo_group_begin();
  if (by == E.numrows) {
    if (E.numrows == 0) { undo_group_end(); return; }
    E.cy = E.numrows - 1;
    E.cx = E.row[E.numrows - 1].size;
  } else {
    E.cy = by;
    E.cx = bx;
  }
  while (!(E.cy == ay && E.cx == ax)) editor_delete_char();
  undo_group_end();
}

/* Insert the clipboard at the cursor as one undo step; embedded newlines split
 * lines. An active selection is replaced. */
static void editor_paste(void) {
  if (E.clipsize == 0) {
    editor_set_status_message("Clipboard is empty");
    return;
  }
  selection_delete();
  undo_group_begin();
  int i;
  for (i = 0; i < E.clipsize; i++) {
    if (E.clipboard[i] == '\n') editor_insert_newline();
    else editor_insert_char(E.clipboard[i]);
  }
  undo_group_end();
  editor_set_status_message("Pasted %d bytes", E.clipsize);
}

/* Push the clipboard to the terminal's *system* clipboard with OSC 52, so a
 * terminal-native paste (or a Ctrl-V that the terminal intercepts and serves
 * from the system clipboard itself) yields exactly what Olly copied, and what
 * Olly copied can be pasted into other applications too. Without this the
 * in-editor clipboard is invisible to the terminal, so a paste shows whatever
 * an unrelated application last left on the system clipboard.
 *
 * Written straight to the terminal in fixed chunks: it allocates nothing, so a
 * failed copy can never die here after the in-editor clipboard already landed.
 * Strictly best-effort -- terminals that ignore OSC 52 (or have it disabled)
 * just keep their own clipboard, and Olly's own Ctrl-V still pastes the
 * internal buffer. */
static void clipboard_sync_system(void) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char hdr[] = "\x1b]52;c;";
  const unsigned char *in = (const unsigned char *)E.clipboard;
  int len = E.clipsize;
  if (len <= 0) return;
  write_all(STDOUT_FILENO, hdr, sizeof(hdr) - 1);

  char chunk[512];
  int c = 0, i = 0;
  while (i + 3 <= len) {
    unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
    chunk[c++] = tbl[(v >> 18) & 63];
    chunk[c++] = tbl[(v >> 12) & 63];
    chunk[c++] = tbl[(v >> 6) & 63];
    chunk[c++] = tbl[v & 63];
    i += 3;
    if (c + 4 > (int)sizeof(chunk)) {  /* room for one more group, else flush */
      write_all(STDOUT_FILENO, chunk, (size_t)c);
      c = 0;
    }
  }
  if (i < len) {
    int rem = len - i;
    unsigned v = (unsigned)in[i] << 16;
    if (rem > 1) v |= (unsigned)in[i + 1] << 8;
    chunk[c++] = tbl[(v >> 18) & 63];
    chunk[c++] = tbl[(v >> 12) & 63];
    chunk[c++] = rem > 1 ? tbl[(v >> 6) & 63] : '=';
    chunk[c++] = '=';
  }
  if (c) write_all(STDOUT_FILENO, chunk, (size_t)c);
  write_all(STDOUT_FILENO, "\x07", 1);  /* BEL terminator (widely accepted) */
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
 * gap shown as a space, keeping every following column aligned.
 *
 * Characters whose display-column range overlaps [sel0, sel1) are wrapped in
 * reverse video. The escape codes occupy no display columns, so they never
 * disturb alignment, and because they live in the cached drawbuf a change of
 * selection changes the row's bytes and forces just that row to redraw. When
 * there is no selection the emitted bytes are identical to before. */
static void editor_draw_text_row(struct abuf *ab, int y, erow *row,
                                 int sel0, int sel1) {
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
    col += w;
  }

  /* Emit the visible slice, toggling reverse video at the selection edges.
   * Worst case per character is one render glyph plus both escape sequences,
   * so size the buffer against the whole render length, not the visible width:
   * a run of zero-width combining marks can consume every column budget-free
   * iteration. */
  int has_sel = (sel1 > sel0);
  drawbuf_ensure(left_pad + rs * 12 + 16);
  int p = 0;
  while (p < left_pad) drawbuf[p++] = ' ';

  int dcol = col;      /* absolute display column of the character at i */
  int vis = left_pad;  /* visible columns emitted so far */
  int in_sel = 0;
  while (i < rs && vis < E.screencols) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    if (vis + w > E.screencols) break;  /* would overflow the right edge */
    int sel = has_sel && dcol >= sel0 && dcol < sel1;
    if (sel != in_sel) {
      if (sel) { memcpy(drawbuf + p, "\x1b[7m", 4); p += 4; }
      else { memcpy(drawbuf + p, "\x1b[27m", 5); p += 5; }
      in_sel = sel;
    }
    memcpy(drawbuf + p, r + i, (size_t)nb);
    p += nb;
    vis += w;
    dcol += w;
    i += nb;
  }
  if (in_sel) { memcpy(drawbuf + p, "\x1b[27m", 5); p += 5; }

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
      int s0, s1;
      selection_row_rx(filerow, &s0, &s1);
      editor_draw_text_row(ab, y, &E.row[filerow], s0, s1);
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
  /* The character column: the Nth character in the line, not E.rx's
   * tab-stop- and glyph-width-aware display column, which jumps around
   * (a leading tab alone puts the display column at 9) and doesn't match
   * what "column" means in most editors and tools. E.cy == E.numrows is
   * the phantom line past the last row, which has no erow to index. */
  int charcol = (E.cy < E.numrows)
      ? editor_row_cx_to_charcol(&E.row[E.cy], E.cx) : 0;
  int rlen = E.sel_active
      ? snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d  Sel %d",
          E.cy + 1, charcol + 1, selection_bytes())
      : snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d",
          E.cy + 1, charcol + 1);
  if (len > E.screencols) len = E.screencols;
  ab_append(ab, status, len);
  /* Right-align the cursor position when it fits; otherwise pad to the edge. */
  if (E.screencols - len >= rlen) {
    ab_append_fill(ab, ' ', E.screencols - len - rlen);
    ab_append(ab, rstatus, rlen);
  } else {
    ab_append_fill(ab, ' ', E.screencols - len);
  }
  ab_append(ab, "\x1b[K", 3);
  ab_append(ab, "\x1b[m", 3);
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
  } else {
    cache_lines[E.screenrows] = NULL;
    cache_lens[E.screenrows] = -1;
    cache_caps[E.screenrows] = 0;
    cache_lines[E.screenrows + 1] = NULL;
    cache_lens[E.screenrows + 1] = -1;
    cache_caps[E.screenrows + 1] = 0;
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

/* Off by default, matching the search behavior this project already
 * shipped; toggled globally with Ctrl-T rather than per-prompt, since
 * editor_prompt's format string is fixed for the life of the prompt and
 * has no channel back to the caller besides the typed buffer. */
static int search_case_sensitive = 0;

static void editor_set_last_query(const char *q) {
  if (last_query) free(last_query);
  last_query = q ? xstrdup(q) : NULL;
}

static int render_match(const char *hay, const char *needle, int n) {
  return search_case_sensitive ? strncmp(hay, needle, (size_t)n) == 0
                                : strncasecmp(hay, needle, (size_t)n) == 0;
}

/* dir = 1 search forward, -1 search backward. exclude_current causes the
 * search to skip a match exactly at the cursor. nowrap stops the sweep
 * after one pass over every row instead of wrapping back to revisit the
 * starting row -- used by replace-all, which must terminate even when the
 * replacement text itself contains the search term. */
static int editor_search_string(const char *query, int qlen, int dir,
    int exclude_current, int nowrap) {
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
   * is what makes a match earlier on the cursor's own line reachable.
   * nowrap drops that extra pass, so the sweep visits each row exactly
   * once and always terminates. */
  int kmax = nowrap ? n - 1 : n;
  for (k = 0; k <= kmax; k++) {
    int i = dir > 0 ? (cur + k) % n : ((cur - k) % n + n) % n;
    erow *row = &E.row[i];
    int maxstart = row->rsize - qlen;
    int on_start_row = (i == E.cy) && (k == 0);
    if (dir > 0) {
      int start = on_start_row ? (exclude_current ? cur_rb + 1 : cur_rb) : 0;
      int col;
      for (col = start; col <= maxstart; col++) {
        if (render_match(row->render + col, query, qlen)) {
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
        if (render_match(row->render + col, query, qlen)) {
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
  if (editor_search_string(term, strlen(term), 1, excl, 0))
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
  if (editor_search_string(last_query, strlen(last_query), 1, 1, 0))
    editor_set_status_message("Found (next)");
  else
    editor_set_status_message("Not found. Press Ctrl-P to search backward");
}

void editor_find_prev(void) {
  if (last_query == NULL) {
    editor_set_status_message("No previous search. Press Ctrl-F to search");
    return;
  }
  if (editor_search_string(last_query, strlen(last_query), -1, 1, 0))
    editor_set_status_message("Found (previous)");
  else
    editor_set_status_message("Not found. Press Ctrl-N to search forward");
}

void editor_toggle_case_sensitive(void) {
  search_case_sensitive = !search_case_sensitive;
  editor_set_status_message("Search is now case-%s",
      search_case_sensitive ? "sensitive" : "insensitive");
}

/* Length, in chars-space bytes, of a match that started at cx in row and is
 * qlen bytes long in render-space. The two spaces diverge whenever a tab is
 * in play (one byte in chars, up to KILO_TAB_STOP in render), so the match
 * length cannot simply be reused as the delete length -- it must be
 * remeasured through the same rbyte<->cx mapping search itself uses. */
static int editor_match_char_len(erow *row, int cx, int qlen) {
  int rb = editor_row_cx_to_rbyte(row, cx);
  int cx_end = editor_render_byte_to_cx(row, rb + qlen);
  return cx_end - cx;
}

void editor_goto_line(void) {
  char *input = editor_prompt("Go to line: %s (ESC to cancel)", NULL);
  if (input == NULL) return;

  char *end;
  long line = strtol(input, &end, 10);
  int valid = (end != input) && (*end == '\0');
  free(input);
  if (!valid) {
    editor_set_status_message("Go to line: not a number");
    return;
  }

  /* Clamp into the file's real rows, not the phantom line past the last
   * row (E.cy == E.numrows) that an unclamped high value would otherwise
   * be allowed to land on for an empty request like "line 999999". */
  int max_cy = (E.numrows > 0) ? E.numrows - 1 : 0;
  int target = (int)line - 1;
  if (target < 0) target = 0;
  if (target > max_cy) target = max_cy;
  E.cy = target;
  E.cx = 0;
  editor_set_status_message("Ln %d", E.cy + 1);
}

void editor_replace(void) {
  char *query = editor_prompt("Replace - search: %s (ESC to cancel)", NULL);
  if (query == NULL) return;
  if (query[0] == '\0') {
    editor_set_status_message("Replace aborted: empty search term");
    free(query);
    return;
  }

  char *repl = editor_prompt("Replace with: %s (ESC to cancel)", NULL);
  if (repl == NULL) {
    free(query);
    editor_set_status_message("Replace aborted");
    return;
  }

  char *mode = editor_prompt("Replace: (n)ext or (a)ll? %s", NULL);
  int all = (mode != NULL && (mode[0] == 'a' || mode[0] == 'A'));
  free(mode);

  int qlen = (int)strlen(query);
  size_t rlen = strlen(repl);
  int count = 0;
  int grouped = 0;

  if (all) {
    E.cy = 0;
    E.cx = 0;
  }

  /* nowrap=1: without it a replace-all whose replacement text contains the
   * search term would find its own output forever. Positioning the cursor
   * just past each replacement before the next search call means the just
   * -inserted text is always behind the new starting point, so it can
   * never be matched again even without the nowrap guard -- but nowrap is
   * kept too, since it is what makes the sweep provably finite regardless. */
  while (editor_search_string(query, qlen, 1, 0, 1)) {
    if (!grouped) {
      undo_group_begin();
      grouped = 1;
    }

    erow *row = &E.row[E.cy];
    int cx_start = E.cx;
    int mlen = editor_match_char_len(row, cx_start, qlen);

    struct uchange *ud = uc_new(UC_DELETE, E.cy, cx_start,
        row->chars + cx_start, (size_t)mlen);
    ud->back = 0;
    ud->cx0 = cx_start;
    ud->cy0 = E.cy;
    ud->cx1 = cx_start;
    ud->cy1 = E.cy;
    undo_push(ud);
    editor_row_delete_n(row, cx_start, mlen);

    struct uchange *ui = uc_new(UC_INSERT, E.cy, cx_start, repl, rlen);
    ui->cx0 = cx_start;
    ui->cy0 = E.cy;
    ui->cx1 = cx_start + (int)rlen;
    ui->cy1 = E.cy;
    undo_push(ui);
    editor_row_insert_n(row, cx_start, repl, rlen);

    count++;
    E.cx = cx_start + (int)rlen;

    if (!all) break;
  }
  if (grouped) undo_group_end();

  free(query);
  free(repl);

  if (count == 0)
    editor_set_status_message("No matches");
  else
    editor_set_status_message("Replaced %d occurrence%s", count,
        count == 1 ? "" : "s");
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
    have_disk_mtime = 0;
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

  /* Someone else may have written the file since it was opened (or last
   * saved). Ask before overwriting their change -- only meaningful when
   * there's a prior on-disk version to compare against and to lose. */
  if (have_disk_mtime && target_exists && !mtime_eq(st.st_mtim, on_disk_mtime)) {
    char *ans = editor_prompt(
        "File changed on disk since opened. Save anyway? (y/N): %s", NULL);
    int proceed = ans != NULL && (ans[0] == 'y' || ans[0] == 'Y');
    free(ans);
    if (!proceed) {
      editor_set_status_message("Save aborted: file changed on disk");
      free(resolved);
      return;
    }
  }

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
    struct stat st_after;
    int have_after = dok && (fstat(direct, &st_after) == 0);
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
    have_disk_mtime = have_after;
    if (have_after) on_disk_mtime = st_after.st_mtim;
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
  /* Captured from the still-open fd, before close/rename: rename() doesn't
   * change the file's mtime (same inode), but re-stating the target path
   * afterward would be both an extra syscall and a needless race window. */
  struct stat st_after;
  int have_after = ok && (fstat(fd, &st_after) == 0);
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
  have_disk_mtime = have_after;
  if (have_after) on_disk_mtime = st_after.st_mtim;
  if (recovery_path[0] != '\0') unlink(recovery_path);
}

void editor_open(char *filename) {
  free(E.filename);
  E.filename = xstrdup(filename);
  build_recovery_path();
  have_disk_mtime = 0;

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
  /* fstat on the still-open descriptor, not a path-based stat after close:
   * the file could otherwise be replaced between the two calls, baselining
   * the wrong content's mtime. */
  struct stat ost;
  if (fstat(fileno(fp), &ost) == 0) {
    on_disk_mtime = ost.st_mtim;
    have_disk_mtime = 1;
  }
  fclose(fp);
  E.dirty = 0;
  saved_snapshot_take();
}

/* ---- help screen: data + layout --------------------------------------- *
 * Every keybinding lives here, grouped by function. The renderer drops to a
 * single column when the terminal is narrow and scrolls when the content is
 * taller than the screen, so nothing is ever clipped. */
struct help_item { const char *key; const char *desc; };
struct help_group { const char *title; const struct help_item *items; };

static const struct help_item help_items_files[] = {
  {"Ctrl-S",   "Save file"},
  {"Ctrl-Q",   "Quit"},
  {"Ctrl-?",   "This help screen"},
  {NULL, NULL}
};
static const struct help_item help_items_movement[] = {
  {"Arrows",       "Move cursor"},
  {"PgUp/PgDn",    "Page up / down"},
  {"Home/End",     "Line start / end"},
  {"Ctrl-G",       "Go to line"},
  {NULL, NULL}
};
static const struct help_item help_items_selection[] = {
  {"Shift+Arrows",   "Extend selection"},
  {"Shift+Home/End", "To line edge"},
  {"Shift+PgUp/Dn",  "Extend by page"},
  {NULL, NULL}
};
static const struct help_item help_items_editing[] = {
  {"Enter",      "Insert new line"},
  {"Tab",        "Insert tab"},
  {"Bksp/Ctrl-H","Delete before"},
  {"Del",        "Delete at cursor"},
  {NULL, NULL}
};
static const struct help_item help_items_find[] = {
  {"Ctrl-F", "Find forward"},
  {"Ctrl-N", "Find next"},
  {"Ctrl-P", "Find previous"},
  {"Ctrl-T", "Case sensitivity"},
  {"Ctrl-R", "Find & replace"},
  {NULL, NULL}
};
static const struct help_item help_items_clip[] = {
  {"Ctrl-C", "Copy selection"},
  {"Ctrl-X", "Cut selection"},
  {"Ctrl-V", "Paste at cursor"},
  {NULL, NULL}
};
static const struct help_item help_items_history[] = {
  {"Ctrl-Z", "Undo"},
  {"Ctrl-Y", "Redo"},
  {NULL, NULL}
};
static const struct help_item help_items_view[] = {
  {"Ctrl-L", "Redraw screen"},
  {NULL, NULL}
};

/* Order matters: the two-column split takes the first half vs the second. */
static const struct help_group help_groups[] = {
  {"FILES",          help_items_files},
  {"MOVEMENT",       help_items_movement},
  {"SELECTION",      help_items_selection},
  {"EDITING",        help_items_editing},
  {"FIND & REPLACE", help_items_find},
  {"CLIPBOARD",      help_items_clip},
  {"HISTORY",        help_items_history},
  {"VIEW",           help_items_view},
};

#define HELP_WIDE 80      /* two columns at or above this width */
#define HELP_GAP 4        /* blank columns between the two columns */
#define HELP_KEYW 14      /* width reserved for the key name */
#define HELP_MAXROWS 48
#define HELP_MAXW 512

static char help_lcol[HELP_MAXROWS][HELP_MAXW];
static char help_rcol[HELP_MAXROWS][HELP_MAXW];
static char help_line[HELP_MAXW];

static void help_build_col(char col[HELP_MAXROWS][HELP_MAXW], int *rn,
                           int g0, int g1, int keyw, int descw) {
  int r = 0, g;
  for (g = g0; g <= g1 && r < HELP_MAXROWS - 1; g++) {
    if (g > g0) { col[r][0] = '\0'; r++; }
    snprintf(col[r], HELP_MAXW, "%s", help_groups[g].title);
    r++;
    const struct help_item *it;
    for (it = help_groups[g].items; it->key && r < HELP_MAXROWS - 1; it++) {
      snprintf(col[r], HELP_MAXW, "  %-*.*s %.*s",
               keyw, keyw, it->key, descw, it->desc);
      r++;
    }
  }
  *rn = r;
}

/* One composed screen row: left field padded to colw, a gap, then the right
 * field truncated to whatever width remains. All help text is ASCII, so this
 * byte-wise truncation can never split a multibyte character. */
static void help_row(char *dst, int dstsz, const char *left, const char *right,
                     int colw, int gap, int total) {
  int rightw = total - colw - gap;
  if (rightw < 0) rightw = 0;
  snprintf(dst, dstsz, "%-*.*s%*s%.*s",
           colw, colw, left, gap, "", rightw, right);
}

static void help_centered(struct abuf *ab, const char *text, int row) {
  char pos[32];
  int pad = (E.screencols - (int)strlen(text)) / 2;
  if (pad < 0) pad = 0;
  snprintf(pos, sizeof(pos), "\x1b[%d;%dH", row, pad + 1);
  ab_append(ab, pos, strlen(pos));
  int len = strlen(text);
  if (len > E.screencols) len = E.screencols;
  ab_append(ab, text, len);
  ab_append(ab, "\x1b[K", 3);
}

/* Re-read the terminal size into E.screenrows/E.screencols while help is shown.
 * The editor's own refresh does this between keypresses, but help runs its own
 * loop, so without this it would keep drawing at whatever size was current when
 * it opened. Mirrors editor_refresh_screen's fallback to the last known size. */
static void help_refresh_size(void) {
  int wsrows, wscols;
  if (get_window_size(&wsrows, &wscols) == -1) {
    wsrows = last_wsrows;
    wscols = last_wscols;
  }
  last_wsrows = wsrows;
  last_wscols = wscols;
  E.screenrows = wsrows - 2;
  if (E.screenrows < 1) E.screenrows = 1;
  E.screencols = wscols;
}

void editor_help(void) {
  int scroll = 0;
  for (;;) {
    /* Recompute the whole layout from the live terminal size on every pass, so
     * a resize (surfaced as WINCH_KEY) reflows one-column vs two-column, recent
     * the title, and recomputes the scroll extent to match the new window. */
    help_refresh_size();

    int wide = (E.screencols >= HELP_WIDE);
    int gap = wide ? HELP_GAP : 0;
    int colw = wide ? (E.screencols - gap) / 2 : E.screencols;
    if (colw < 1) colw = 1;
    int keyw = HELP_KEYW;
    if (keyw > colw - 3) keyw = (colw > 3) ? colw - 3 : 1;
    int descw = colw - 3 - keyw;
    if (descw < 1) descw = 1;

    int ngroups = (int)(sizeof(help_groups) / sizeof(help_groups[0]));
    int nl, nr;
    if (wide) {
      int mid = ngroups / 2;
      help_build_col(help_lcol, &nl, 0, mid - 1, keyw, descw);
      help_build_col(help_rcol, &nr, mid, ngroups - 1, keyw, descw);
    } else {
      help_build_col(help_lcol, &nl, 0, ngroups - 1, keyw, descw);
      nr = 0;
    }
    int content = (nl > nr) ? nl : nr;

    int content_top = 3;                 /* row 1 title, row 2 blank */
    int view_h = E.screenrows - (content_top - 1) - 2; /* 2 footer rows */
    if (view_h < 1) view_h = 1;
    int maxscroll = content - view_h;
    if (maxscroll < 0) maxscroll = 0;
    if (scroll > maxscroll) scroll = maxscroll;
    if (scroll < 0) scroll = 0;

    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[2J", 4);
    ab_append(&ab, "\x1b[?25l", 6);
    char pos[32];

    help_centered(&ab, "Olly - Keyboard Reference", 1);

    int k;
    for (k = 0; k < view_h; k++) {
      int idx = scroll + k;
      if (idx >= nl && idx >= nr) break;
      const char *L = (idx < nl) ? help_lcol[idx] : "";
      const char *R = (idx < nr) ? help_rcol[idx] : "";
      help_row(help_line, sizeof(help_line), L, R, colw, gap, E.screencols);
      snprintf(pos, sizeof(pos), "\x1b[%d;1H", content_top + k);
      ab_append(&ab, pos, strlen(pos));
      ab_append(&ab, help_line, strlen(help_line));
      ab_append(&ab, "\x1b[K", 3);
    }

    help_centered(&ab, "In prompts: Enter accepts   Esc cancels",
                  E.screenrows - 1);
    help_centered(&ab,
        maxscroll > 0 ? "PgUp/PgDn scroll   any other key returns"
                      : "Press any key to return to the editor",
        E.screenrows);

    write_all(STDOUT_FILENO, ab.b, (size_t)ab.len);
    free(ab.b);

    int c = editor_read_key();
    /* A resize is not a dismiss: fall through to the top of the loop, which
     * re-reads the size and re-lays the screen out at the new dimensions. */
    if (c == WINCH_KEY) continue;
    if (maxscroll == 0) break;
    int ns = scroll;
    if (c == ARROW_DOWN) ns = scroll + 1;
    else if (c == ARROW_UP) ns = scroll - 1;
    else if (c == PAGE_DOWN) ns = scroll + view_h;
    else if (c == PAGE_UP) ns = scroll - view_h;
    else if (c == HOME_KEY) ns = 0;
    else if (c == END_KEY) ns = maxscroll;
    else break;
    if (ns < 0) ns = 0;
    if (ns > maxscroll) ns = maxscroll;
    scroll = ns;
  }
  force_full = 1;
}

void editor_process_keypress(void) {
  static int quit_times = KILO_QUIT_TIMES;
  int c = editor_read_key();
  int shift = last_key_shift;
  int pre_cx = E.cx, pre_cy = E.cy;
  int sel_move = (c == ARROW_UP || c == ARROW_DOWN || c == ARROW_LEFT ||
      c == ARROW_RIGHT || c == HOME_KEY || c == END_KEY ||
      c == PAGE_UP || c == PAGE_DOWN);

  switch (c) {
    case '\r':
      if (E.sel_active) selection_delete();
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

    case CTRL_KEY('c'): {
      /* Copy: selection to clipboard, selection kept (see the clear guard
       * below). Ctrl-C only reaches here because raw mode clears ISIG. */
      if (E.sel_active) {
        int n = 0;
        char *s = selection_dup(&n);
        clipboard_set(s, n);
        free(s);
        clipboard_sync_system();
        editor_set_status_message("Copied %d bytes", n);
      }
      break;
    }

    case CTRL_KEY('x'):
      /* Cut: copy then delete the selection (one undo step). */
      if (E.sel_active) {
        int n = 0;
        char *s = selection_dup(&n);
        clipboard_set(s, n);
        free(s);
        clipboard_sync_system();
        selection_delete();
        editor_set_status_message("Cut %d bytes", n);
      }
      break;

    case CTRL_KEY('v'):
      editor_paste();
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

    case CTRL_KEY('t'):
      editor_toggle_case_sensitive();
      break;

    case CTRL_KEY('r'):
      editor_replace();
      break;

    case CTRL_KEY('g'):
      editor_goto_line();
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
      if (E.sel_active) {
        selection_delete();
      } else if (c == DEL_KEY) {
        editor_del_char();
      } else {
        editor_delete_char();
      }
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

    case WINCH_KEY:
      /* Not a keystroke -- just loop back so the next editor_refresh_screen
       * picks up the new terminal size. Return rather than break, so this
       * doesn't reset the Ctrl-Q quit-confirmation countdown. */
      return;

    case '\x1b':
      break;

    default:
      /* Tab is a control character, so testing iscntrl() alone threw it
       * away and a tab could not be typed at all. Key codes of 1000 and up
       * are outside iscntrl()'s domain and must not reach it. */
      if (c == '\t' || (c < 256 && !iscntrl(c))) {
        if (E.sel_active) selection_delete();
        editor_insert_char(c);
      }
      break;
  }

  /* A shifted movement extends the selection (seeding the anchor on the first
   * one from a resting cursor). Copying keeps the selection; every other
   * plain move, edit, or command drops it. Edits that need the selected text
   * first consume it via selection_delete() above. */
  if (sel_move && shift) {
    if (!E.sel_active) {
      E.sx = pre_cx;
      E.sy = pre_cy;
      E.sel_active = 1;
    }
  } else if (c != CTRL_KEY('c')) {
    selection_clear();
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
  free(E.clipboard);
  E.clipboard = NULL;
  E.clipsize = 0;
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
  E.sel_active = 0;
  E.sx = 0;
  E.sy = 0;
  E.clipboard = NULL;
  E.clipsize = 0;
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

  /* Paint the first frame up front. The loop below reads a key before it
   * paints, so without this the screen stayed blank until the first
   * keystroke. */
  editor_refresh_screen();

  while (1) {
    editor_process_keypress();
    editor_refresh_screen();
  }

  return 0;
}