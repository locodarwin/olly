# Olly — Selection, Marking, and Copy/Paste: Implementation Guide

> **What this doc is.** Olly's next feature is text selection with
> cut/copy/paste. This file was written to be handed to a session that has
> **no memory** of how the editor got here. It maps the parts of `olly.c`
> you'll touch (with line numbers as of the clone this was written against),
> lays out a design, sequences the work into independently-shippable
> milestones, and flags the one genuinely risky piece plus the open
> decisions that need a human call before coding.
>
> **Status:** M1 done (selection input + state). M2 done (reverse-video
> highlight of the selected range, drawn in `editor_draw_text_row`). M3 done
> (internal clipboard: cut/copy/paste, replace-on-type). M4 done (copy/cut also
> push the selection to the system clipboard via OSC 52, so a terminal-native
> paste matches what Olly copied; bracketed paste is disabled at startup so a
> paste lands as literal text). M5 (full bracketed-paste parsing) pending. Also
> done: the help screen now reflows live on a terminal resize (`WINCH` re-reads
> the size and recomputes the layout).
> Companion to the "Planned next" section of
> [`olly_project_plan.md`](../olly_project_plan.md).

---

## 0. Non-negotiable project rules

Every one of these is already enforced by the codebase and the test suite;
this feature must not regress any of them.

- **Single C file, C11, zero external dependencies.** `olly.c` is ~2,231
  lines and stays that way (it grows, but no new files, no libraries). libc
  only. Build: `make` → `cc -Wall -Wextra -O2 -std=c11`.
- **Out-of-memory is fatal, routed through `die()`.** Never `malloc`
  directly — use `xmalloc` / `xrealloc` / `xstrdup` (olly.c:146-162). They
  call `die()` (olly.c:135) which writes a recovery file and restores the
  terminal. A raw `malloc` that returns NULL and gets dereferenced is a bug.
- **The test suite must stay green.** `make test` runs
  `tests/test_olly.py ./olly` (currently **164 passing**). Every milestone
  below ends with new tests added and the whole suite passing. `make test-oom`
  runs the allocation-failure sweep — any new allocation path needs its
  injection point added there too.
- **Static analysis stays clean.** The project is kept free of new
  scan-build / `gcc -fanalyzer` / clang-tidy findings. Prefer the existing
  idioms.
- **Docs are part of the feature.** `README.md` and `MANUAL.md` document every
  key binding; `editor_help()` (olly.c:1992) has a built-in reference. A new
  key that isn't in all three is incomplete.

---

## 1. Mental model of the parts you'll touch

Read these before writing code. All line numbers are in `olly.c`.

### 1.1 State

- `struct editorConfig E` (olly.c:49-65, global at olly.c:94) holds cursor
  `cx`/`cy` (byte offsets into `row->chars`), display col `rx`, scroll offsets
  `rowoff`/`coloff`, screen size, `numrows`, `dirty` (an int counter, not a
  flag — see §5.4), `filename`, EOL style, and the saved `termios`.
- `erow` (olly.c:41-47): `chars`/`size` is the **raw** text; `render`/`rsize`
  is the tab-expanded form used for display and search. `editor_update_row`
  (olly.c:631) rebuilds `render` from `chars`. **`cx`/`cy` are always byte
  offsets into `chars`, never `render`.** Selection anchors must be too.
- `struct undoState U` (olly.c:84-92, global at olly.c:95): undo/redo stacks
  of `struct uchange *`, plus `locked` and the group fields (§4).

### 1.2 Input — `editor_read_key` (olly.c:530-599)

- A non-ESC byte is returned as-is. ESC then peeks the next byte:
  - `[` = CSI: reads parameter bytes into `params[]` (olly.c:553-561), then a
    `switch (final)` on the terminating byte returns `ARROW_*` / `HOME_KEY` /
    `END_KEY` / `PAGE_*` / `DEL_KEY` (olly.c:563-581). For `~` it `atoi`s the
    params to distinguish Home/Del/End/PgUp/PgDn.
  - `O` = SS3 (application cursor mode), olly.c:585-596.
  - Anything else returns `'\x1b'` — **unmapped sequences are swallowed whole,
    never typed.** This is load-bearing; there are tests for it
    ("unmapped escape sequences are swallowed", test_olly.py:159-170).
