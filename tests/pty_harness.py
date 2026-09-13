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


def run(argv, keys, rows=24, cols=80, settle=0.4, env=None, status=False):
    """Start argv under a pty, send `keys`, return everything it printed.

    A float in `keys` is treated as a pause in seconds rather than input.
    With status=True, return (output, wait status) instead. A program still
    running at the end is killed, so its status reports SIGKILL.
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
        try:
            os.write(fd, key if isinstance(key, bytes) else key.encode())
        except OSError:
            break  # the program has already exited
        time.sleep(0.06)
        drain(0.02)

    deadline = time.time() + settle
    while time.time() < deadline:
        drain(0.1)

    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    _, wstatus = os.waitpid(pid, 0)
    try:
        os.close(fd)
    except OSError:
        pass
    return (out, wstatus) if status else out


def run_until_signal(argv, sig, rows=24, cols=80, startup=0.5, keys=()):
    """Start argv on a pty, let it paint, optionally type `keys`, then `sig`.

    Returns (modes before start, modes while running, modes after exit, wait
    status), each set of modes as termios.tcgetattr reports them. The slave
    end stays open here so the terminal can still be inspected once the
    program has gone -- its modes then are what the user is left with.
    """
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    before = termios.tcgetattr(slave)
    pid = os.fork()
    if pid == 0:
        try:
            os.close(master)
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            for fd in (0, 1, 2):
                os.dup2(slave, fd)
            if slave > 2:
                os.close(slave)
            os.execve(argv[0], argv, dict(os.environ, TERM="xterm"))
        finally:
            os._exit(127)

    def drain(timeout):
        while select.select([master], [], [], timeout)[0]:
            try:
                if not os.read(master, 1 << 20):
                    return
            except OSError:
                return

    deadline = time.time() + startup
    while time.time() < deadline:
        drain(0.05)
    for key in keys:
        os.write(master, key if isinstance(key, bytes) else key.encode())
        time.sleep(0.06)
        drain(0.02)
    during = termios.tcgetattr(slave)
    os.kill(pid, sig)

    deadline = time.time() + 3
    while True:
        drain(0.05)
        done, wstatus = os.waitpid(pid, os.WNOHANG)
        if done:
            break
        if time.time() > deadline:
            os.kill(pid, signal.SIGKILL)
            _, wstatus = os.waitpid(pid, 0)
            break
    after = termios.tcgetattr(slave)
    os.close(master)
    os.close(slave)
    return before, during, after, wstatus


def status_lines(out, rows=24):
    """The message-bar texts olly painted, oldest first."""
    import re

    text = out.decode("utf8", "replace")
    pattern = "\x1b\\[%d;1H\x1b\\[K([^\x1b]*)" % rows
    return [m for m in re.findall(pattern, text) if m.strip()]
