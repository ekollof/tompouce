"""Run with python3 tests/test_tui_pty.py; all persistence stays in a temp dir."""
import fcntl
import json
import os
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import urllib.request
import re
import base64

BINARY = os.environ.get('TOMPOUCE_TEST_BINARY', './tompouce')
CASE_FILTER = os.environ.get('TOMPOUCE_TEST_FILTER', '')


def check(name, code, keys=b"", expected=None, vi=False, stop_signal=None, observed=None, companion=None, import_sounds=False, persisted_vi=None, application="tompouce", graphics=None):
    if CASE_FILTER and CASE_FILTER not in name:
        return
    with tempfile.TemporaryDirectory(prefix="tompouce-pty-") as root:
        os.mkdir(root + "/" + application)
        with open(root + "/" + application + "/config.json", "w") as config:
            json.dump({"vi_mode": vi}, config)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 120, 0, 0))
        original = termios.tcgetattr(slave)
        env = dict(os.environ, TERM="xterm-256color", XDG_CONFIG_HOME=root,
                   XDG_CACHE_HOME=root, TOMPOUCE_BANKS=root)
        env.pop('TMUX', None)
        env.pop('STY', None)
        if import_sounds:
            from test_sound_import import audio
            folder = root + '/My Drums'
            os.mkdir(folder)
            with open(folder + '/kick.wav', 'wb') as file:
                file.write(audio())
            keys = b'\x1b[17~\x1b[D\x1b[D\x1b[200~' + folder.encode() + b'\x1b[201~\r'
        browser_record = root + "/browser.jsonl"
        companion_url = None
        if companion:
            with open(root + "/chromium", "w") as stub:
                stub.write("#!/usr/bin/env python3\nimport os,sys,json\n"
                           "with open(os.environ['TEST_BROWSER_RECORD'],'a') as f:\n"
                           " f.write(json.dumps({'url':sys.argv[1]})+'\\n')\n")
            os.chmod(root + "/chromium", 0o700)
            env["PATH"] = root + os.pathsep + env["PATH"]
            env["TEST_BROWSER_RECORD"] = browser_record
        proc = subprocess.Popen([BINARY, "--code", code], stdin=slave,
                                stdout=slave, stderr=slave, env=env)
        application = "tompouce"  # Legacy roots migrate during startup.
        output = bytearray()

        def drain(duration):
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                if select.select([master], [], [], .02)[0]:
                    output.extend(os.read(master, 65536))

        try:
            deadline = time.monotonic() + 8
            while b"\x1b[?2004h" not in output and time.monotonic() < deadline:
                drain(.05)
            assert b"\x1b[?2004h" in output, (name, "UI did not initialize")
            drain(.15)
            if graphics:
                assert b'i=1414483200,s=1,v=1,a=q,t=d,f=24;AAAA' in output, (name, 'missing capability query')
                response = b'OK' if graphics != 'unsupported' else b'ENOTSUP'
                os.write(master, b'\x1b_Gi=1414483200;' + response + b'\x1b\\')
                drain(.2)
                if name == 'Kitty terminal palette':
                    assert b'\x1b]10;?' in output and b'\x1b]4;14;?' in output
                    start=len(output)
                    os.write(master,b'\x1b]10;rgb:1111/2222/3333\x1b\\\x1b]11;rgb:ee/ee/ee\x07'
                             b'\x1b]4;14;rgb:ab/cd/ef\x1b\\\x1b]4;99;rgb:ff/ff/ff\x07')
                    drain(.15)
                    chunks=re.findall(rb'\x1b_G([^;]*);([A-Za-z0-9+/=]*)\x1b\\',bytes(output[start:]))
                    pixels=b''.join(base64.b64decode(payload) for header,payload in chunks
                                    if header.startswith((b'a=T,',b'q=2,m=')))
                    assert b'\xab\xcd\xef\xff' in pixels, 'scope did not use terminal cyan'
                    assert b'\x11\x22\x33\xff' in pixels, 'axis text did not use foreground'
                if graphics != 'unsupported':
                    assert b'\x1b_Ga=T,t=d,f=32,' in output, (name, 'missing Kitty image')
                    assert b'i=1414483201,p=1' in output, (name, 'missing first placement')
                    assert b'i=1414483202,p=1' in output, (name, 'missing second placement')
                    if name == 'Kitty graphics capability, resize and editor input':
                        start = len(output)
                        started = time.monotonic()
                        drain(.5)
                        elapsed = time.monotonic()-started
                        frames = output[start:].count(b'i=1414483201,p=1')
                        fps = frames/elapsed
                        print(f'Kitty graphics: {fps:.1f} FPS')
                        assert fps >= 30, (name, 'graphics refresh stalled', fps)
                        assert fps < 80, (name, 'graphics refresh is not paced', fps)
                    # Resizing invalidates placements and keeps images within
                    # the new geometry. Editing after replies must remain safe.
                    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 28, 100, 0, 0))
                    proc.send_signal(signal.SIGWINCH)
                    drain(.15)
                    if graphics == 'scroll':
                        start = len(output)
                        os.write(master,b'\x1b[<65;10;5M'*12)
                        drain(.3)
                        assert b'\x1b_Ga=d,d=I,' in output[start:], (name, 'scroll left images behind')
            if vi:
                assert b"VI:NORMAL" in output, (name, "vi startup config ignored")
            if keys:
                os.write(master, keys)
            drain(.3)
            if graphics == 'braille' or name == 'Kitty hide inline cleanup':
                assert b'\x1b_Ga=d,d=I,i=1414483201,q=2' in output, (name, 'hidden images survived until exit')
            if import_sounds:
                deadline = time.monotonic() + 5
                while not os.path.exists(root + '/' + application + '/sounds/index.json') and time.monotonic() < deadline:
                    drain(.05)
                assert os.path.exists(root + '/' + application + '/sounds/index.json'), (name, 'folder import did not finish', bytes(output[-2500:]))
                drain(.2)  # Let the foreground helper poll reload the catalog.
                os.write(master, b'my_drums\r')
                drain(.2)
            if companion:
                deadline = time.monotonic() + 8
                while not os.path.exists(browser_record) and time.monotonic() < deadline:
                    drain(.05)
                with open(browser_record) as record:
                    companion_url = json.loads(record.readline())["url"]
                html = urllib.request.urlopen(companion_url, timeout=2).read().decode()
                token = re.search(r'name="companion-token" content="([^"]+)"', html).group(1)
                origin = companion_url.rstrip("/")
                request = urllib.request.Request(origin + "/api/tool",
                    json.dumps({"name": "music_get_code"}).encode(), headers={
                        "Origin": origin, "X-Companion-Token": token,
                        "Content-Type": "application/json"})
                with urllib.request.urlopen(request, timeout=3) as response:
                    assert json.load(response)["result"]["content"][0]["text"].rstrip("\n") == code
                if companion == "quit":
                    # Catch the former 50 ms sleep on both request and close:
                    # a burst must not pay two terminal ticks per snapshot.
                    started = time.monotonic()
                    for _ in range(20):
                        request = urllib.request.Request(origin + "/api/tool",
                            json.dumps({"name": "music_debug", "arguments": {"limit": 32}}).encode(),
                            headers={"Origin": origin, "X-Companion-Token": token,
                                     "Content-Type": "application/json"})
                        with urllib.request.urlopen(request, timeout=3) as response:
                            assert "pool=" in json.load(response)["result"]["content"][0]["text"]
                    elapsed = time.monotonic() - started
                    assert elapsed < 1, ("voice polling stalled behind UI ticks", elapsed)
                    print(f"20 companion voice snapshots: {elapsed:.3f}s")
                os.write(master, b"\x1bOQ/companion\r")
                drain(.3)
                with open(browser_record) as record:
                    assert len(record.readlines()) == 1, "duplicate companion/browser launch"
                if companion == "stop":
                    os.write(master, b"\x1bOQ/companion stop\r")
                    drain(.5)
                    try:
                        urllib.request.urlopen(companion_url, timeout=.2).close()
                    except OSError:
                        pass
                    else:
                        raise AssertionError("/companion stop left its helper running")
            if observed is not None:
                assert observed in output, (name, "expected display value missing")
            if stop_signal:
                proc.send_signal(stop_signal)
            else:
                os.write(master, b"\x11")
            deadline = time.monotonic() + 8
            while proc.poll() is None and time.monotonic() < deadline:
                drain(.05)
            assert proc.poll() == 0, (name, "exit failed", proc.poll())
            if companion_url:
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    try:
                        urllib.request.urlopen(companion_url, timeout=.2).close()
                    except OSError:
                        break
                    time.sleep(.05)
                else:
                    raise AssertionError("companion survived UI shutdown")
            drain(.05)
            if graphics == 'unsupported':
                assert b'\x1b_Ga=T' not in output, (name, 'unsupported terminal received images')
            elif graphics:
                assert b'\x1b_Ga=d,d=I,i=1414483201,q=2' in output, (name, 'owned images were not removed')
                if graphics == 'braille':
                    with open(root + '/tompouce/config.json') as file:
                        assert json.load(file)['kitty_graphics'] is False
            assert termios.tcgetattr(slave) == original, (name, "termios changed")
            assert b"\x1b[?1002l\x1b[?1006l\x1b[?2004l" in output, name
            if expected is not None:
                with open(root + "/" + application + "/state.json") as state_file:
                    state = json.load(state_file)
                assert state["code"].rstrip("\n") == expected, (name, state["code"])
            if name == 'disable editor scope':
                with open(root + '/' + application + '/config.json') as file:
                    assert json.load(file)['editor_scope'] is False
            if name.startswith('inline braille'):
                assert re.search(rb'\xe2[\xa0-\xa3][\x80-\xbf]', output), (name, 'braille pixels missing')
            if name == 'disable inline visuals':
                with open(root + '/' + application + '/config.json') as file:
                    assert json.load(file)['inline_visuals'] is False
            if persisted_vi is not None:
                with open(root + '/' + application + '/config.json') as file:
                    assert json.load(file)['vi_mode'] is persisted_vi
                output.clear()
                proc = subprocess.Popen([BINARY, '--code', code], stdin=slave,
                    stdout=slave, stderr=slave, env=env)
                deadline = time.monotonic() + 8
                while b'\x1b[?2004h' not in output and time.monotonic() < deadline:
                    drain(.05)
                drain(.15)
                assert b'\x1b[?2004h' in output, (name, 'restart failed')
                assert (b'VI:NORMAL' in output) is persisted_vi, (name, 'startup mode not restored')
                os.write(master,b'\x11')
                deadline = time.monotonic() + 8
                while proc.poll() is None and time.monotonic() < deadline:
                    drain(.05)
                assert proc.poll() == 0
                assert termios.tcgetattr(slave) == original
            print(name, "ok")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)


