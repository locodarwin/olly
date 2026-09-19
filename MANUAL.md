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
- [Selecting Text](#selecting-text)
- [Searching](#searching)
- [Search and Replace](#search-and-replace)
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
A path that exists but cannot be read in full — a directory, for example — is
refused with an error instead of being opened as an empty or partial buffer
that a later save would write back over the original.
The status bar (the highlighted line at the bottom) shows the file name, the
number of lines, whether the buffer is modified, and the cursor position as
`Ln <line>, Col <column>`.

## Editing

Olly is modeless: whatever you type is inserted at the cursor.

| Key | Action |
| --- | --- |
| Any character | Insert at the cursor |
| `Tab` | Insert a tab character |
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
| `Ctrl-G` | Go to a line number |
| `Ctrl-L` | Repaint the screen |

Hold `Shift` while using the arrows, `Home`/`End`, or `PgUp`/`PgDn` to move the
cursor and select text along the way — see [Selecting Text](#selecting-text).

`Ctrl-G` prompts for a line number and moves the cursor to the start of that
line. A number past the last line goes to the last line instead; a number
below 1 goes to the first line. Anything that is not a valid number leaves
the cursor where it was.

The terminal can also be resized freely while Olly is running — the display
adjusts to the new size as it happens, without needing a keypress first.

## Help Screen

`Ctrl-?` opens a keyboard reference. It lays out in two columns on a wide
terminal and drops to a single column when the window is narrower, scrolling
with the arrows, `PgUp`/`PgDn`, `Home`/`End` when the content is taller than the
screen. Resizing the terminal **while the help screen is open** reflows it live —
it re-reads the size and switches between the one- and two-column layouts to
match, without leaving the screen. Press any other key to return to the editor.

## Selecting Text

Hold `Shift` and use the movement keys to select a region between the anchor
(where the selection started) and the cursor. The selected text is shown in
reverse video as you extend it, and the status bar shows a `Sel <bytes>` count
while a selection is active.

| Key | Action |
| --- | --- |
| `Shift` + `←` `↑` `↓` `→` | Extend or shrink the selection |
| `Shift` + `Home` / `End` | Select to the start / end of the line |
| `Shift` + `PgUp` / `PgDn` | Extend the selection up / down a screenful |
| `Ctrl-C` | Copy the selection to the clipboard |
| `Ctrl-X` | Cut the selection (copy, then delete it) |
| `Ctrl-V` | Paste the clipboard at the cursor |

A selection is a rectangular-free, character-wise range: it always grows and
shrinks on whole character boundaries, so it never splits a multi-byte (UTF-8)
character. Pressing any movement key **without** `Shift`, or clicking/editing
elsewhere, clears the selection first.

Editing replaces the selection in one step: typing a character, `Enter`,
`Backspace` or `Delete` deletes the selected range and then does its normal
thing. `Ctrl-C` deliberately leaves the selection in place so you can copy the
same range again.

Pasting inserts the clipboard at the cursor (replacing any current selection);
embedded newlines split lines. A cut and a paste each undo (and redo) as a
single step with `Ctrl-Z` / `Ctrl-Y`.

Copying and cutting also push the selection to your **system (terminal)
clipboard** with the OSC 52 escape sequence, so it can be pasted into other
applications — and pasted back into Olly with the terminal's own paste binding
(right-click, `Ctrl-Shift-V`, …), which is handy when a terminal intercepts
`Ctrl-V` and would otherwise paste an unrelated application's clipboard. This
depends on your terminal honouring OSC 52 (some disable it by default for
security); where it is off, Olly's own `Ctrl-V` still pastes its internal
buffer. Olly does not *read* the system clipboard — `Ctrl-V` pastes only what
Olly last copied or cut.

## Searching

`Ctrl-F` starts a search. Type your query and press `Enter`. Matching begins
from the current cursor position, and whenever the search wraps around the
end of the file, Olly just keeps going.

| Key | Action |
| --- | --- |
| `Ctrl-F` | Start a search; `Enter` finds the first match |
| `Ctrl-F` `Enter` (empty query) | Repeat the last search forward |
| `Ctrl-N` | Find the next instance of the search term |
| `Ctrl-P` | Find the previous instance of the search term |
| `Ctrl-T` | Toggle case-sensitive search |
| `Esc` | Cancel the search prompt |

The same term is remembered for the whole session, so you can keep tapping
`Ctrl-N` / `Ctrl-P` to step through every occurrence. If there is no previous
search, Olly tells you and asks you to press `Ctrl-F` again.

Matching is case-insensitive by default. `Ctrl-T` toggles case-sensitive
matching on or off at any time (not just while a search prompt is open); the
status bar reports which mode is now active. The setting applies to `Ctrl-F`,
`Ctrl-N`, `Ctrl-P`, and search-and-replace alike.

## Search and Replace

`Ctrl-R` prompts for a search term, then a replacement, then asks whether to
replace just the next match (`n`) or every match in the file (`a`).

- **Next** replaces the first match found from the cursor position forward
  and stops there.
- **All** replaces every match in the file, from the start, and reports how
  many occurrences it changed.

A replace-all undoes and redoes as a single step, no matter how many
occurrences it touched — one `Ctrl-Z` puts the whole file back exactly as it
was. Replacement respects the current case-sensitivity setting (`Ctrl-T`).

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
- A replace-all (see [Search and Replace](#search-and-replace)) undoes and
  redoes as one step, however many occurrences it changed.
- A cut (`Ctrl-X`) and a paste (`Ctrl-V`) each undo and redo as one step,
  however many characters or lines they span.

Typing a character on a brand-new auto-created row records two changes (the
row itself, then the character), so it takes two `Ctrl-Z` presses to fully
undo it. Any edit made after an undo clears the redo history — a fresh
`Ctrl-Y` will not resurrect changes replaced by newer ones.

## Saving and Quitting

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save the file |
| `Ctrl-Q` | Quit |

If the buffer has no file name yet, `Ctrl-S` first asks where to save. An
empty answer cancels the save rather than being taken as a file name, so the
buffer keeps its unnamed state and `Ctrl-S` will ask again next time.

### How a save is written

Olly does not write over your file directly. It creates a uniquely named
temporary file beside the target, writes the buffer there, flushes it to disk,
and only then renames it into place. A save that is interrupted — by a crash,
a full disk, or the power going out — therefore leaves the original file
untouched rather than half-written. The temporary name is unpredictable and
created exclusively, so it can never collide with a file of your own and
cannot be redirected by anything else in the directory.

Two consequences are worth knowing:

- Saving replaces the file, so any **hard links** to it are broken: the other
  names keep the old contents.
- Renaming requires a writable **directory**. If the file is writable but its
  directory is not, Olly falls back to rewriting the file in place and says
  `written (in place, not atomic)` — your work is saved, but that particular
  write has no crash protection.

When the file name is a symbolic link, Olly follows it and replaces the file
it points at, leaving the link itself alone.

If the file has changed on disk since Olly opened it (or since the last
save) — another process wrote to it in the meantime — `Ctrl-S` asks before
overwriting that change, rather than silently discarding it. Answering `n`
aborts the save; `y` proceeds and overwrites it as normal.

Olly protects you from losing work: when there are unsaved changes, the first
few presses of `Ctrl-Q` only confirm that you really want to quit, and a
warning shows how many more presses are needed. A clean buffer quits on the
first `Ctrl-Q`.

If Olly is stopped from outside instead — its terminal window is closed, or it
receives `SIGTERM`, `SIGHUP`, `SIGINT` or `SIGQUIT` — it puts your terminal's
settings back on the way out, so the shell you return to echoes and edits
normally.

### Recovery of unsaved work

If Olly is killed by one of those signals, or exits because it ran out of
memory, and the buffer had unsaved changes, it writes what was in the buffer
to a recovery file before it goes. The recovery file sits next to the file you
were editing, named after it with a `.olly-recover` suffix (for an unnamed
buffer it is `olly-recover.<pid>` in the current directory). Nothing opens it
for you — inspect it and rename it over your file if you want to keep it. A
successful save removes any recovery file for that name, and so does a clean
`Ctrl-Q` quit.

After saving, the undo history is kept, but the dirty flag is cleared.

## Keyboard Shortcuts

    Ctrl-S   Save file
    Ctrl-Q   Quit (asks several times if there are unsaved changes)
    Ctrl-F   Search forward (case-insensitive by default)
    Ctrl-N   Find next instance of the search
    Ctrl-P   Find previous instance of the search
    Ctrl-T   Toggle case-sensitive search
    Ctrl-R   Search and replace (next or all)
    Ctrl-G   Go to line
    Ctrl-C   Copy the selection
    Ctrl-X   Cut the selection
    Ctrl-V   Paste the clipboard at the cursor
    Ctrl-Z   Undo the last change
    Ctrl-Y   Redo an undone change
    Ctrl-?   Show this help screen
    Ctrl-L   Repaint the screen
    Shift    Hold with arrows / Home / End / PgUp / PgDn to select
    Esc      Cancel the current prompt

## File Format Notes

- Line endings are preserved. A file loaded with Windows `CRLF` endings is
  saved back with `CRLF`; a Unix `LF` file stays `LF`. A brand-new file uses
  `LF`.
- A trailing newline is preserved too: if the file you opened ended without
  one, the save leaves it without one. A brand-new file is given a trailing
  newline.
- Text is stored and saved as raw bytes, so UTF-8 (and any other encoding)
  round-trips unchanged. The cursor, `Backspace` and `Delete` operate on whole
  UTF-8 characters rather than individual bytes, and the `Col` indicator counts
  characters. Display width is honoured: East Asian wide characters take two
  columns, combining marks take none, so the cursor and horizontal scrolling
  stay aligned with what the terminal actually shows.
- The status bar shows the file name capped at 20 characters.
