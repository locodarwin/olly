#!/usr/bin/env python3
"""Differential fuzz of replace-all against Python's str.replace.

Run with:  make test    (standalone: python3 tests/fuzz_replace.py ./olly)

Each iteration generates a small file and a search/replace pair, performs a
replace-all in olly, and requires the saved file to be byte-identical to
Python's own left-to-right, non-overlapping replacement (re.sub with an
ASCII-only IGNORECASE, which is exactly the fold Olly implements). Olly's
sweep continues strictly after each inserted replacement, so the two models
should agree on every input.

This targets the bug class behind a real 22 GB incident -- a replacement
containing the search term, which under a wrapping sweep re-matched its own
output forever. Every olly here runs under `ulimit -v`, so a regression
turns into a fast, loud mismatch instead of a melted machine.

Deterministic: case i draws from random.Random(SEED * 1000 + i), so a
failure reproduces with the printed command. Set OLLY_FUZZ_SEED to explore
elsewhere and OLLY_FUZZ_N for more iterations. Tabs are deliberately absent
from the generated text: search matches the tab-expanded render buffer, and
modeling that mapping in Python would duplicate the code under test.
"""

import os
import random
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import run

N = int(os.environ.get("OLLY_FUZZ_N", "40"))
SEED = int(os.environ.get("OLLY_FUZZ_SEED", "42"))
SAVE = "\x13"
QUIT = "\x11"
TOGGLE_CASE = "\x14"
REPLACE = "\x12"

# Accented words exercise the ASCII-only fold (neither str.replace nor Olly
# folds them, but the bytes must still survive the round trip); CJK words
# exercise multibyte-safe deletion and insertion offsets.
VOCAB = ["alpha", "beta", "gamma", "delta", "Cat", "dog", "pea", "Pea",
         "install", "Install", "cafe", "café", "naïve", "über",
         "日", "本", "木", "a", "ab"]
SELF_REFERENTIAL = ["{q}", "{q}{q}", "x{q}", "{q}x"]


def gen_case(rng):
    lines = []
    for _ in range(rng.randint(1, 8)):
        words = [rng.choice(VOCAB) for _ in range(rng.randint(1, 6))]
        if rng.random() < 0.4:
            w = rng.randrange(len(words))
            words[w] = words[w].capitalize() if rng.getrandbits(1) \
                else words[w].upper()
        lines.append(" ".join(words))
    text = "\n".join(lines) + "\n"

    words = [w for w in re.split(r"\s+", text) if w]
    q = rng.choice(words)
    r = rng.random()
    if r < 0.2:  # two adjacent words on one line: a multi-word query
        i = text.find(q)
        tail = text[i + len(q):]
        if tail.startswith(" "):  # a space (not a newline) means same line
            nxt = tail[1:].split(maxsplit=1)[0]  # stops at any whitespace
            q = q + " " + nxt
    elif r < 0.3:  # a term that usually is absent: exercises "No matches"
        q = rng.choice(VOCAB) + "qq"

    r = rng.random()
    if r < 0.45:
        repl = rng.choice(VOCAB)
    elif r < 0.6:
        repl = ""
    else:
        repl = rng.choice(SELF_REFERENTIAL).format(q=q)

    return text, q, repl, rng.random() < 0.4


def expected(text, q, repl, case_sensitive):
    if case_sensitive:
        return text.replace(q, repl)
    # re.ASCII confines the IGNORECASE fold to A-Z/a-z, matching Olly's
    # ascii_fold exactly. Without it, Python's re folds non-ASCII letters
    # (cafe matching cafe-with-accent), which Olly deliberately never does.
    # The lambda keeps repl literal even when it contains the query.
    return re.sub(re.escape(q), lambda m: repl, text,
                  flags=re.IGNORECASE | re.ASCII)


def main():
    olly = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./olly")
    if not os.path.exists(olly):
        sys.exit("no olly binary at %s -- run make first" % olly)

    tmpdir = tempfile.mkdtemp(prefix="olly-fuzz-")
    path = os.path.join(tmpdir, "f.txt")
    failures = 0

    for i in range(N):
        rng = random.Random(SEED * 1000 + i)
        text, q, repl, case_sensitive = gen_case(rng)
        want = expected(text, q, repl, case_sensitive)
        hits = len(re.findall(re.escape(q), text,
                              0 if case_sensitive else
                              (re.IGNORECASE | re.ASCII)))

        with open(path, "w") as fh:
            fh.write(text)
        keys = ([TOGGLE_CASE] if case_sensitive else []) + \
               [REPLACE, q, "\r", repl, "\r", "a", "\r", SAVE, QUIT]
        # The ulimit is the safety net: a looping replace-all must die in
        # its tracks rather than eat the machine it fails on.
        out, st = run(["/bin/bash", "-c",
                       "ulimit -v 2000000; exec %s %s" % (olly, path)],
                      keys, status=True)
        with open(path, "r") as fh:
            got = fh.read()

        problems = []
        if got != want:
            problems.append("content\n          want: %r\n          got:  %r"
                            % (want, got))
        # Quitting is the termination witness: after a clean save Ctrl-Q
        # exits 0, while a replace-all that regressed into a loop never
        # reaches it and run() ends in a SIGKILL.
        if not (os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0):
            problems.append("olly did not quit cleanly (status %r)" % st)
        if problems:
            failures += 1
            print("  FAIL  case %d (seed %d): %s"
                  % (i, SEED * 1000 + i, "; ".join(problems)))
            print("          text: %r\n          query: %r  repl: %r  "
                  "case-sensitive: %s" % (text, q, repl, case_sensitive))
            print("          reproduce: OLLY_FUZZ_SEED=%d "
                  "python3 tests/fuzz_replace.py %s" % (SEED, olly))
        else:
            print("  pass  case %d: %d match(es), repl %r%s"
                  % (i, hits, repl, "" if case_sensitive else
                     ", case-insensitive"))

    print("\n%d cases, %d failed (seed %d)" % (N, failures, SEED))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