- **The modifier problem (corrected from the plan).** The comment at
  olly.c:550-552 says "Ctrl-Right is `\x1b[1;5C`" and the sequence *is* fully
  consumed, but **the modifier is never parsed** — only `final` (olly.c:563)
  selects the key, and `params` is only ever `atoi`'d for the `~` form. So
  today `Shift+Right` (`\x1b[1;2C`) returns plain `ARROW_RIGHT`, indistinguishable
  from `Right`. **We are writing the modifier extraction, not just un-ignoring
  an existing parse.** This is the single correction to the plan's difficulty
  read; it is still small, but budget for it.

### 1.3 Cursor movement — `editor_move_cursor` (olly.c:1183-1222)

Pure `E.cx`/`E.cy` mutation by whole UTF-8 characters (via `utf8_prev`,
`utf8_char_bytes`), then a clamp pass (olly.c:1211-1221) that keeps `cx` on a
character boundary and within the row. `ARROW_UP`/`DOWN` do **not** preserve
the target column across a vertical move (kilo behavior) — note this if you
want selection over multiple lines to feel right.

### 1.4 Editing primitives (the copy/paste building blocks)

- Row-level: `editor_row_insert_char` / `editor_row_insert_n` (olly.c:716,742),
  `editor_row_delete_char` / `editor_row_delete_n` (olly.c:734,752). These
  mutate `chars`, call `editor_update_row`, bump `E.dirty`. **They do not touch
  undo.** Undo is layered on top by the callers.
- Row add/remove: `editor_insert_row` (olly.c:673), `editor_delete_row`
  (olly.c:708).
- Char-level with undo (the "user pressed a key" layer): `editor_insert_char`
  (olly.c:1017), `editor_insert_newline` (olly.c:1052), `editor_delete_char`
  (olly.c:1082, backspace), `editor_del_char` (olly.c:1138, forward delete).
  **Copy/cut/paste must sit at this layer** (or above) so they undo.
- Byte-range helpers you'll reuse for multi-line copy: a selection can span
  rows, so you'll extract `row->chars` slices with `editor_row_delete_n` /
  `editor_insert_n`, and join/split rows the way `editor_delete_char`'s
  cross-row branch does (olly.c:1120-1133).

### 1.5 Undo and the group primitive — the gift from replace-all

- `struct uchange` (olly.c:73-82) has a `group` field; `uc_new` stamps it with
  `U.active_group` (olly.c:775).
- `undo_group_begin()` / `undo_group_end()` (olly.c:813-824): every uchange
  created between them shares one id, so `editor_undo`/`editor_redo`
  (olly.c:908-944) pop/replay the whole run as a single step.
- **`editor_replace` (olly.c:1639-1718) is the template.** It calls
  `undo_group_begin()` before the first mutation, pushes raw `UC_DELETE` /
  `UC_INSERT` uchanges via `uc_new` + `undo_push` (olly.c:1685-1701), mutates
  with `editor_row_delete_n`/`editor_row_insert_n`, then `undo_group_end()`.
- **A cut, a paste, and (if you make it one) a paste-block are exactly
  replace-all's shape**: N row-scoped deletes + inserts that must collapse to
  one undo step. Reuse `undo_group_begin/end` verbatim; you likely won't add a
  single new undo primitive.
- **Coalescing gotcha:** `undo_group_end()` sets `U.locked = 1` (olly.c:823) so
  the next typed char doesn't merge into the group's trailing insert (the
  `!U.locked` coalescing checks live in `editor_insert_char` olly.c:1032 and
  `editor_delete_char`/`del_char`). If you build a cut/paste by hand rather
  than in one function, mirror this.

### 1.6 Render path and **the risky part**

- `editor_refresh_screen` (olly.c:1375-1421): each frame it clears the cursor,
  calls `editor_draw_rows` → `editor_draw_status_bar` → `editor_draw_message_bar`
  into one `abuf`, then positions the hardware cursor (olly.c:1414-1416).
- `editor_draw_text_row` (olly.c:1252-1289): walks `row->render` by *display
  width*, honors horizontal scroll (`coloff`) and screen width, builds a
  per-row slice in the `drawbuf` scratch buffer, and hands the finished bytes
  to `cache_line_draw` (olly.c:1288). Wide glyphs split by an edge become
  leading spaces (olly.c:1264-1271) — respect this or CJK alignment breaks.
