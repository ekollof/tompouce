"""The backing-cell harness must not depend on the caller's terminal size."""
import fcntl
import os
from pathlib import Path
import pty
import resource
import select
import struct
import subprocess
import tempfile
import termios
import time
import unittest

BINARY = Path(__file__).resolve().with_name('test_terminal_render')


def no_core():
    # A failed regression still reports its assertion through the PTY.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class TerminalRenderTest(unittest.TestCase):
    def test_independent_of_terminal_geometry(self):
        for rows, cols in [(24, 80), (52, 174), (16, 60), (72, 240)]:
            with self.subTest(rows=rows, cols=cols), tempfile.TemporaryDirectory(
                    prefix='tompouce-render-pty-') as root:
                master, slave = pty.openpty()
                output = bytearray()
                child = None
                try:
                    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                                struct.pack('HHHH', rows, cols, 0, 0))
                    env = dict(os.environ, TERM='xterm-256color', XDG_CONFIG_HOME=root,
                               XDG_CACHE_HOME=root, TOMPOUCE_BANKS=root)
                    child = subprocess.Popen([str(BINARY)], stdin=slave, stdout=slave,
                                             stderr=slave, env=env, preexec_fn=no_core)
                    os.close(slave)
                    slave = None
                    deadline = time.monotonic() + 20
                    while child.poll() is None and time.monotonic() < deadline:
                        if select.select([master], [], [], .1)[0]:
                            try:
                                output.extend(os.read(master, 65536))
                            except OSError:
                                break  # PTY slave closed on exit.
                    diagnostic = output.decode(errors='replace')[-3000:]
                    try:
                        child.wait(timeout=max(.01, deadline-time.monotonic()))
                    except subprocess.TimeoutExpired:
                        self.fail('render harness timed out: ' + diagnostic)
                    self.assertEqual(child.returncode, 0, diagnostic)
                finally:
                    if child and child.poll() is None:
                        child.kill()
                        child.wait()
                    if slave is not None:
                        os.close(slave)
                    os.close(master)


if __name__ == '__main__':
    unittest.main()