check("multiline paste and clipboard", "",
      b"\x1b[200~one\ntwo\nthree\x1b[201~\x01\x03\x01\x18\x16", "one\ntwo\nthree")
check("vi visual lines and startup config", "one\ntwo\nthree", b"Vjyp", "one\ntwo\none\ntwo\nthree", vi=True)
check("persist vi mode on restart", "", b'\x1bOR', expected='', persisted_vi=True)
check("persist insert mode on restart", "", b'\x1bOR', expected='', vi=True, persisted_vi=False)
check("vi mode clears regular selection", 'note("c4").gain(0)', b'\x01\x1bOR',
      expected='note("c4").gain(0)', observed=b'VI:NORMAL')
check("mouse cursor", "one\ntwo", b"\x1b[<0;8;4M\x1b[<0;8;4mX", "one\ntXwo")
check("mouse selection", "one\ntwo\nthree",
      b"\x1b[<0;7;3M\x1b[<32;10;4M\x1b[<0;10;4m\x18", "\nthree")
check("pane focus", "one", b"\x1b[<0;90;4MX\x1b[<0;7;3MY", "Yone")
check("mouse slider", 's("hh").gain(slider(.5))',
      b"\x1b[<0;65;3M\x1b[<0;65;3m", 's("hh").gain(slider(.5))', observed=b"slider 1 = 1")
