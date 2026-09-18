#!/usr/bin/env python3
"""Regression tests for olly.

Run with:  make test        (or: python3 tests/test_olly.py ./olly)

Every test here pins down a bug that was actually reachable from the keyboard,
so a failure means a real regression rather than a style drift.
"""

import fcntl
import multiprocessing
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import run, run_until_signal, status_lines, cursor_col

OLLY = None
SAVE = "\x13"
UNDO = "\x1a"
REDO = "\x19"
GOTO = "\x07"
TOGGLE_CASE = "\x14"
REPLACE = "\x12"
FAILURES = []
PASSED = 0

HOME, END = "\x1b[H", "\x1b[F"
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"


def run_interleaved(argv, before_keys, touch, after_keys, rows=24, cols=80):
    """Like pty_harness.run, but calls touch() between the two key batches --
    used to simulate another process modifying the file while olly still has
    it open, which is not expressible as a single flat key list."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(argv[0], argv, dict(os.environ, TERM="xterm"))

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = b""
    time.sleep(0.3)

    def drain(timeout):
        nonlocal out
        while select.select([fd], [], [], timeout)[0]:
            try:
                chunk = os.read(fd, 1 << 20)
            except OSError:
                return
            if not chunk:
                return
            out += chunk

    def send(keys):
        for key in keys:
            if isinstance(key, float):
                time.sleep(key)
                continue
            try:
                os.write(fd, key.encode() if isinstance(key, str) else key)
            except OSError:
                break
            time.sleep(0.06)
            drain(0.02)

    send(before_keys)
    touch()
    send(after_keys)

    deadline = time.time() + 0.4
    while time.time() < deadline:
        drain(0.1)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    os.waitpid(pid, 0)
    try:
        os.close(fd)
    except OSError:
        pass
    return out


def check(label, got, want):
    global PASSED
    if got == want:
        PASSED += 1
        print("  pass  %s" % label)
    else:
        FAILURES.append(label)
        print("  FAIL  %s\n          want: %r\n          got:  %r"
              % (label, want, got))


def edit(tmpdir, original, keys, name="f.txt"):
    """Open a file with `original` in it, send `keys`, return it afterwards."""
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(original)
    run([OLLY, path], keys)
    with open(path) as fh:
        return fh.read()


def edit_bytes(tmpdir, original, keys, name="f.txt"):
    """Like edit(), but writes and reads raw bytes -- for content that is not
    valid text (CRLF endings, no trailing newline, multibyte characters)."""
    path = os.path.join(tmpdir, name)
    with open(path, "wb") as fh:
        fh.write(original)
    run([OLLY, path], keys)
    with open(path, "rb") as fh:
        return fh.read()


# --------------------------------------------------------------- input ----

def test_keys(tmpdir):
    print("\ninput: cursor keys move, they do not type")
    check("arrow right", edit(tmpdir, "ab\n", [RIGHT, "X", SAVE]), "aXb\n")
    check("arrow left", edit(tmpdir, "ab\n", [RIGHT, RIGHT, LEFT, "X", SAVE]),
          "aXb\n")
    check("arrow down", edit(tmpdir, "ab\ncd\n", [DOWN, "X", SAVE]),
          "ab\nXcd\n")
    check("arrow up", edit(tmpdir, "ab\ncd\n", [DOWN, UP, "X", SAVE]),
          "Xab\ncd\n")
    check("home", edit(tmpdir, "ab\n", [END, HOME, "X", SAVE]), "Xab\n")
    check("end", edit(tmpdir, "ab\n", [END, "X", SAVE]), "abX\n")
    check("home via \\x1b[1~",
          edit(tmpdir, "ab\n", [RIGHT, "\x1b[1~", "X", SAVE]), "Xab\n")
    check("end via \\x1b[4~", edit(tmpdir, "ab\n", ["\x1b[4~", "X", SAVE]),
          "abX\n")
    check("delete key", edit(tmpdir, "abc\n", ["\x1b[3~", SAVE]), "bc\n")
    check("page down", edit(tmpdir, "a\nb\nc\n", ["\x1b[6~", "X", SAVE]),
          "a\nb\nc\nX\n")
    check("page up",
          edit(tmpdir, "a\nb\nc\n", [DOWN, DOWN, "\x1b[5~", "X", SAVE]),
          "Xa\nb\nc\n")

    print("\ninput: SS3 (application cursor mode)")
    check("SS3 right", edit(tmpdir, "ab\n", ["\x1bOC", "X", SAVE]), "aXb\n")
    check("SS3 home", edit(tmpdir, "ab\n", [END, "\x1bOH", "X", SAVE]),
          "Xab\n")
    check("SS3 end", edit(tmpdir, "ab\n", ["\x1bOF", "X", SAVE]), "abX\n")

    # Regression: a partly-consumed escape sequence used to leave its tail in
    # the input stream, where it was inserted as ordinary text. Ctrl-Right
    # ("\x1b[1;5C") typed a literal "5C" into the file.
    print("\ninput: unmapped escape sequences are swallowed, never typed")
    for label, seq in [("ctrl-right", "\x1b[1;5C"),
                       ("shift-left", "\x1b[1;2D"),
                       ("ctrl-shift-up", "\x1b[1;6A"),
                       ("F5", "\x1b[15~"),
                       ("F12", "\x1b[24~"),
                       ("bracketed paste on", "\x1b[200~"),
                       ("bracketed paste off", "\x1b[201~"),
                       ("SGR mouse", "\x1b[<0;1;1M"),
                       ("private CSI", "\x1b[?1049h"),
                       ("bare ESC", "\x1b")]:
        check(label + " inserts nothing",
              edit(tmpdir, "ab\n", [seq, SAVE]), "ab\n")


def test_tab(tmpdir):
    print("\ninput: the Tab key types a tab")
    # Regression: Tab is a control character, and the keypress handler threw
    # away every control character it had no binding for, so a tab could not
    # be typed at all -- even though tabs already in a file displayed fine.
    check("tab at line start", edit(tmpdir, "ab\n", ["\t", SAVE]), "\tab\n")
    check("typing continues after the tab",
          edit(tmpdir, "ab\n", [RIGHT, "\t", "X", SAVE]), "a\tXb\n")
    check("tab undoes", edit(tmpdir, "ab\n", ["\t", "\x1a", SAVE]), "ab\n")
    check("unbound control characters are still not typed",
          edit(tmpdir, "ab\n", ["\x01\x02\x0b", SAVE]), "ab\n")


# -------------------------------------------------------------- search ----

def test_search(tmpdir):
    print("\nsearch: wraps all the way around, including the cursor's own row")
    path = os.path.join(tmpdir, "s.txt")

    def find(body, keys):
        with open(path, "w") as fh:
            fh.write(body)
        return (status_lines(run([OLLY, path], keys)) or [""])[-1]

    # Regression: the sweep used to visit the starting row once, scanning only
    # from the cursor forward, so a match earlier on that row was unreachable.
    check("match before cursor, single line",
          find("alpha beta\n", [END, "\x06", "alpha", "\r"]).startswith("Found"),
          True)
    check("match before cursor, multi line",
          find("alpha beta\nsecond line\n",
               [END, "\x06", "alpha", "\r"]).startswith("Found"),
          True)
    check("match after cursor still found",
          find("alpha beta\n", ["\x06", "beta", "\r"]).startswith("Found"),
          True)
    check("genuinely absent term reports not found",
          find("alpha beta\n", ["\x06", "zzzz", "\r"]).startswith("Not found"),
          True)
    check("search is case-insensitive",
          find("Alpha\n", [END, "\x06", "alpha", "\r"]).startswith("Found"),
          True)


def test_case_sensitive_search(tmpdir):
    print("\nsearch: Ctrl-T toggles case-sensitive matching")
    path = os.path.join(tmpdir, "cs.txt")

    def find(body, keys):
        with open(path, "w") as fh:
            fh.write(body)
        return (status_lines(run([OLLY, path], keys)) or [""])[-1]

    check("toggling on reports the new mode",
          find("Alpha\n", [TOGGLE_CASE]),
          "Search is now case-sensitive")
    check("toggling again reports insensitive",
          find("Alpha\n", [TOGGLE_CASE, TOGGLE_CASE]),
          "Search is now case-insensitive")
    check("case-sensitive search misses a differently-cased match",
          find("Alpha\n", [TOGGLE_CASE, HOME, "\x06", "alpha", "\r"])
              .startswith("Not found"),
          True)
    check("case-sensitive search still finds an exact-case match",
          find("Alpha\n", [TOGGLE_CASE, HOME, "\x06", "Alpha", "\r"])
              .startswith("Found"),
          True)


# --------------------------------------------------------------- goto ----

def test_goto_line(tmpdir):
    print("\ngoto: Ctrl-G moves the cursor to a line, clamped and validated")
    body = "one\ntwo\nthree\nfour\nfive\n"
    check("goes to an existing line",
          edit(tmpdir, body, [GOTO, "3", "\r", "X", SAVE]),
          "one\ntwo\nXthree\nfour\nfive\n")
    check("out-of-range input clamps to the last line",
          edit(tmpdir, body, [GOTO, "999", "\r", "X", SAVE]),
          "one\ntwo\nthree\nfour\nXfive\n")
    check("line 0 clamps to the first line",
          edit(tmpdir, body, [GOTO, "0", "\r", "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    check("a negative line clamps to the first line",
          edit(tmpdir, body, [GOTO, "-1", "\r", "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    # Regression: atoi() on non-numeric input silently returns 0, which (as
    # line - 1) sent the cursor to row -1 -- an out-of-bounds row access.
    # strtol-based validation rejects it outright instead.
    check("non-numeric input is rejected, cursor left unmoved",
          edit(tmpdir, body, [GOTO, "abc", "\r", "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    check("ESC cancels without moving the cursor",
          edit(tmpdir, body, [GOTO, "3", "\x1b", 0.15, "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    check("an empty buffer does not crash (E.cy == E.numrows == 0)",
          edit(tmpdir, "", [GOTO, "5", "\r", "X", SAVE]),
          "X\n")


# ------------------------------------------------------------- replace ----

def test_replace(tmpdir):
    print("\nreplace: Ctrl-R replaces the next match or every match")
    check("replace next only touches the first match",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "n", "\r", SAVE]),
          "dog cat cat\n")
    check("replace all touches every match",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r", SAVE]),
          "dog dog dog\n")
    check("empty search term aborts, buffer untouched",
          edit(tmpdir, "cat\n", [REPLACE, "\r", SAVE]),
          "cat\n")
    check("ESC at the search prompt aborts",
          edit(tmpdir, "cat\n", [REPLACE, "\x1b", 0.15, "X", SAVE]),
          "Xcat\n")
    check("ESC at the replacement prompt aborts, nothing replaced",
          edit(tmpdir, "cat\n", [REPLACE, "cat", "\r", "\x1b", SAVE]),
          "cat\n")
    check("replace-all is case-insensitive by default",
          edit(tmpdir, "Cat cat CAT\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r", SAVE]),
          "dog dog dog\n")
    check("the Ctrl-T case-sensitivity toggle is honored by replace",
          edit(tmpdir, "Cat cat CAT\n",
               [TOGGLE_CASE, REPLACE, "cat", "\r", "dog", "\r", "a", "\r",
                SAVE]),
          "Cat dog CAT\n")
    # Regression: replace-all must terminate even when the replacement text
    # contains the search term -- it must not keep re-matching its own
    # output.
    check("a replacement containing the search term still terminates",
          edit(tmpdir, "a a a\n",
               [REPLACE, "a", "\r", "aa", "\r", "a", "\r", SAVE]),
          "aa aa aa\n")

    print("\nreplace: a match spanning a tab-expanded region deletes the "
          "right bytes")
    # Regression: search matches against the tab-expanded render buffer, so
    # a match's length in render bytes is not always its length in
    # chars-space -- a tab is one byte in chars but up to KILO_TAB_STOP in
    # render. Naively deleting qlen chars-bytes here would try to delete 8
    # bytes from a 4-byte row (out of range -- editor_row_delete_n silently
    # no-ops) and leave the matched text in place instead of replacing it.
    check("replace across a tab spans the tab as a single character",
          edit(tmpdir, "ab\tc\n",
               [REPLACE, "b      c", "\r", "Q", "\r", "n", "\r", SAVE]),
          "aQ\n")

    print("\nreplace: replace-all undoes and redoes as a single step")
    check("one undo restores the original after replace-all",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r", UNDO, SAVE]),
          "cat cat cat\n")
    check("one redo re-applies the whole replace-all",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r",
                UNDO, REDO, SAVE]),
          "dog dog dog\n")


# ---------------------------------------------------------------- save ----

def test_save(tmpdir):
    print("\nsave: naming")
    # Regression: answering the "Save as" prompt with an empty line used to set
    # the filename to "", which could never be written and, being non-NULL,
    # suppressed the prompt forever -- stranding the buffer with no way out.
    work = tempfile.mkdtemp(dir=tmpdir)
    # Type, Ctrl-S, answer the prompt with an empty line, then Ctrl-S again:
    # the second Ctrl-S must re-prompt, so the named file ends up written.
    cwd = os.getcwd()
    os.chdir(work)
    try:
        run([OLLY], ["abc", SAVE, "\r", SAVE, "named.txt", "\r"])
        check("empty answer re-prompts instead of stranding the buffer",
              os.path.exists(os.path.join(work, "named.txt")), True)
        if os.path.exists(os.path.join(work, "named.txt")):
            with open(os.path.join(work, "named.txt")) as fh:
                check("re-prompted save wrote the buffer", fh.read(), "abc\n")
    finally:
        os.chdir(cwd)

    print("\nsave: the temp file must not collide with the user's own files")
    # Regression: the temp file was a fixed "<target>.tmp", unlinked without
    # looking, so saving notes.txt destroyed an unrelated notes.txt.tmp.
    d = tempfile.mkdtemp(dir=tmpdir)
    with open(os.path.join(d, "notes.txt"), "w") as fh:
        fh.write("data\n")
    with open(os.path.join(d, "notes.txt.tmp"), "w") as fh:
        fh.write("A REAL FILE\n")
    run([OLLY, os.path.join(d, "notes.txt")], ["X", SAVE])
    check("unrelated .tmp file survives",
          os.path.exists(os.path.join(d, "notes.txt.tmp")), True)
    with open(os.path.join(d, "notes.txt")) as fh:
        check("target still saved correctly", fh.read(), "Xdata\n")

    print("\nsave: metadata and fallbacks")
    d = tempfile.mkdtemp(dir=tmpdir)
    priv = os.path.join(d, "priv.txt")
    with open(priv, "w") as fh:
        fh.write("secret\n")
    os.chmod(priv, 0o600)
    run([OLLY, priv], ["X", SAVE])
    check("restrictive mode preserved", os.stat(priv).st_mode & 0o7777, 0o600)

    # A writable file inside a read-only directory has no room for a sibling
    # temp file; olly falls back to an in-place write rather than refusing.
    d = tempfile.mkdtemp(dir=tmpdir)
    rod = os.path.join(d, "rod")
    os.makedirs(rod)
    target = os.path.join(rod, "c.txt")
    with open(target, "w") as fh:
        fh.write("data\n")
    os.chmod(rod, 0o555)
    try:
        run([OLLY, target], ["X", SAVE])
        with open(target) as fh:
            check("writable file in read-only dir still saves",
                  fh.read(), "Xdata\n")
    finally:
        os.chmod(rod, 0o755)

    print("\nsave: large files round-trip through the write buffer")
    # Save batches rows into a 64 KB buffer. Rows that exactly fill it, spill
    # one byte past it, and dwarf it take three different paths. Kept to a few
    # thousand rows: loading grows the row array one row at a time, which is
    # quadratic under allocators that always move on realloc (ASan's among
    # them), and the suite should stay usable with a sanitizer build.
    rows = ["line %d %s" % (i, "x" * (i % 97)) for i in range(3000)]
    rows[1000] = "a" * 65535
    rows[1001] = "b" * 65536
    rows[1002] = "c" * 200000
    body = "\n".join(rows) + "\n"
    check("rows at and past the buffer size save intact",
          edit(tmpdir, body, ["X", SAVE], name="big.txt") == "X" + body, True)

    print("\nsave: content round-trips")
    check("NUL bytes survive",
          edit(tmpdir, "ab\x00cd\n", [SAVE]), "ab\x00cd\n")
    check("empty buffer saves", edit(tmpdir, "", [SAVE]), "")


# ------------------------------------------------ external file change ----

def test_external_change(tmpdir):
    print("\nsave: warns before overwriting a file changed on disk")
    path = os.path.join(tmpdir, "ext.txt")

    def touched(new_content, delay=0.05):
        def touch():
            time.sleep(delay)
            with open(path, "w") as fh:
                fh.write(new_content)
        return touch

    with open(path, "w") as fh:
        fh.write("orig\n")
    out = run_interleaved([OLLY, path], ["X"], touched("external\n"),
                           [SAVE, "n", "\r"])
    msgs = status_lines(out)
    check("declining reports the save was aborted",
          any(m.startswith("Save aborted: file changed on disk")
              for m in msgs),
          True)
    with open(path) as fh:
        check("declining leaves the externally-written content on disk",
              fh.read(), "external\n")

    with open(path, "w") as fh:
        fh.write("orig\n")
    out = run_interleaved([OLLY, path], ["X"], touched("external\n"),
                           [SAVE, "y", "\r"])
    msgs = status_lines(out)
    check("confirming reports bytes written",
          any(m.endswith("bytes written") for m in msgs), True)
    with open(path) as fh:
        check("confirming overwrites with the editor's own content",
              fh.read(), "Xorig\n")

    result = edit(tmpdir, "orig\n", ["X", SAVE], name="ext.txt")
    check("no external change means no warning at all", result, "Xorig\n")


# ------------------------------------------------------------ security ----

def _attacker(stop, link_path, victim):
    while not stop.is_set():
        try:
            os.symlink(victim, link_path)
        except OSError:
            pass


def test_symlink_race(tmpdir):
    print("\nsecurity: the save temp file must not be hijackable")
    # Regression: the temp file had a predictable name and was opened without
    # O_EXCL after an unlink. A local attacker who could create files in the
    # directory could win the race between unlink and open, redirecting the
    # write through a symlink -- an arbitrary file overwrite as the editing
    # user, and the attacker's symlink then got renamed over the real file.
    d = tempfile.mkdtemp(dir=tmpdir)
    doc = os.path.join(d, "doc.txt")
    victim = os.path.join(d, "victim.txt")
    link = os.path.join(d, "doc.txt.tmp")
    with open(doc, "w") as fh:
        fh.write("data\n")
    with open(victim, "w") as fh:
        fh.write("SECRET-ORIGINAL\n")

    stop = multiprocessing.Event()
    procs = [multiprocessing.Process(target=_attacker, args=(stop, link, victim))
             for _ in range(4)]
    for p in procs:
        p.start()
    keys = []
    for _ in range(25):
        keys += ["Z", SAVE, 0.05]
    try:
        run([OLLY, doc], keys, settle=1.0)
    finally:
        stop.set()
        for p in procs:
            p.join(timeout=2)
            if p.is_alive():
                p.terminate()

    with open(victim) as fh:
        check("attacker cannot redirect the write", fh.read(),
              "SECRET-ORIGINAL\n")
    check("attacker cannot replace the real file with a symlink",
          os.path.islink(doc), False)


# ------------------------------------------------------------- signals ----

def test_signals(tmpdir):
    print("\nsignals: a killed editor hands back a usable terminal")
    # Regression: a fatal signal skips atexit, so closing the terminal window
    # or a plain `kill` left the terminal in raw mode -- no echo, no line
    # editing -- until the user blindly typed `reset`.
    path = os.path.join(tmpdir, "sig.txt")
    with open(path, "w") as fh:
        fh.write("data\n")
    for name in ("SIGTERM", "SIGHUP", "SIGINT"):
        sig = getattr(signal, name)
        before, during, after, st = run_until_signal([OLLY, path], sig)
        check(name + ": the editor had switched the terminal to raw mode",
              during != before, True)
        check(name + ": terminal modes restored", after, before)
        check(name + ": still exits by the signal",
              os.WIFSIGNALED(st) and os.WTERMSIG(st) == sig, True)


# ---------------------------------------------------------------- utf-8 ----

def test_utf8(tmpdir):
    print("\nutf-8: editing steps over whole characters")
    cafe = "café\n".encode("utf-8")  # é is two bytes
    # Regression: backspace/delete used to remove a single byte, leaving an
    # invalid UTF-8 sequence behind (café -> "caf\xc3").
    check("backspace removes a whole multibyte char",
          edit_bytes(tmpdir, cafe, [END, "\x7f", SAVE]), b"caf\n")
    check("forward delete removes a whole multibyte char",
          edit_bytes(tmpdir, cafe, [HOME, RIGHT, RIGHT, RIGHT, "\x1b[3~", SAVE]),
          b"caf\n")
    check("right arrow crosses a multibyte char in one step",
          edit_bytes(tmpdir, "aéb\n".encode(), [HOME, RIGHT, RIGHT, "X", SAVE]),
          "aéXb\n".encode())
    check("undo restores a deleted multibyte char",
          edit_bytes(tmpdir, cafe, [END, "\x7f", "\x1a", SAVE]), cafe)

    path = os.path.join(tmpdir, "col.txt")
    with open(path, "wb") as fh:
        fh.write(cafe)
    # café is four columns but five bytes; the status bar's Col reports the
    # display column (5 at end of line), never the byte position (6).
    out = run([OLLY, path], [END])
    check("column reflects display position, not bytes",
          b"Col 5" in out and b"Col 6" not in out, True)
    found = (status_lines(run([OLLY, path], [HOME, "\x06", "é", "\r"]))
             or [""])[-1]
    check("a multibyte search term is accepted and found",
          found.startswith("Found"), True)


# ------------------------------------------------- wide-char rendering ----

def test_wide(tmpdir):
    print("\nwide chars: the cursor column tracks display width, not bytes")

    def cursor_after(body, keys):
        path = os.path.join(tmpdir, "w.txt")
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        return cursor_col(run([OLLY, path], keys))

    # A wide glyph (U+3042, HIRAGANA A) occupies two columns but three bytes.
    # End of "aあb" is at display column 4, so the cursor sits at screen col 5
    # -- byte counting would wrongly place it at 6.
    check("wide glyph counts as two columns",
          cursor_after("aあb\n", [END]), 5)
    # A combining mark (U+0301) adds no column: "e" + acute is one column wide.
    check("combining mark counts as zero columns",
          cursor_after("é\n", [END]), 2)
    # Stepping onto a wide glyph lands the cursor past its full width.
    check("cursor steps across a wide glyph by two columns",
          cursor_after("あい\n", [HOME, RIGHT]), 3)
    # In a narrow window full of wide glyphs the cursor never runs off the
    # right edge (a split glyph is dropped, not half-drawn).
    col = cursor_after("あ" * 40 + "\n", [END], )
    check("cursor stays within a narrow window of wide glyphs",
          col is not None and col <= 80, True)


def test_status_col(tmpdir):
    print("\nstatus bar: Col reports the character column, not the display "
          "column")

    def status_col(body, keys):
        path = os.path.join(tmpdir, "sc.txt")
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        text = run([OLLY, path], keys).decode("utf-8", "replace")
        m = re.findall(r"Ln \d+, Col (\d+)", text)
        return int(m[-1]) if m else None

    # Col is the 1-based index of the Nth character in the line -- a tab or a
    # double-width glyph is one character, regardless of how many terminal
    # columns it occupies on screen. It intentionally does NOT match the
    # cursor's on-screen column on a line with a tab or a wide glyph (that
    # number is tracked separately, internally, as the render column).
    check("plain ASCII end of line", status_col("hello\n", [END]), 6)
    check("a leading tab is one character",
          status_col("\thello\n", [END]), 7)
    check("a tab mid-line is one character",
          status_col("a\tb\n", [END]), 4)
    check("a wide glyph is one character",
          status_col("aあb\n", [END]), 4)
    check("home is always column 1", status_col("\thello\n", [END, HOME]), 1)


# ---------------------------------------------------------- file format ----

def test_line_endings(tmpdir):
    print("\nfile format: line endings and final newline are preserved")
    # Regression: CRLF was rewritten to LF, and a missing final newline was
    # added, so a Windows-origin file showed as entirely modified in git.
    check("a CRLF file stays CRLF",
          edit_bytes(tmpdir, b"one\r\ntwo\r\n", [SAVE]), b"one\r\ntwo\r\n")
    check("editing a CRLF file keeps CRLF",
          edit_bytes(tmpdir, b"one\r\ntwo\r\n", ["X", SAVE]),
          b"Xone\r\ntwo\r\n")
    check("a missing final newline stays missing",
          edit_bytes(tmpdir, b"no newline", [SAVE]), b"no newline")
    check("an LF file stays LF", edit_bytes(tmpdir, b"a\nb\n", [SAVE]),
          b"a\nb\n")

    work = tempfile.mkdtemp(dir=tmpdir)
    cwd = os.getcwd()
    os.chdir(work)
    try:
        run([OLLY, "new.txt"], ["hi", SAVE])
        with open(os.path.join(work, "new.txt"), "rb") as fh:
            check("a brand-new file is saved with LF and a trailing newline",
                  fh.read(), b"hi\n")
    finally:
        os.chdir(cwd)


# ------------------------------------------------------------ recovery ----

def test_recovery(tmpdir):
    print("\nrecovery: a signal with unsaved changes writes a recovery file")
    path = os.path.join(tmpdir, "doc.txt")
    with open(path, "w") as fh:
        fh.write("original\n")
    rec = path + ".olly-recover"

    # Regression/feature: work used to vanish when a signal killed the editor.
    run_until_signal([OLLY, path], signal.SIGTERM, keys=["HELLO"])
    check("recovery file is created", os.path.exists(rec), True)
    if os.path.exists(rec):
        with open(rec) as fh:
            check("recovery file holds the unsaved buffer",
                  fh.read(), "HELLOoriginal\n")
        os.remove(rec)

    with open(rec, "w") as fh:
        fh.write("stale\n")
    run([OLLY, path], ["X", SAVE])
    check("a successful save clears the recovery file",
          os.path.exists(rec), False)

    run_until_signal([OLLY, path], signal.SIGTERM)
    check("no recovery file when nothing is unsaved",
          os.path.exists(rec), False)


# --------------------------------------------------------- robustness ----

def test_directory_refused(tmpdir):
    print("\nrobustness: a directory is refused, not opened as a buffer")
    d = tempfile.mkdtemp(dir=tmpdir)
    _, st = run([OLLY, d], [SAVE, "\x11\x11\x11\x11"], status=True)
    check("opening a directory exits with an error",
          os.WIFEXITED(st) and os.WEXITSTATUS(st) == 1, True)


# ---------------------------------------------------------------- undo ----

def test_undo(tmpdir):
    print("\nundo: a buffer undone to the start matches the original")
    undo = "\x1a"
    cases = [
        ("typed run undoes as one step", "hello world\n", ["abc"], 1),
        ("enter at column 0", "hello world\n", ["\r"], 1),
        ("enter mid-line", "hello world\n", [RIGHT * 3, "\r"], 1),
        ("backspace joining lines", "one\ntwo\n", [DOWN, "\x7f"], 1),
        ("delete joining lines", "one\ntwo\n", [END, "\x1b[3~"], 1),
        ("backspace run", "hello\n", [END, "\x7f\x7f\x7f"], 1),
        ("mixed edits", "abc\ndef\n", ["x", "\r", "y", DOWN, "z"], 5),
    ]
    for label, original, keys, undos in cases:
        check(label, edit(tmpdir, original, keys + [undo] * undos + [SAVE]),
              original)


def main():
    global OLLY
    OLLY = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./olly")
    if not os.path.exists(OLLY):
        sys.exit("no olly binary at %s -- run make first" % OLLY)

    tmpdir = tempfile.mkdtemp(prefix="olly-tests-")
    try:
        test_keys(tmpdir)
        test_tab(tmpdir)
        test_utf8(tmpdir)
        test_wide(tmpdir)
        test_status_col(tmpdir)
        test_search(tmpdir)
        test_case_sensitive_search(tmpdir)
        test_goto_line(tmpdir)
        test_replace(tmpdir)
        test_save(tmpdir)
        test_external_change(tmpdir)
        test_line_endings(tmpdir)
        test_symlink_race(tmpdir)
        test_signals(tmpdir)
        test_recovery(tmpdir)
        test_directory_refused(tmpdir)
        test_undo(tmpdir)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\n%d passed, %d failed" % (PASSED, len(FAILURES)))
    for f in FAILURES:
        print("  failed: %s" % f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
