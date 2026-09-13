#!/usr/bin/env python3
"""Allocation-failure sweep for olly.

Run with:  make test-oom    (or: python3 tests/oom_sweep.py ./olly [LIMIT])

Replays one editing session again and again, failing a different single
allocation each time -- the 1st, then the 2nd, and so on -- until the session
no longer reaches the failing call (or LIMIT is hit). Whichever allocation
fails, olly must either carry on or exit through die(). It must never crash,
never leave the file half-written, and never leave a temp file behind.

Needs a C compiler for the LD_PRELOAD shim. Slow, so not part of `make test`.
"""

import multiprocessing
import os
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pty_harness import run

ORIGINAL = "alpha\nbeta\ngamma\n"
DOWN = "\x1b[B"
# Type, tab, split a line, undo, move, type again, save: rows, undo history,
# repaints and the save path all allocate along the way.
KEYS = ["X", "\t", "\r", "y", "\x1a", DOWN, "z", "\x13"]


def session(args):
    olly, shim, workroot, fail_at = args
    work = tempfile.mkdtemp(dir=workroot)
    path = os.path.join(work, "f.txt")
    with open(path, "w") as fh:
        fh.write(ORIGINAL)
    hit = os.path.join(workroot, "hit-%d" % fail_at)
    env = {"LD_PRELOAD": shim, "OLLY_FAIL_AT": str(fail_at),
           "OLLY_FAIL_HIT": hit}
    _, st = run([olly, path], KEYS, env=env, status=True)
    with open(path, errors="replace") as fh:
        content = fh.read()
    return fail_at, os.path.exists(hit), st, content, sorted(os.listdir(work))


def outcome(st):
    if os.WIFSIGNALED(st) and os.WTERMSIG(st) == signal.SIGKILL:
        return "kept running"
    if os.WIFEXITED(st) and os.WEXITSTATUS(st) == 1:
        return "exited through die()"
    if os.WIFSIGNALED(st):
        return "CRASHED with " + signal.Signals(os.WTERMSIG(st)).name
    return "exited with status %d" % os.WEXITSTATUS(st)


def main():
    olly = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./olly")
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else None
    if not os.path.exists(olly):
        sys.exit("no olly binary at %s -- run make first" % olly)

    workroot = tempfile.mkdtemp(prefix="olly-oom-")
    failures = []
    tally = {}
    last = 0
    try:
        shim = os.path.join(workroot, "failalloc.so")
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-O2",
                        os.path.join(HERE, "failalloc.c"), "-o", shim],
                       check=True)

        _, _, st, expected, _ = session((olly, shim, workroot, 0))
        if outcome(st) != "kept running" or expected == ORIGINAL:
            sys.exit("the session does not run cleanly even without failures")

        workers = min(16, os.cpu_count() or 1)
        with multiprocessing.Pool(workers) as pool:
            n = 1
            while limit is None or n <= limit:
                batch = range(n, n + workers)
                if limit is not None:
                    batch = range(n, min(n + workers, limit + 1))
                reached = False
                for k, hit, st, content, files in pool.map(
                        session, [(olly, shim, workroot, k) for k in batch]):
                    if not hit:
                        continue
                    reached = True
                    last = max(last, k)
                    result = outcome(st)
                    tally[result] = tally.get(result, 0) + 1
                    problems = []
                    if result not in ("kept running", "exited through die()"):
                        problems.append(result)
                    if content not in (ORIGINAL, expected):
                        problems.append("file left as %r" % content[:60])
                    if files != ["f.txt"]:
                        problems.append("stray files %s" % files)
                    if problems:
                        failures.append(k)
                        print("  FAIL  allocation #%d: %s"
                              % (k, "; ".join(problems)))
                if not reached:
                    break
                n += workers
    finally:
        shutil.rmtree(workroot, ignore_errors=True)

    print("\nfailed each of allocations #1-#%d in turn:" % last)
    for result, count in sorted(tally.items()):
        print("  %5d  %s" % (count, result))
    print("%d failed" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
