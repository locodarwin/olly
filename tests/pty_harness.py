"""Drive olly inside a real pty and capture what it writes.

Olly only runs on a terminal (it calls tcgetattr on stdin at startup), so the
tests give it one rather than a pipe.
"""

import fcntl
import os
import pty
import select
import signal
import struct
import termios
import time


def run(argv, keys, rows=24, cols=80, settle=0.4, env=None):
    """Start argv under a pty, send `keys`, return everything it printed.

    A float in `keys` is treated as a pause in seconds rather than input.
    """
    pid, fd = pty.fork()
    if pid == 0:
        environ = dict(os.environ, TERM="xterm")
        if env:
            environ.update(env)
        os.execve(argv[0], argv, environ)

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = b""
    time.sleep(0.3)  # let the first repaint land

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

    for key in keys:
        if isinstance(key, float):
            time.sleep(key)
            continue
        os.write(fd, key if isinstance(key, bytes) else key.encode())
        time.sleep(0.06)
        drain(0.02)

    deadline = time.time() + settle
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


def status_lines(out, rows=24):
    """The message-bar texts olly painted, oldest first."""
    import re

    text = out.decode("utf8", "replace")
    pattern = "\x1b\\[%d;1H\x1b\\[K([^\x1b]*)" % rows
    return [m for m in re.findall(pattern, text) if m.strip()]
