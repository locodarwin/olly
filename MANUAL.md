# Olly — A Command-Line Text Editor

Olly is a small, dependency-free text editor for the terminal, written in C.
It provides modaless editing in the style of tools like nano: type to insert,
the arrow keys move around, and the `Ctrl` combinations do the work.

## Table of Contents

- [Building](#building)
- [Installing](#installing)
- [Getting Started](#getting-started)
- [Editing](#editing)
- [Moving Around](#moving-around)
- [Searching](#searching)
- [Undo and Redo](#undo-and-redo)
- [Saving and Quitting](#saving-and-quitting)
- [Keys At a Glance](#keys-at-a-glance)
- [File Format Notes](#file-format-notes)

## Building

Olly builds with any C11 compiler and has no external dependencies.

    make

This produces the `olly` binary in the current directory.

## Installing

    make install

The default install location is `/usr/local/bin`. If that directory is not
writable by your user (a common situation), `make install` detects it and
**automatically falls back to** `~/.local/bin`, which is usually already on
your `PATH` — no `sudo` required.

You can control where the binary goes:

- `make install PREFIX=/some/dir` installs to `/some/dir/bin`.
- `make install DESTDIR=/wherever` installs into a staging directory instead,
  useful for packaging.

To remove the installed binary:

    make uninstall

## Getting Started

    olly                      # start with an unnamed buffer
    olly file.txt             # open an existing file
    olly brand-new.txt        # open a file that does not exist yet (created
                              # empty; saved only when you save it)

If the file does not exist, Olly opens an empty buffer rather than failing.
The status bar (the highlighted line at the bottom) shows the file name, the
number of lines, whether the buffer is modified, and the cursor position as
`Ln <line>, Col <column>`.

## Editing

Olly is modeless: whatever you type is inserted at the cursor.

| Key | Action |
| --- | --- |
| Any character | Insert at the cursor |
| `Enter` | Split the line / insert a new line |
| `Backspace` | Delete the character before the cursor |
| `Delete` | Delete the character at the cursor |

If the cursor is on the empty row below the last line of text, typing creates
that row for you automatically. If the cursor is at the very start of a line,
`Backspace` joins the current line onto the previous one; `Delete` at the very
end of a line joins the next line onto it.

## Moving Around

| Key | Action |
| --- | --- |
| `←` `↑` `↓` `→` | Move the cursor |
| `Home` | Go to the start of the line |
| `End` | Go to the end of the line |
| `PgUp` / `PgDn` | Move up / down a screenful |
| `Ctrl-L` | Repaint the screen |

## Searching

`Ctrl-F` starts a search. Type your query and press `Enter`. Matching is
case-insensitive and begins from the current cursor position; whenever the
search wraps around the end of the file, Olly just keeps going.

| Key | Action |
| --- | --- |
| `Ctrl-F` | Start a search; `Enter` finds the first match |
| `Ctrl-F` `Enter` (empty query) | Repeat the last search forward |
| `Ctrl-N` | Find the next instance of the search term |
| `Ctrl-P` | Find the previous instance of the search term |
| `Esc` | Cancel the search prompt |

The same term is remembered for the whole session, so you can keep tapping
`Ctrl-N` / `Ctrl-P` to step through every occurrence. If there is no previous
search, Olly tells you and asks you to press `Ctrl-F` again.

## Undo and Redo

| Key | Action |
| --- | --- |
| `Ctrl-Z` | Undo the last change |
| `Ctrl-Y` | Redo an undone change |

Consecutive typing of characters (and consecutive partial deletions) are
grouped into a single change, so undoing a whole typed word is one press.
Undoing a change puts it back onto the redo stack, so `Ctrl-Y` replays it.

Changes are aggregated this way:

- A run of typed characters undoes as one step.
- A run of backspaces undoes as one step.
- Line splits, line joins, and row insertions likewise undo as one step.

Typing a character on a brand-new auto-created row records two changes (the
row itself, then the character), so it takes two `Ctrl-Z` presses to fully
undo it. Any edit made after an undo clears the redo history — a fresh
`Ctrl-Y` will not resurrect changes replaced by newer ones.

## Saving and Quitting

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save the file |
| `Ctrl-Q` | Quit |

If the buffer has no file name yet, `Ctrl-S` first asks where to save.

Olly protects you from losing work: when there are unsaved changes, the first
few presses of `Ctrl-Q` only confirm that you really want to quit, and a
warning shows how many more presses are needed. A clean buffer quits on the
first `Ctrl-Q`.

After saving, the undo history is kept, but the dirty flag is cleared.

## Keys At a Glance

    Ctrl-S   Save file
    Ctrl-Q   Quit (asks several times if there are unsaved changes)
    Ctrl-F   Search forward (case-insensitive)
    Ctrl-N   Find next instance of the search
    Ctrl-P   Find previous instance of the search
    Ctrl-Z   Undo the last change
    Ctrl-Y   Redo an undone change
    Ctrl-?   Show this help screen
    Ctrl-L   Repaint the screen
    Esc      Cancel the current prompt

## File Format Notes

- Files are saved with a trailing newline on each line (the usual text file
  convention). A one-line buffer containing `hello` is stored as `hello\n`.
- A file that ends without a trailing newline is loaded fine; lines are
  stripped of their line endings when read.
- The status bar shows the file name capped at 20 characters.