- `cache_line_draw` (olly.c:377-399) is the **diff-based cache**: it compares
  the *rendered byte string* for row `y` against last frame and skips the
  redraw if identical. `force=1` bypasses the comparison. `cache_invalidate`
  (olly.c:345) nukes everything (used on full redraw).
- **Why selection highlighting is the hard part.** The highlight is drawn by
  injecting reverse-video escapes (`\x1b[7m` … `\x1b[27m`) into the row bytes.
  Two failure modes to reason about:
  1. The comparison at olly.c:381-382 compares raw bytes; escape codes *are*
     bytes, so a row that gains/loses a highlight **does** change its byte
     string and normally invalidates correctly. The subtle bug is the
     **cursor-only rows**: when the selection moves but the underlying text is
     identical *and* your highlight bytes happen to reproduce the previous
     frame's bytes, or when a row leaves the selection on a frame where you
     forget to emit the un-highlighted bytes.
  2. The cache stores the byte string **including** the escape codes and
     compares `len` then `memcmp`. Mixing variable-length escapes into a cached
     row means the cached length is no longer a proxy for text length — fine,
     but be deliberate.
  - **The sidestep (what the plan recommended, and what I'd do).** Rather than
    make the cache highlight-aware, keep a **previous-selection bounding
    box** (set of rows that were drawn highlighted last frame). Each frame,
    after computing the new selection, `force`-redraw the union of rows in
    (previous selection) ∪ (current selection). `cache_line_draw(..., force=1)`
    already exists for exactly this (welcome screen uses it, olly.c:1307/1319/1322).
    This bounds the correctness argument to "the rows I touched," sidesteps the
    diff-cache interaction entirely, and costs one forced redraw per row that
    entered or left the selection. It is slightly less efficient than a
    perfectly-integrated cache key but is *correct by construction* and tiny.
    Do **not** try to thread highlight state into `cache_line_draw`'s key as a
    first pass — that's the "solve it fully" path the plan explicitly chose to
    avoid.

- **M2 — what was actually built (done).** The simple diff-cache path turned
  out to be correct without a bounding box, so the sidestep was unnecessary:
  - `selection_row_rx(filerow, &s0, &s1)` (near `selection_bytes`) maps the
    anchor/cursor pair to an **absolute display-column** range for one row via
    `editor_row_cx_to_rx`, so the highlight lands on rendered glyphs (tab stops
    and wide chars already accounted for). Top row runs anchor→EOL, bottom row
    start→far end, interior rows full.
  - `editor_draw_text_row` now takes `(sel0, sel1)` and emits `\x1b[7m`/
    `\x1b[27m` only when a character's column range overlaps `[sel0,sel1)`.
    The escapes carry **no display columns**, so alignment is untouched, and
    when there is no selection the emitted bytes are byte-for-byte what the old
    `memcpy` path produced — so a no-selection frame never repaints.
  - Cache interaction: because the escape bytes live in `drawbuf`, gaining or
    losing a highlight changes the row's cached byte string and the row
    redraws on its own; clearing the selection reverts the bytes and forces the
    un-highlighted redraw. No `force=1` bounding box needed.
  - **Gotchas hit:** (1) size `drawbuf` against `rs` (render length), not the
    visible width — a run of zero-width combining marks consumes every
    iteration without advancing the visible-column counter, so the old
    `screencols`-bounded sizing could overflow; (2) advance `dcol` in the
    left-edge straddle branch too, or the highlight is off by the straddled
    glyph's width; (3) the **status bar also uses `\x1b[7m`** but resets with
    `\x1b[m`, so tests key off `\x1b[27m` (emitted only by the row highlight)
    to avoid a false positive.

---

## 2. Design decisions — **needs a human call before coding**

These change the code you write, so they're asked up front. My recommendation
is first in each.

- **Selection model: anchor + cursor (recommended).** Keep an `E.sel_active`,
  and an anchor `(E.sx, E.sy)` = fixed end; the moving end is always the live
  cursor `(E.cx, E.cy)`. Highlight the range between them. This is what every
  GUI editor and `nano` do, and it means selection state is just "is there one,
  and where's the anchor" — cursor movement stays untouched (you only *extend*
  by moving the cursor with shift held and *not* clearing the anchor; you
  *clear* it on any non-shift move, edit, or click-equivalent). Alternative: an
  explicit start/end rectangle — more flexible, more state to keep consistent.
  Recommend anchor+cursor.
