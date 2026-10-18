#!/usr/bin/env python3
"""Inspect an owned, silent Kitty window; leave user sessions untouched."""
import json
import os
from pathlib import Path
import subprocess
import signal
import tempfile
import time


def run(*args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def main():
    checkout = Path(__file__).resolve().parents[1]
    binary = Path(os.environ.get('TOMPOUCE_TEST_BINARY', str(checkout / 'build/tompouce')))
    output = Path('/tmp/tompouce-notcurses-qa')
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tompouce-kitty-qa-') as root:
        sock = 'unix:' + root + '/kitty.sock'
        code = ('// Notcurses: café ♫ 界\n'
                'note("c4 e4 g4 b4").s("sine").gain(0)._pianoroll()\n'
                'note("c3").s("sawtooth").gain(0)._scope()')
        env = dict(os.environ, XDG_CONFIG_HOME=root, XDG_CACHE_HOME=root,
                   TOMPOUCE_BANKS=root)
        child = subprocess.Popen(['kitty', '--config', 'NONE', '--class', 'tompouce-notcurses-qa',
            '--title', 'Tompouce Notcurses QA', '--listen-on', sock,
            '-o', 'allow_remote_control=socket-only', '-o', 'font_size=12',
            str(binary), '--code', code], env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            window = None
            while time.monotonic() < deadline:
                clients = json.loads(run('hyprctl', 'clients', '-j').stdout)
                window = next((c for c in clients if c.get('class') == 'tompouce-notcurses-qa'), None)
                if window and Path(root + '/kitty.sock').exists(): break
                time.sleep(.1)
            assert window, 'owned Kitty window did not appear'
            time.sleep(1)

            def capture(name):
                clients = json.loads(run('hyprctl', 'clients', '-j').stdout)
                own = next(c for c in clients if c['address'] == window['address'])
                x, y = own['at']; w, h = own['size']
                terminals = json.loads(run('kitty', '@', '--to', sock, 'ls').stdout)
                engine = terminals[0]['tabs'][0]['windows'][0]['foreground_processes'][0]
                assert engine['cmdline'][0] == str(binary)
                os.kill(engine['pid'], signal.SIGSTOP)
                try:
                    time.sleep(.6)  # Let the terminal's GPU and synchronized frame settle.
                    run('grim', '-g', f'{x},{y} {w}x{h}', str(output / name))
                finally:
                    os.kill(engine['pid'], signal.SIGCONT)

            capture('editor.png')
            initial_text = run('kitty', '@', '--to', sock, 'get-text').stdout.decode()
            assert 'Notcurses: café' in initial_text, initial_text[:1000]
            run('kitty', '@', '--to', sock, 'send-text', '--all', '--bracketed-paste', 'disable', '--stdin', input=b'\x1bOQ/graphics braille\r')
            time.sleep(.3)
            capture('editor-braille.png')
            run('kitty', '@', '--to', sock, 'send-text', '--all', '--bracketed-paste', 'disable', '--stdin', input=b'\x1bOQ/graphics auto\r')
            time.sleep(.3)
            run('kitty', '@', '--to', sock, 'send-text', '--all', '--bracketed-paste', 'disable', '--stdin', input=b'\x1b[17~')
            time.sleep(.3)
            capture('sounds.png')
            sounds_text = run('kitty', '@', '--to', sock, 'get-text').stdout.decode()
            assert 'import-sounds' in sounds_text, sounds_text[:1000]
            run('kitty', '@', '--to', sock, 'send-text', '--all', '--bracketed-paste', 'disable', '--stdin', input=b'\x1b[17~\x11')
            child.wait(timeout=8)
            assert child.returncode == 0
            with open(root + '/tompouce/state.json') as state_file:
                assert json.load(state_file)['code'].rstrip('\n') == code
            print(output)
        finally:
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait()


if __name__ == '__main__':
    main()
