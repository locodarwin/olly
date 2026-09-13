#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
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
  int dirty;
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
  E.row = xrealloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = xmalloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].rcap = 0;
  editor_update_row(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editor_free_row(erow *row) {
  free(row->render);
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

int editor_row_cx_to_rx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }
  return rx;
}

int editor_row_rx_to_cx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t')
      cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
    cur_rx++;
    if (cur_rx > rx) return cx;
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
    int at = E.cx - 1;
    struct uchange *top = U.undo_n ? U.undo[U.undo_n - 1] : NULL;
    if (!U.locked && top && top->kind == UC_DELETE && top->row == E.cy &&
        top->at == E.cx && top->back) {
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
      struct uchange *u = uc_new(UC_DELETE, E.cy, at, row->chars + at, 1);
      u->back = 1;
      u->cx0 = pre_cx;
      u->cy0 = pre_cy;
      u->cx1 = at;
      u->cy1 = E.cy;
      undo_push(u);
      clear_redo();
    }
    editor_row_delete_char(row, at);
    E.cx--;
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
    struct uchange *top = U.undo_n ? U.undo[U.undo_n - 1] : NULL;
    if (!U.locked && top && top->kind == UC_DELETE && top->row == E.cy &&
        top->at == E.cx && !top->back) {
      size_t at = E.cx;
      size_t l = top->len;
      top->text = xrealloc(top->text, l + 2);
      top->text[l] = row->chars[at];
      top->text[l + 1] = '\0';
      top->len++;
      top->cx1 = E.cx;
      top->cy1 = E.cy;
    } else {
      struct uchange *u = uc_new(UC_DELETE, E.cy, E.cx, row->chars + E.cx, 1);
      u->back = 0;
      u->cx0 = pre_cx;
      u->cy0 = pre_cy;
      u->cx1 = E.cx;
      u->cy1 = E.cy;
      undo_push(u);
      clear_redo();
    }
    editor_row_delete_char(row, E.cx);
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
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
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
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      cache_line_draw(ab, y, E.row[filerow].render + E.coloff, len, 0);
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
  int rlen = snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d",
      E.cy + 1, E.cx + 1);
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
  get_window_size(&wsrows, &wscols);
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
  write(STDOUT_FILENO, ab.b, ab.len);
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
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editor_set_status_message("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (callback) callback(buf, c);
      return buf;
    } else if (c < 128 && !iscntrl(c)) {
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
  /* k runs to n inclusive, so the starting row is visited a second time at
   * the end of the wrap. Only then is it scanned from its beginning, which
   * is what makes a match earlier on the cursor's own line reachable. */
  for (k = 0; k <= n; k++) {
    int i = dir > 0 ? (cur + k) % n : ((cur - k) % n + n) % n;
    erow *row = &E.row[i];
    int maxstart = row->rsize - qlen;
    int on_start_row = (i == E.cy) && (k == 0);
    if (dir > 0) {
      int start = on_start_row ? (exclude_current ? E.rx + 1 : E.rx) : 0;
      int col;
      for (col = start; col <= maxstart; col++) {
        if (strncasecmp(row->render + col, query, qlen) == 0) {
          E.cy = i;
          E.cx = editor_row_rx_to_cx(row, col);
          return 1;
        }
      }
    } else {
      int start = on_start_row ? (exclude_current ? E.rx - 1 : E.rx)
                               : maxstart;
      if (start > maxstart) start = maxstart;
      int col;
      for (col = start; col >= 0; col--) {
        if (strncasecmp(row->render + col, query, qlen) == 0) {
          E.cy = i;
          E.cx = editor_row_rx_to_cx(row, col);
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

/* Write every row, newline-terminated, to an already-open descriptor. Rows
 * are gathered into a 64 KB buffer and written a chunk at a time: writing
 * each row and then its newline cost two syscalls per line, 100,000 of them
 * for a 50,000-line file. */
static int editor_write_rows(int fd, size_t *total) {
  static char buf[65536];
  size_t used = 0, n = 0;
  int j;
  for (j = 0; j < E.numrows; j++) {
    size_t len = (size_t)E.row[j].size;
    if (used + len + 1 > sizeof(buf)) {
      if (write_all(fd, buf, used) == -1) return -1;
      used = 0;
    }
    if (len + 1 > sizeof(buf)) {
      /* Longer than the whole buffer: it was flushed above, so write the
       * row straight through. */
      if (write_all(fd, E.row[j].chars, len) == -1 ||
          write_all(fd, "\n", 1) == -1)
        return -1;
    } else {
      memcpy(buf + used, E.row[j].chars, len);
      buf[used + len] = '\n';
      used += len + 1;
    }
    n += len + 1;
  }
  if (write_all(fd, buf, used) == -1) return -1;
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

  mode_t mode = (stat(target, &st) == 0) ? (st.st_mode & 07777)
                                         : default_file_mode();

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
    return;
  }

  int ok = 1;
  /* Not fatal if it fails: the file is saved either way, only its mode is off.
   * mkstemp creates at 0600, so the content is never briefly world-readable. */
  (void)fchmod(fd, mode);

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
}

void editor_open(char *filename) {
  free(E.filename);
  E.filename = xstrdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    if (errno == ENOENT) return;
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
        line[linelen - 1] == '\r'))
      linelen--;
    editor_insert_row(E.numrows, line, linelen);
  }
  /* getline returns -1 for a read error as well as at end of file. Taking an
   * error for the end would present a partial buffer as the whole file, and
   * the next save would truncate the file on disk. This also refuses a
   * directory, which used to open as an empty buffer that could never be
   * saved. */
  if (!feof(fp)) die(filename);
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

  write(STDOUT_FILENO, ab.b, ab.len);
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
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
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
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.dirty = 0;

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