- **Anchor normalization.** Store the anchor as typed, but always compute a
  `sel_start ≤ sel_end` pair (by `(cy,cx)` lexicographic) at use time, so
  copy/cut/Highlight don't each re-derive direction.
- **Internal first, OSC 52 layered on — DECIDED (both in scope).** Build and
  test the internal clipboard first (M3); OSC 52 copy-out is confirmed in-scope
  for this pass (M4). Internal-only is testable in the harness (which has no
  real clipboard); OSC 52 is a fire-and-forget escape sequence — assert the
  emitted bytes, not the clipboard itself.
- **OSC 52 and base64 — DECIDED: include.** OSC 52 wants base64; libc has none
  and a dependency is off the table, so add a **hand-rolled static base64**
  encoder (~30 lines). OSC 52 has a per-terminal size limit (~74 KB encoded):
  cap and report via status message rather than silently truncating.
- **Bracketed paste on/off.** Enabling `\x1b[?2004h` gives a clean paste
  boundary (start `\x1b[200~`, end `\x1b[201~`) so a pasted block becomes one
  undo group and control chars survive. Without it, paste already "works" as
  raw typed bytes, and the harness already asserts the on/off *markers* are
  swallowed (test_olly.py:167-168). Enabling it
  changes input handling: you must intercept `200`/`201` in the `~` handler
  (olly.c:570-580) and drain to the terminator. Recommend: **milestone 5,
  optional,** gated on the human wanting bracketed paste vs. keeping the
  simple raw-paste behavior.
- **Delete-selection semantics.** Does typing with a selection replace it
  (GUI/nano-style), and does `Backspace`/`Delete` delete the selection? Recommend
  **yes to both** — a selection with no replace-on-type feels broken. This means
  `editor_insert_char`, `editor_insert_newline`, `editor_delete_char`,
  `editor_del_char` each need a "if selection active, delete it first (as part
  of this undo group), then proceed" prologue.
- **Key bindings — DECIDED.** `Ctrl-C` copy, `Ctrl-X` cut, `Ctrl-V` paste. All
  three are currently **unbound** (no case in `editor_process_keypress`,
  olly.c:2046-2157); `Ctrl-Q` stays quit. With `ISIG` cleared in raw mode
  (olly.c:514) Olly receives byte 3, so `Ctrl-C` binds cleanly.

---

## 3. Feature scope (the four moving parts)

1. **Selection input.** Shift+arrows / Shift+Home/End extend a selection;
   non-shift movement collapses it.
2. **Highlight rendering.** Draw the selected range with reverse video, without
   breaking the diff cache (use the §1.6 sidestep).
3. **Cut / copy / paste.** Internal clipboard; each is one undo group;
   replace-on-type and delete-selection semantics.
4. **Clipboard integration (OSC 52 copy-out; M4, in scope).** Bracketed-paste
   handling for reliable paste-in is deferred (M5).

---

## 4. Milestones (each: implement → test → docs → green → commit)

Ship in this order. Each is independently useful and testable; stop after any.

### M1 — Selection input + state — **DONE**
Implemented (build clean, suite 120 passing 3×, OOM sweep 0 failed, ASan/UBSan
clean, all uncommitted as of this note):
- `E.sel_active` + anchor `E.sx`/`E.sy` (olly.c:57-ish), init in `init_editor`,
  plus `selection_clear()` / `selection_range()` / `selection_bytes()` helpers.
- Shift detection: a file-scope `last_key_shift` set in `editor_read_key`'s CSI
  branch from the `;<code>` modifier field (low bit of `code-1` = Shift), read
  by `editor_process_keypress` right after its own `editor_read_key`. Swallowing
  of unmapped sequences is unchanged (the key is still chosen by `final`).
- Dispatcher: shifted movement sets/extends the selection (anchoring on the
  first shifted move); any other key clears it. Replace-on-type deferred to M3.