check("invalid mouse report", "one", b"\x1b[<999999999999999999999999;7;3M", "one")
check("shift selection", "one\ntwo", b"\x1b[1;2B\x1b[1;2F\x18", "")
check("SIGTERM cleanup", "", stop_signal=signal.SIGTERM)
check("SIGINT cleanup", "", stop_signal=signal.SIGINT)
check("disable editor scope", "", b'\x1bOQ/scope off\r', expected='', observed=b'editor scope off')
check("F4 state diagnostics", "note('c4').gain(0)", b'\x1bOS',
      expected="note('c4').gain(0)", vi=True, observed=b'mode=VI:N')
check("Tab indentation", "", b'\tX', expected='  X')
check("Shift Tab unindent", "  note('c4')", b'\x1b[Z', expected="note('c4')")
check("Tab selected lines", "one\ntwo", b'\x01\t', expected='  one\n  two')
check("Tab undo", "", b'\t\x1a', expected='')
check("Ctrl Space completion", "", b'bp\x00', expected='bpm')
check("Selected block unindent", "one\ntwo", b'\x01\t\x1b[Z', expected='one\ntwo')
check("categorized sound search and insertion", "", b'\x1b[17~supersaw\r',
      expected='note("c4").s("supersaw")')
check("sound category navigation", "", b'\x1b[17~\x1b[C\x1b[C\x1b[Csine\r',
      expected='note("c4").s("sine")')
