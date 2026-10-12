#!/usr/bin/env python3
"""Filesystem-error regression checks for save durability paths."""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import run, status_lines

SAVE = "\x13"


def main():
    olly = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./olly")
    cc = os.environ.get("CC", "cc")
    root = tempfile.mkdtemp(prefix="olly-io-")
    try:
        sync_shim = os.path.join(root, "failfsync.so")
        write_shim = os.path.join(root, "failwrite.so")
        subprocess.run([cc, "-shared", "-fPIC", "-O2",
                        os.path.join(os.path.dirname(__file__), "failfsync.c"),
                        "-ldl", "-o", sync_shim], check=True)
        subprocess.run([cc, "-shared", "-fPIC", "-O2",
                        os.path.join(os.path.dirname(__file__), "failwrite.c"),
                        "-ldl", "-o", write_shim], check=True)
        path = os.path.join(root, "target.txt")
        with open(path, "w") as fh:
            fh.write("original\n")

        out, status = run(
            [olly, path], ["X", SAVE],
            env={"LD_PRELOAD": sync_shim, "OLLY_FAIL_FSYNC": "1"},
            status=True)
        with open(path) as fh:
            body = fh.read()
        messages = status_lines(out)
        assert body == "original\n", body
        assert any("Save failed: fsync" in msg for msg in messages), messages
        assert os.WIFEXITED(status) or os.WIFSIGNALED(status)

        with open(path, "w") as fh:
            fh.write("original\n")
        out, status = run(
            [olly, path], ["X", SAVE],
            env={"LD_PRELOAD": write_shim, "OLLY_FAIL_WRITE": "1"},
            status=True)
        with open(path) as fh:
            body = fh.read()
        messages = status_lines(out)
        assert body == "original\n", body
        assert any("Save failed" in msg for msg in messages), messages
        assert os.WIFEXITED(status) or os.WIFSIGNALED(status)

        with open(path, "w") as fh:
            fh.write("original\n")
        out, status = run(
            [olly, path], ["X", SAVE],
            env={"LD_PRELOAD": write_shim, "OLLY_FAIL_WRITE": "1",
                 "OLLY_FAIL_WRITE_ERR": "EINTR"},
            status=True)
        with open(path) as fh:
            body = fh.read()
        assert body == "Xoriginal\n", body
        assert os.WIFEXITED(status) or os.WIFSIGNALED(status)
        print("fsync, disk-full, and interrupted-write failures handled")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
