#!/usr/bin/env python3
"""Small repeatable PTY benchmarks for Olly.

This is diagnostic rather than a CI gate. Run with:
    make bench

The PTY harness deliberately includes terminal startup and input pacing, so
compare runs on the same machine and focus on changes between revisions.
"""

import os
import statistics
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import run

SAVE = "\x13"
QUIT = "\x11"
REPLACE = "\x12"
END = "\x1b[F"


def measure(olly, payload, keys, repeats=5):
    samples = []
    with tempfile.TemporaryDirectory(prefix="olly-bench-") as d:
        path = os.path.join(d, "bench.txt")
        for _ in range(repeats):
            with open(path, "w") as fh:
                fh.write(payload)
            start = time.perf_counter()
            out, status = run([olly, path], keys, status=True, settle=0.2)
            elapsed = time.perf_counter() - start
            if not (os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0):
                raise RuntimeError("Olly did not exit cleanly: %r" % status)
            samples.append(elapsed)
    return statistics.median(samples), min(samples), max(samples)


def report(label, payload, keys, olly):
    median, minimum, maximum = measure(olly, payload, keys)
    print("%-24s median %.3fs  min %.3fs  max %.3fs  bytes %d" %
          (label, median, minimum, maximum, len(payload.encode())))


def main():
    olly = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./olly")
    if not os.path.exists(olly):
        raise SystemExit("no olly binary at %s -- run make first" % olly)

    small = "alpha beta gamma\n" * 100
    large = "alpha beta gamma delta\n" * 10000
    report("load/save small", small, ["X", SAVE, QUIT], olly)
    report("load/save large", large, ["X", SAVE, QUIT], olly)
    report("replace-all large", large, [REPLACE, "alpha", "\r", "ALPHA", "\r",
                                         "a", "\r", SAVE, QUIT], olly)
    report("row insertion", small, [END, "\r", SAVE, QUIT], olly)


if __name__ == "__main__":
    main()