- **Observable (testability):** the status bar shows `Sel <n>` while a selection
  is active. Put on the status bar (already force-redrawn every frame, olly.c
  status-bar row) rather than a text highlight, so M1 touches **no** render-cache
  code. M2 adds the actual reverse-video highlight and can keep or drop the
  count. Tests (test_olly.py `test_selection`) read the last status-bar frame.

### M1 original sketch (kept for reference)
- Add to `E` (olly.c:49): `int sel_active; int sx, sy;` (anchor). Init in
  `init_editor` (olly.c:2191). Add a `selection_clear()` helper.
- **Modifier parse:** in `editor_read_key`'s CSI branch (olly.c:563-581), parse
  `params` for a `;<mod>` field. Introduce a way to return "shifted" arrow/home/end.
  Options: (a) a new global `last_key_shift` set by `editor_read_key` and read
  by the dispatcher — least invasive to the key enum; (b) new enum values
  `ARROW_LEFT_SEL` etc. — cleaner types, more dispatch churn. Recommend (a) for
  a single-file editor, mirroring how `WINCH_KEY` smuggles out-of-band state.
  **Preserve swallowing:** unknown sequences must still return `'\x1b'`.
- **Dispatcher (olly.c:2046):** for `ARROW_*`/`HOME_KEY`/`END_KEY`/`PAGE_*`,
  call `editor_move_cursor(c)` then, if shift was held, set `sel_active` (and
  set the anchor on the first shifted move from a non-selection state);
  otherwise `selection_clear()`.
- Tests (test_olly.py): shifted-arrow sequences select (assert via a follow-up
  op or, once M2 lands, via rendered highlight bytes); plain move clears.
- Docs: none user-visible yet (no highlight = invisible). **Do not** advertise
  half-finished keys in help.

### M2 — Highlight rendering (the risky milestone — go here only after M1 tests pass)
- Compute `sel_start`/`sel_end` each frame from `(sx,sy)`+`(cx,cy)`.
- In `editor_draw_text_row` (olly.c:1252), when the row intersects the
  selection, wrap the in-selection columns in `\x1b[7m`/`\x1b[27m` **while
  building `drawbuf`**, keeping all width/`coloff` logic intact.
- Track the **previous selection's row span** in a small static; each frame,
  `force=1` every row in prev∪current so the cache can't go stale (§1.6).
- **Test hardest here:** select within a line, across lines, select off-screen
  (scrolled), select containing tabs and wide glyphs, then move the selection so
  a row *loses* its highlight — assert the un-highlighted bytes actually
  redraw (this is the case the diff cache is most likely to swallow). Run under
  the harness's byte-capture.
- Docs: add selection to MANUAL "Editing" and help screen once visible.

### M3 — Internal clipboard: copy / cut / paste — **DONE**

> **As-built (olly.c):** clipboard lives in `E.clipboard`/`E.clipsize`.
> `selection_dup` (extract, multi-row aware) → `clipboard_set` → free;
> `selection_delete` walks far→near via `editor_delete_char` inside an undo
> group; `editor_paste` inserts char-by-char inside an undo group. Replace-
> on-type/delete-selection handled in `editor_process_keypress` (copy is
> guarded so `Ctrl-C` keeps the selection). **Gotcha:** `clipboard_set` must
> allocate the new buffer *before* `free(E.clipboard)` — freeing first leaves
> a dangling global that the `atexit(editor_cleanup)` run inside `die()` frees
> a second time (OOM double-free; caught by `make test-oom`). Docs updated in
> README.md + MANUAL.md + `editor_help`.

- Global `static char *clip; static size_t clip_len;` (raw bytes, rows joined by
  `\n`; a flag for "copy was whole lines" is optional and nano-like). Free in
  `editor_cleanup` (olly.c:2162).
- `editor_copy()`: extract selection bytes (multi-row aware), stash in `clip`,
  status message, **no buffer mutation, no undo entry.**
- `editor_cut()`: like copy, then delete the selection **inside an undo group**
  (reuse `undo_group_begin`/`end`, olly.c:813/818, exactly like `editor_replace`).
- `editor_paste()`: insert `clip` at cursor inside one undo group; handles
  multi-line (split into `editor_insert_row` + `editor_row_insert_n`).
- Wire `Ctrl-C`/`Ctrl-X`/`Ctrl-V` (currently unbound, olly.c:2046) — pending the
  §2 key-binding confirmation.
