# Olly

A small, dependency-free text editor for the terminal, written in C11.

Olly is a modeless editor in the spirit of nano: start typing and you're
editing. It offers incremental search that wraps around the file, grouped
undo/redo, automatic empty-file creation, and careful, atomic save handling. 
All this in a single C file with zero external dependencies.

## Features

- **Modeless editing**: type to insert, arrow keys to move, `Ctrl` keys act.
- **Search, with a case-sensitivity toggle**, that wraps around the file and
  remembers the last term:
  - `Ctrl-F` search, `Ctrl-N` next match, `Ctrl-P` previous match, or just
    press `Ctrl-F` again with an empty query to repeat.
  - `Ctrl-T` toggles between case-insensitive (the default) and
    case-sensitive matching.
- **Search and replace**: `Ctrl-R` replaces the next match or every match,
  and a whole replace-all undoes or redoes as one step.
- **Go to line**: `Ctrl-G` jumps straight to a line number.
- **Grouped undo/redo**: a run of typed (or deleted) characters, or a
  replace-all, undoes as one step. `Ctrl-Z` undo, `Ctrl-Y` redo.
- **Selection and clipboard**: hold `Shift` and use the arrows, `Home`/`End`
  or `PgUp`/`PgDn` to select (the covered text is highlighted in reverse video);
  `Ctrl-C` copies, `Ctrl-X` cuts and `Ctrl-V` pastes. Typing or deleting over a
  selection replaces it, and a cut or paste undoes as one step. Copy and cut
  also place the text on the system clipboard (OSC 52), where the terminal
  supports it.
- **Atomic saves**: the file is written to a private temporary file and
  renamed into place, so an interrupted save never leaves it truncated. Line
  endings (`LF` or `CRLF`), a missing final newline, and the file's mode and
  owner are all preserved. If the file changed on disk since it was opened,
  Olly asks before overwriting it.
- **UTF-8-aware editing**: the cursor, `Backspace` and `Delete` move over
  whole characters, text round-trips as raw bytes, and display width is
  honoured so wide (CJK) and combining characters stay aligned on screen.
- **Crash recovery**: if a signal or an out-of-memory condition kills Olly
  with unsaved changes, the buffer is written to a `.olly-recover` file.
- **Open anything**: never errors on a missing file; it simply starts an
  empty buffer.
- **Dirty-quit protection**: when there are unsaved changes, `Ctrl-Q` asks
  several times before exiting (counted down in the status bar).
- **Keyboard reference built in**: press `Ctrl-?` at any time.
- No runtime dependencies. Compiles with any C11 compiler.

## Install

Just run:

```sh
make
```

Then put it somewhere on your `PATH`:

```sh
make install
```

Its default install location is `/usr/local/bin`; if that directory isn't
writable (a very common situation), `make install` detects this and falls
back to `~/.local/bin` automatically - no `sudo` needed. You can override the
target with `make install PREFIX=/some/dir` or stage a package with
`make install DESTDIR=/wherever`. `make uninstall` to remove it.

## Usage

```sh
olly                 # start with an unnamed buffer
olly file.txt        # open an existing file
olly new.txt         # open a buffer for a file that doesn't exist yet
```

Edit immediately. Every key you type is inserted at the cursor. Save with
`Ctrl-S`, search with `Ctrl-F`, select with `Shift` + the arrows, and press
`Ctrl-?` for a key reference. Keep an eye on the status bar: it shows the file
name, line count, modified state, and the cursor as `Ln <line>, Col <column>`
(plus a `Sel <bytes>` count while text is selected). The display — including the
help screen, which reflows between one and two columns — resizes live as you
resize the terminal window.

## Keyboard shortcuts

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save |
| `Ctrl-Q` | Quit (asks several times with unsaved changes) |
| `Ctrl-F` | Search forward |
| `Ctrl-N` | Find next match |
| `Ctrl-P` | Find previous match |
| `Ctrl-T` | Toggle case-sensitive search |
| `Ctrl-R` | Search and replace (next or all) |
| `Ctrl-G` | Go to line |
| `Ctrl-C` | Copy selection |
| `Ctrl-X` | Cut selection |
| `Ctrl-V` | Paste at the cursor |
| `Ctrl-Z` | Undo |
| `Ctrl-Y` | Redo |
| `Ctrl-?` | Help screen |
| `Ctrl-L` | Repaint |
| `Esc` | Cancel a prompt |
| Arrows / `Home` / `End` / `PgUp` / `PgDn` | Move around |
| `Shift` + Arrows / `Home` / `End` / `PgUp` / `PgDn` | Extend selection |
| `Enter`, `Backspace`, `Delete` | Edit (replaces any selection) |

The full write-up — including the grouped-undo rules, the fallback install
details, and the file-format notes — is in [`MANUAL.md`](MANUAL.md).

## Acknowledgements

Olly builds on the architecture of the classic [kilo] editor by antirez;
the inherited portions are credited in [`NOTICE`](NOTICE.md).

[kilo]: https://github.com/antirez/kilo

## License

MIT — see [`LICENSE`](LICENSE).