check("sound folder import and insertion", "", import_sounds=True, expected='s("my_drums")')
check("inline braille scope", "const melody=note('c4').gain(0); melody._scope()",
      expected="const melody=note('c4').gain(0); melody._scope()", observed=b"scope | pattern output")
check("inline braille piano roll", 'note("c4 e4 g4").gain(0)._pianoroll()',
      expected='note("c4 e4 g4").gain(0)._pianoroll()', observed=b'piano roll | pattern notes')
for visual in ('punchcard', 'spiral', 'pitchwheel', 'spectrum', 'wordfall'):
    visual_code = 'note("60 64 67").gain(0)._'+visual+'()'
    check('inline braille '+visual, visual_code, expected=visual_code,
          observed=(visual+' | pattern').encode())
    kitty_visual_code=visual_code+'\nnote("60").gain(0)._scope()'
    check('Kitty '+visual, kitty_visual_code, expected=kitty_visual_code, graphics='supported')
check("inline widget keyboard movement", 'note("c4").gain(0)._scope()\n// next', b'\x1b[BX',
      expected='note("c4").gain(0)._scope()\nX// next')
check("inline widget mouse ignores display rows", 'note("c4").gain(0)._scope()\n// next',
      b'\x1b[<0;7;8M\x1b[<0;7;8mX', expected='note("c4").gain(0)._scope()\nX// next')
check("inline widget mouse click is inert", 'note("c4").gain(0)._scope()\n// next',
      b'\x1b[<0;7;5M\x1b[<0;7;5mX', expected='Xnote("c4").gain(0)._scope()\n// next')
check("inline widget horizontal scroll layout", 'note("c4").gain(0)._scope()\n// next',
      b'\x1bOQ/wrap off\r\x1b[<0;7;8M\x1b[<0;7;8mX', expected='note("c4").gain(0)._scope()\nX// next')
check("disable inline visuals", 'note("c4").gain(0)._scope()', b'\x1bOQ/inline off\r',
      expected='note("c4").gain(0)._scope()', observed=b'inline visuals off')
check("companion launch and quit cleanup", "note('c4').gain(0)", b"\x1bOQ/companion\r", companion="quit")
check("companion stop", "note('c4').gain(0)", b"\x1bOQ/companion\r", companion="stop")
check("legacy vi configuration", "one", b"iX", expected="Xone", vi=True, application="strudelterm")
check("legacy sound library import", "", import_sounds=True, expected='s("my_drums")', application="strudelterm")
graphics_code = 'note("c4").gain(0)._scope()\nnote("g4").gain(0)._pianoroll()'
check('Kitty terminal palette', graphics_code, expected=graphics_code, graphics='supported')
check('Kitty graphics capability, resize and editor input', graphics_code, b'X', expected='X'+graphics_code, graphics='supported')
check('Kitty unsupported braille fallback', graphics_code, expected=graphics_code, graphics='unsupported')
check('Kitty manual braille fallback', graphics_code, b'\x1bOQ/graphics braille\r', expected=graphics_code, graphics='braille')
check('Kitty hide inline cleanup', graphics_code, b'\x1bOQ/inline off\r', expected=graphics_code, graphics='supported')
scroll_graphics_code = graphics_code+'\n'+'\n'.join('// scroll '+str(i) for i in range(40))
check('Kitty scrolling removes images', scroll_graphics_code, expected=scroll_graphics_code, graphics='scroll')