- Delete-selection + replace-on-type prologue in the edit primitives (§2).
- Tests: copy/cut/paste round-trip, single and multi-line, NUL bytes in
  selection, paste is one undo step, cut-then-undo restores everything,
  delete-selection semantics.

### M4 — OSC 52 copy-out (stretch)
- Add a static base64 encoder (libc has none; keep it dependency-free).
- On copy (and cut), emit `\x1b]52;c;<base64>\a` via `write_all` (olly.c:403).
  Cap input size (many terminals clamp ~74 KB encoded); report if skipped.
- Consider a `#define`/env opt-out for terminals that mangle OSC 52.
- Tests: assert the exact escape sequence bytes appear on copy (the harness
  captures stdout). Can't test the clipboard itself — test the emitted bytes.

### M5 — Bracketed paste (optional, §2)
- Enable `\x1b[?2004h` in `enable_raw_mode` (olly.c:505) / after `init_editor`;
  disable `\x1b[?2004l` in `disable_raw_mode` (olly.c:455) and the fatal-signal
  reset (olly.c:463-468).
- Intercept params `200`/`201` in `editor_read_key`'s `~` handler
  (olly.c:570-580); on start, drain bytes to the terminator and hand the block
  to a paste-as-one-undo-group path.
- **Re-verify** the existing swallow tests still pass (test_olly.py:167-168 send
  the on/off sequences).

---

## 5. Gotchas

- **`cx` is a `chars` byte offset, not `render`.** Anchor, selection bounds,
  and copy extraction all operate on `chars`; only rendering and search use
  `render`. Don't mix them (see the render↔chars mappers at olly.c:983 and
  olly.c:1000 for how the two differ around tabs).
- **`E.dirty` is a counter, not a bool.** `editor_recompute_dirty` (olly.c:326)
  recomputes it. Cut/paste change content → must affect `dirty`; copy must not.
- **UTF-8 integrity:** never leave `cx` inside a multibyte char. `editor_move_cursor`'s
  clamp (olly.c:1216-1218) is the model; selection normalization should snap the
  same way so cut boundaries don't split glyphs.
- **The phantom line:** `E.cy == E.numrows` is a valid cursor position past the
  last row (e.g. olly.c:1019). Selection anchor/end math must tolerate it.
- **Help/dispatch/help-screen drift:** three places list keys (help olly.c:1992,
  README table, MANUAL tables). Update all three together.
- **Don't break the swallow invariant.** Any new `editor_read_key` parsing must
  consume sequences to their terminator and return a sentinel (or `'\x1b'`) for
  anything unmapped, never leak trailing bytes into the buffer.
- **OOM sweep:** every new allocation needs an injection point in
  `tests/oom_sweep.py` (plan notes 121 points today) or `make test-oom` won't
  cover it.

---

## 6. Verification checklist (before declaring done)

1. `make` clean, no new warnings under `-Wall -Wextra`.
2. `make test` — all prior 112 plus new selection/cut/copy/paste tests green;
   run **3× consecutively** (the plan's bar) to catch render-cache nondeterminism.
3. `make test-oom` — new allocation points added, 0 failures.
4. ASan+UBSan build, zero reports (the plan ran this before).
5. scan-build / `gcc -fanalyzer` / clang-tidy — nothing new.
6. Manual: select + copy/paste with tabs, CJK wide glyphs, combining marks,
   across scrolled-off regions, and empty/1-line buffers.
7. `README.md`, `MANUAL.md`, `editor_help()` all updated.

---

## 7. Decisions locked / still open

**Locked (this session):**
1. **Keys:** `Ctrl-C` copy, `Ctrl-X` cut, `Ctrl-V` paste. `Ctrl-Q` stays quit.
2. **OSC 52:** in scope now (M4), with a hand-rolled base64 encoder; cap+report
   oversized copies.
3. **Bracketed paste (M5):** deferred — re-decide after M3 lands. Raw paste
   works today; revisit only if paste-as-one-undo-group or control-char-in-paste
   turns out to matter.

**Still open (answer at the relevant milestone):**
4. **Whole-line copy:** if a selection runs to end-of-line, should copy/paste
   behave line-wise (nano/yank style)? Affects M3 extraction.
5. **Selection across a horizontal scroll:** highlight only the visible slice
   (recommended, simpler)? Affects M2.
```
