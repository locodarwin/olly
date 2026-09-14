# Olly

A small, dependency-free text editor for the terminal, written in C11.

Olly is a modeless editor in the spirit of nano: start typing and you're
editing. It offers incremental search that wraps around the file, grouped
undo/redo, automatic empty-file creation, and careful, atomic save handling —
all in a single C file with zero external dependencies.

## Features

- **Modeless editing** — type to insert, arrow keys to move, `Ctrl` keys act.
- **Case-insensitive search** that wraps around the file and remembers the
  last term:
  - `Ctrl-F` search, `Ctrl-N` next match, `Ctrl-P` previous match, or just
    press `Ctrl-F` again with an empty query to repeat.
- **Grouped undo/redo** — a run of typed (or deleted) characters undoes as
  one step. `Ctrl-Z` undo, `Ctrl-Y` redo.
- **Atomic saves** — the file is written to a private temporary file and
  renamed into place, so an interrupted save never leaves it truncated. Line
  endings (`LF` or `CRLF`), a missing final newline, and the file's mode and
  owner are all preserved.
- **UTF-8-aware editing** — the cursor, `Backspace` and `Delete` move over
  whole characters, text round-trips as raw bytes, and display width is
  honoured so wide (CJK) and combining characters stay aligned on screen.
- **Crash recovery** — if a signal or an out-of-memory condition kills Olly
  with unsaved changes, the buffer is written to a `.olly-recover` file.
- **Open anything** — never errors on a missing file; it simply starts an
  empty buffer.
- **Dirty-quit protection** — when there are unsaved changes, `Ctrl-Q` asks
  several times before exiting (counted down in the status bar).
- **Keyboard reference built in** — press `Ctrl-?` at any time.
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
back to `~/.local/bin` automatically — no `sudo` needed. You can override the
target with `make install PREFIX=/some/dir` or stage a package with
`make install DESTDIR=/wherever`. `make uninstall` removes it.

## Usage

```sh
olly                 # start with an unnamed buffer
olly file.txt        # open an existing file
olly new.txt         # open a buffer for a file that doesn't exist yet
```

Edit immediately — every key you type is inserted at the cursor. Save with
`Ctrl-S`, search with `Ctrl-F`, and keep an eye on the status bar: it shows
the file name, line count, modified state, and the cursor as
`Ln <line>, Col <column>`.

## Keys At a Glance

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save |
| `Ctrl-Q` | Quit (asks several times with unsaved changes) |
| `Ctrl-F` | Search forward |
| `Ctrl-N` | Find next match |
| `Ctrl-P` | Find previous match |
| `Ctrl-Z` | Undo |
| `Ctrl-Y` | Redo |
| `Ctrl-?` | Help screen |
| `Ctrl-L` | Repaint |
| `Esc` | Cancel a prompt |
| Arrows / `Home` / `End` / `PgUp` / `PgDn` | Move around |
| `Enter`, `Backspace`, `Delete` | Edit |

The full write-up — including the grouped-undo rules, the fallback install
details, and the file-format notes — is in [`MANUAL.md`](MANUAL.md).

## Acknowledgements

Olly builds on the architecture of the classic [kilo] editor by antirez;
the inherited portions are credited in [`NOTICE`](NOTICE.md).

[kilo]: https://github.com/antirez/kilo

## License

MIT — see [`LICENSE`](LICENSE).