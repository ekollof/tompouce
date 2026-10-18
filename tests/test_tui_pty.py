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
from pathlib import Path
from terminal_screen import screen_text

PROJECT_ROOT = Path(__file__).resolve().parents[1]
BINARY = os.environ.get('TOMPOUCE_TEST_BINARY', str(PROJECT_ROOT / 'build/tompouce'))
CASE_FILTER = os.environ.get('TOMPOUCE_TEST_FILTER', '')

# Direct invocation must test the current source, including private UI headers.
# Explicit binary overrides intentionally test an already installed executable.
if 'TOMPOUCE_TEST_BINARY' not in os.environ:
    subprocess.run(['cmake', '--preset', 'default'], cwd=PROJECT_ROOT, check=True)
    subprocess.run(['cmake', '--build', '--preset', 'default', '--target', 'tompouce'], cwd=PROJECT_ROOT, check=True)

GM_SUPPORTED = None


def gm_supported():
    global GM_SUPPORTED
    if GM_SUPPORTED is None:
        with tempfile.TemporaryDirectory(prefix='tompouce-gm-catalog-') as root:
            env = dict(os.environ, XDG_CONFIG_HOME=root, XDG_CACHE_HOME=root,
                       XDG_STATE_HOME=root, TOMPOUCE_BANKS=root)
            request = {'jsonrpc':'2.0','id':1,'method':'tools/call','params':{
                'name':'music_sounds','arguments':{'category':'synths','query':'gm_','limit':256}}}
            binary = str(Path(BINARY).resolve()) if '/' in BINARY else BINARY
            result = subprocess.run([binary,'--mcp'],input=json.dumps(request)+'\n',
                                    capture_output=True,text=True,env=env,cwd=root,timeout=15,check=True)
            replies = [json.loads(line) for line in result.stdout.splitlines()]
            reply = next(reply for reply in replies if reply.get('id') == 1)
            catalog = json.loads(reply['result']['content'][0]['text'])
            GM_SUPPORTED = catalog['total'] > 0
            if GM_SUPPORTED:
                assert catalog['total'] == 125
                assert any(item['name']=='gm_flute' and item['code']=='note("c4").s("gm_flute")'
                           for item in catalog['items'])
            elif os.environ.get('TOMPOUCE_TEST_REQUIRE_GM'):
                raise AssertionError('GM support required but unavailable in the tested executable')
    return GM_SUPPORTED


def check(name, code, keys=b"", expected=None, vi=False, stop_signal=None, observed=None, companion=None, import_sounds=False, persisted_vi=None, application="tompouce", graphics=None, load_song=False, theme=False, manifest=False, mouse_pixels=False, mouse=True, command_files=False, saved_file=None, tutor_existing=None, tutor_saved=False, live_slider=False, transport=None, source_state=None):
    if CASE_FILTER and CASE_FILTER not in name:
        return
    if name.startswith('GM synth') and not gm_supported():
        print(name+' skipped (optional GM backend/font unavailable)')
        return
    with tempfile.TemporaryDirectory(prefix="tompouce-pty-") as root:
        os.mkdir(root + "/" + application)
        with open(root + "/" + application + "/config.json", "w") as config:
            json.dump({"vi_mode": vi, "mouse": mouse}, config)
        if tutor_existing is not None:
            tutorial = json.loads((PROJECT_ROOT/'strudeltutor.json').read_text())
            tutorial['code'] = tutorial['tracks'][0] = tutor_existing
            with open(root+'/'+application+'/strudeltutor.json','w') as file:
                json.dump(tutorial,file)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 120,
                                                       960 if mouse_pixels else 0, 512 if mouse_pixels else 0))
        original = termios.tcgetattr(slave)
        env = dict(os.environ, TERM="xterm-256color", XDG_CONFIG_HOME=root,
                   XDG_CACHE_HOME=root, XDG_STATE_HOME=root, TOMPOUCE_BANKS=root)
        env.pop('TMUX', None)
        env.pop('STY', None)
        if theme:
            env['COLORTERM']='truecolor'
        if manifest:
            from test_sound_import import audio
            os.mkdir(root+'/kit')
            for filename in ('z.wav','a.wav','snare.wav'):
                with open(root+'/kit/'+filename,'wb') as file:
                    file.write(audio())
            with open(root+'/stray.wav','wb') as file:
                file.write(audio())
            with open(root+'/My Drum.wav','wb') as file:
                file.write(audio())
            with open(root+'/kit/strudel.json','w') as file:
                catalog = {'Fixture909_bd':['z.wav','a.wav'],'Fixture909_sd':['snare.wav'],
                           'fixturebell':['z.wav','a.wav'],'wt_test':{'D2':['z.wav','a.wav']}}
                if 'scrolled row' in name:
                    catalog.update({f'scrollfixture{i:02}':['z.wav'] for i in range(40)})
                json.dump(catalog,file)
        if load_song:
            path=root+'/loaded-song.json'
            with open(path,'w') as song:
                json.dump({'version':2,'bpm':120,'code':expected,'tracks':[expected],'active':0},song)
            keys=b'\x1bOQ/load '+path.encode()+b'\r'
        if import_sounds:
            from test_sound_import import audio
            folder = root + '/My Drums'
            os.mkdir(folder)
            with open(folder + '/kick.wav', 'wb') as file:
                file.write(audio())
            keys = b'\x1b[17~\x1b[D\x1b[D\x1b[200~' + folder.encode() + b'\x1b[201~\r'
        if command_files:
            os.mkdir(root+'/songs')
            for path in ('Existing.json','songs/My song.json'):
                with open(root+'/'+path,'w') as file:
                    json.dump({'version':2,'bpm':120,'code':'note(72).gain(0)'},file)
        launch_binary = str(Path(BINARY).resolve()) if '/' in BINARY else BINARY
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
        proc = subprocess.Popen([launch_binary, "--code", code], stdin=slave,
                                stdout=slave, stderr=slave, env=env, cwd=root if command_files else None)
        application = "tompouce"  # Legacy roots migrate during startup.
        output = bytearray()
        negotiated = False

        def mouse_modes():
            modes = {}
            for match in re.finditer(rb'\x1b\[\?([0-9;]+)([hl])', output):
                for mode in match[1].split(b';'):
                    modes[int(mode)] = match[2] == b'h'
            return modes

        def terminal_keys(data):
            # A real terminal follows the protocol the app enabled. Advertising
            # pixel support alone must not silently change cell reports to pixels.
            if mouse_pixels and mouse_modes().get(1016):
                def pixels(match):
                    x = (int(match[2])-1)*8+4
                    y = (int(match[3])-1)*16+8
                    return b'\x1b[<'+match[1]+b';'+str(x).encode()+b';'+str(y).encode()+match[4]
                data = re.sub(rb'\x1b\[<([0-9]+);([0-9]+);([0-9]+)([Mm])', pixels, data)
            os.write(master, data)

        def drain(duration):
            nonlocal negotiated
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                if select.select([master], [], [], .02)[0]:
                    output.extend(os.read(master, 65536))
                    if not negotiated and b'\x1b[c' in output:
                        palette = (b'\x1b]10;rgb:1111/2222/3333\x1b\\\x1b]11;rgb:ee/ee/ee\x1b\\'
                                   b'\x1b]4;14;rgb:ab/cd/ef\x1b\\') if name == 'Kitty terminal palette' else b''
                        pixels = (b'\x1bP>|kitty(0.40.0)\x1b\\\x1b_Gi=1;OK\x1b\\'
                                  b'\x1b[4;512;960t\x1b[8;32;120t') if graphics and graphics != 'unsupported' else b''
                        if mouse_pixels:
                            pixels += b'\x1b[?1016;2$y\x1b[4;512;960t\x1b[8;32;120t'
                        os.write(master,palette+pixels+b'\x1b[1;1R\x1b[?1;2c')
                        negotiated = True

        try:
            deadline = time.monotonic() + 8
            while b"\x1b[?2004h" not in output and time.monotonic() < deadline:
                drain(.05)
            assert b"\x1b[?2004h" in output, (name, "UI did not initialize")
            drain(.15)
            if theme:
                os.mkdir(root+'/wal')
                def write_palette(fg):
                    with open(root+'/wal/new.json','w') as palette:
                        json.dump({'special':{'foreground':fg,'background':'#010203'},
                                   'colors':{f'color{i}':f'#{0x200000+i:06x}' for i in range(16)}},palette)
                    os.replace(root+'/wal/new.json',root+'/wal/colors.json')
                write_palette('#123456')
                os.write(master,b'\x1bOQ/theme pywal\r')
                drain(.3)
                assert b'38;2;18;52;86' in output, ('theme command did not recolor text',bytes(output[-3000:]))
                start=len(output)
                write_palette('#abcdef')
                drain(1.3)
                assert b'38;2;171;205;239' in output[start:], 'file replacement did not recolor live text'
                if graphics:
                    chunks=re.findall(rb'\x1b_G([^;]*);([A-Za-z0-9+/=]*)\x1b\\',bytes(output[start:]))
                    pixels=b''.join(base64.b64decode(payload) for header,payload in chunks
                                    if header.startswith((b'a=T,',b'q=2,m=')))
                    assert b'\xab\xcd\xef\xff' in pixels, 'file change did not recolor graphics labels'
                with open(root+'/tompouce/config.json') as config:
                    assert json.load(config)['theme_source']=='pywal'
            if graphics:
                assert b'\x1b_Gi=1,a=q;' in output, (name, 'missing Notcurses capability query')
                response = b'OK' if graphics != 'unsupported' else b'ENOTSUP'
                os.write(master, b'\x1b_Gi=1414483200;' + response + b'\x1b\\')
                drain(.2)
                if name == 'Kitty terminal palette':
                    assert b'\x1b]10;?' in output and b'\x1b]4;14;?' in output
                    start=0
                    os.write(master,b'\x1b]10;rgb:1111/2222/3333\x1b\\\x1b]11;rgb:ee/ee/ee\x07'
                             b'\x1b]4;14;rgb:ab/cd/ef\x1b\\\x1b]4;99;rgb:ff/ff/ff\x07')
                    drain(.15)
                    chunks=re.findall(rb'\x1b_G([^;]*);([A-Za-z0-9+/=]*)\x1b\\',bytes(output[start:]))
                    pixels=b''.join(base64.b64decode(payload) for header,payload in chunks
                                    if header.startswith((b'a=T,',b'q=2,m=')))
                    assert b'\xab\xcd\xef\xff' in pixels, 'scope did not use Notcurses terminal cyan'
                    assert b'\x11\x22\x33\xff' in pixels, 'axis text did not use foreground'
                if graphics != 'unsupported':
                    if graphics != 'live':
                        assert b'\x1b_Ga=T,t=d,f=32,' in output, (name, 'missing Kitty image')
                        assert b'i=1414483201,p=1' in output, (name, 'missing first placement')
                        assert b'i=1414483202,p=1' in output, (name, 'missing second placement')
                    if name in ('Kitty graphics capability, resize and editor input', 'Kitty spectrum'):
                        start = len(output)
                        started = time.monotonic()
                        drain(.5)
                        elapsed = time.monotonic()-started
                        frames = output[start:].count(b'i=1414483201,p=1')
                        fps = frames/elapsed
                        print(f'{name}: {fps:.1f} FPS')
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
                assert b"VI:NORMAL" in re.sub(rb'\x1b\[[0-9;]*m', b'', output), (name, "vi startup config ignored")
            if live_slider:
                os.write(master,b'\x1bOS'); drain(.15)
                match=re.search(rb'gen=(\d+)',screen_text(output)); assert match, screen_text(output)
                slider_generation=match[1]
                os.write(master,b'\x1bOS'); drain(.1)
            if keys:
                terminal_keys(keys)
            drain(.3)
            if transport is not None:
                text=screen_text(output)
                assert (b' PLAY ' if transport else b' STOP ') in text, (name,'transport state',text)
            if source_state is not None:
                text=screen_text(output)
                assert b'source='+source_state in text, (name,'source state',text)
            if live_slider:
                if observed is not None:
                    assert observed in screen_text(output), (name,'live value did not change',screen_text(output))
                    observed=None
                os.write(master,b'\x1bOS'); drain(.15)
                text=screen_text(output)
                match=re.search(rb'gen=(\d+)',text)
                assert match and match[1]==slider_generation, (name,'slider recompiled/restarted the pattern',text)
                if 'dirty' in name:
                    assert b'source=pending F5' in text, (name,'slider applied unrelated edits',text)
                os.write(master,b'\x1bOS'); drain(.1)
            if graphics == 'live':
                # Generator-side attachment and the first graphics frame are
                # asynchronous; wait for the frame instead of a fixed sleep.
                deadline=time.monotonic()+2
                while b'\x1b_Ga=T,t=d,f=32,' not in output and time.monotonic()<deadline:
                    drain(.05)
                assert b'\x1b_Ga=T,t=d,f=32,' in output, (name, 'live view did not send Kitty pixels',screen_text(output)[-6000:])
                assert b'i=1414483201,p=1' in output, (name, 'live view did not place Kitty image')
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
                deadline = time.monotonic() + 3
                while observed not in re.sub(rb'\x1b\[[0-9;]*m', b'', output) and observed not in screen_text(output) and proc.poll() is None and time.monotonic() < deadline:
                    drain(.05)
                assert observed in re.sub(rb'\x1b\[[0-9;]*m', b'', output) or observed in screen_text(output), (name, "expected display value missing", screen_text(output))
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
                assert b'\x1b_Ga=d,d=I,i=1414483201,q=2' in output, (name, 'owned images were not removed', re.findall(rb'\x1b_Ga=d[^\x1b]*', output), bytes(output[-1500:]))
                if graphics == 'braille':
                    with open(root + '/tompouce/config.json') as file:
                        assert json.load(file)['kitty_graphics'] is False
            assert termios.tcgetattr(slave) == original, (name, "termios changed")
            assert b"\x1b[?2004l" in output, name
            assert not any(mouse_modes().get(mode) for mode in (1000,1002,1003,1006,1016)), (name, 'mouse tracking survived exit')
            if expected is not None:
                with open(root + "/" + application + "/state.json") as state_file:
                    state = json.load(state_file)
                assert state["code"].rstrip("\n") == expected, (name, state["code"])
            if saved_file:
                with open(root+'/'+saved_file) as file:
                    assert json.load(file)['code'].rstrip('\n') == expected
            if tutor_saved:
                with open(root+'/'+application+'/strudeltutor.json') as file:
                    working = json.load(file)
                assert working['code'].rstrip('\n') == expected, (name, 'config working copy differs')
                assert len(working['tracks']) == 22, (name, 'lesson buffers missing')
                assert not Path(root+'/strudeltutor.json').exists(), (name, 'saved into CWD instead of config')
            if command_files and saved_file!='Existing.json':
                with open(root+'/Existing.json') as file:
                    assert json.load(file)['code']=='note(72).gain(0)'
            if name == 'disable editor scope':
                with open(root + '/' + application + '/config.json') as file:
                    assert json.load(file)['editor_scope'] is False
            if mouse_pixels and mouse:
                assert re.search(rb'\x1b\[\?1002;1016h', output), (name, 'pixel mouse mode not enabled')
                assert not mouse_modes().get(1016), (name, 'pixel mouse mode survived exit')
            if name.startswith('inline braille'):
                assert re.search(rb'\xe2[\xa0-\xa3][\x80-\xbf]', output), (name, 'braille pixels missing')
            if name == 'disable inline visuals':
                with open(root + '/' + application + '/config.json') as file:
                    assert json.load(file)['inline_visuals'] is False
            if persisted_vi is not None:
                with open(root + '/' + application + '/config.json') as file:
                    assert json.load(file)['vi_mode'] is persisted_vi
                output.clear()
                negotiated = False
                proc = subprocess.Popen([BINARY, '--code', code], stdin=slave,
                    stdout=slave, stderr=slave, env=env)
                deadline = time.monotonic() + 8
                while b'\x1b[?2004h' not in output and time.monotonic() < deadline:
                    drain(.05)
                drain(.15)
                assert b'\x1b[?2004h' in output, (name, 'restart failed')
                assert (b'VI:NORMAL' in re.sub(rb'\x1b\[[0-9;]*m', b'', output)) is persisted_vi, (name, 'startup mode not restored')
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
check("vi counted operators", "one two three four five six seven", b"2d3w", "seven", vi=True)
check("vi change quoted text", 'note("c4 e4").gain(0)', b'6lci"g4\x1b', 'note("g4").gain(0)', vi=True)
check("vi change undo transaction", "one two", b"cwX\x1bu", "one two", vi=True)
check("vi text object delete", "one two three", b"wdiw", "one  three", vi=True)
check("vi character find delete", "abc, def, ghi", b"df,", " def, ghi", vi=True)
check("vi buffer search", "one two three", b"/three\rdiw", "one two ", vi=True)
check("vi unicode cursor boundaries", "é λ x", b"2lx", "é  x", vi=True)
check("vi unicode insert backspace", "éx", b"A\x7f\x7f", "", vi=True)
check("vi bracket match", "foo(a,(b))", b"%x", "foo(a,(b)", vi=True)
check("vi replace characters", "one two", b"3rX", "XXX two", vi=True)
check("vi counted yank paste", "one two", b"yiw$2p", "one twooneone", vi=True)
check("persist vi mode on restart", "", b'\x1bOR', expected='', persisted_vi=True)
check("persist insert mode on restart", "", b'\x1bOR', expected='', vi=True, persisted_vi=False)
check("vi mode clears regular selection", 'note("c4").gain(0)', b'\x01\x1bOR',
      expected='note("c4").gain(0)', observed=b'VI:NORMAL')
check("mouse cursor", "one\ntwo", b"\x1b[<0;8;4M\x1b[<0;8;4mX", "one\ntXwo")
check("pixel mouse cursor", "one\ntwo", b"\x1b[<0;8;4M\x1b[<0;8;4mX", "one\ntXwo", mouse_pixels=True)
check("pixel mouse selection", "one\ntwo\nthree",
      b"\x1b[<0;7;3M\x1b[<32;10;4M\x1b[<0;10;4m\x18", "\nthree", mouse_pixels=True)
check("pixel mouse wheel", "zero\none\ntwo\nthree\nfour", b"\x1b[<65;10;5MX",
      "zero\none\ntwo\nXthree\nfour", mouse_pixels=True)
check("slider incomplete call typing", '',
      b's("sawtooth").lpf(200).lpenv(slider(', 's("sawtooth").lpf(200).lpenv(slider(')
check("slider empty call typing", '',
      b's("sawtooth").lpf(200).lpenv(slider())', 's("sawtooth").lpf(200).lpenv(slider())')
check("slider expression typing", '',
      b's("sawtooth").lpf(200).lpenv(slider(2.28,0,8))', 's("sawtooth").lpf(200).lpenv(slider(2.28,0,8))')

check("pixel mouse slider", 's("hh").gain(slider(.5)).gain(0)',
      b"\x1b[<0;65;3M\x1b[<0;65;3m", 's("hh").gain(slider(1)).gain(0)',
      observed=b"slider 1 = 1", mouse_pixels=True)
check("pixel mouse pane focus", "one", b"\x1b[<0;90;4M\x1b[<0;90;4mX\x1b[<0;7;3MY", "Yone", mouse_pixels=True)
check("mouse shift selection", "abcdef",
      b"\x1b[<0;8;3M\x1b[<0;8;3m\x1b[<4;11;3M\x1b[<4;11;3m\x18", "aef")
check("pixel mouse shift selection", "abcdef",
      b"\x1b[<0;8;3M\x1b[<0;8;3m\x1b[<4;11;3M\x1b[<4;11;3m\x18", "aef", mouse_pixels=True)
check("mouse disabled ignores reports", "one\ntwo", b"\x1b[<0;8;4M\x1b[<0;8;4mX", "Xone\ntwo", mouse=False)
check("pixel mouse disabled ignores reports", "one\ntwo", b"\x1b[<0;8;4M\x1b[<0;8;4mX", "Xone\ntwo", mouse=False, mouse_pixels=True)
check("mouse selection", "one\ntwo\nthree",
      b"\x1b[<0;7;3M\x1b[<32;10;4M\x1b[<0;10;4m\x18", "\nthree")
check("pane focus", "one", b"\x1b[<0;90;4M\x1b[<0;90;4mX\x1b[<0;7;3MY", "Yone")
check("mouse slider", 's("hh").gain(slider(.5)).gain(0)',
      b"\x1b[<0;65;3M\x1b[<0;65;3m", 's("hh").gain(slider(1)).gain(0)', observed=b"slider 1 = 1")
check("live slider keyboard preserves program", 'note(60).gain(slider(.5)).gain(0)',
      b'\x1b[1;5C', 'note(60).gain(slider(0.501)).gain(0)', observed=b'slider 1 = 0.501', live_slider=True)
check("live slider dirty source stays unapplied", 'note(60).gain(slider(.5)).gain(0)',
      b'// pending\r\x1b[1;5C', '// pending\nnote(60).gain(slider(0.501)).gain(0)',
      observed=b'slider 1 = 0.501', live_slider=True)
check("live slider mouse preserves program", 'note(60).gain(slider(.5)).gain(0)',
      b'\x1b[<0;65;3M\x1b[<0;65;3m', 'note(60).gain(slider(1)).gain(0)',
      observed=b'slider 1 = 1', live_slider=True)
check("live slider pixel mouse preserves program", 'note(60).gain(slider(.5)).gain(0)',
      b'\x1b[<0;65;3M\x1b[<0;65;3m', 'note(60).gain(slider(1)).gain(0)',
      observed=b'slider 1 = 1', live_slider=True, mouse_pixels=True)
check("performance activation F5 starts stopped editor", 'note(60).gain(0)',
      b'\x10\x1b[F;note(72).gain(0)\x1b[15~\x1bOS',
      expected='note(60).gain(0);note(72).gain(0)',transport=True,source_state=b'evaluated')
check("performance activation Play evaluates pending editor", 'note(60).gain(0)',
      b'\x10\x1b[F;note(72).gain(0)\x10\x1bOS',
      expected='note(60).gain(0);note(72).gain(0)',transport=True,source_state=b'evaluated')
check("performance activation play command evaluates pending editor", 'note(60).gain(0)',
      b'\x10\x1b[F;note(72).gain(0)\x1bOQ/play\r\x1bOS',
      expected='note(60).gain(0);note(72).gain(0)',transport=True,source_state=b'evaluated')
check("performance activation vi eval starts stopped editor", 'note(60).gain(0)',
      b'\x10A;note(72).gain(0)\x1b:eval\r\x1bOS',vi=True,
      expected='note(60).gain(0);note(72).gain(0)',transport=True,source_state=b'evaluated')
check("performance activation failed F5 stays stopped", 'note(60).gain(0)',
      b'\x10\x1b[F;throw Error("bad edit")\x1b[15~\x1bOS',
      expected='note(60).gain(0);throw Error("bad edit")',transport=False,source_state=b'pending F5')
check("performance activation failed Play stays stopped", 'note(60).gain(0)',
      b'\x10\x1b[F;throw Error("bad edit")\x10\x1bOS',
      expected='note(60).gain(0);throw Error("bad edit")',transport=False,source_state=b'pending F5')
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
check("Strudel method Tab completion", 'note(60).ga', b'\x1b[F\t(0)', expected='note(60).gain(0)')
check("Strudel method Ctrl Space completion", 'note(60).ga', b'\x1b[F\x00(0)', expected='note(60).gain(0)')
check("Strudel sound completion", 'note(60).s("squ', b'\x1b[F\t").gain(0)', expected='note(60).s("square").gain(0)')
check("Strudel completion in vi insert", 'note(60).ga', b'A\t(0)\x1b', expected='note(60).gain(0)', vi=True)
check("Strudel completion undo", 'note(60).ga', b'\x1b[F\t\x1a', expected='note(60).ga')
check("vi command entry", 'one', b':set nowrap\r', expected='one', vi=True, observed=b'nowrap')
check("vi command from console focus", 'one', b'\x1b[<0;90;4M\x1b[<0;90;4m:set nowrap\r',
      expected='one', vi=True, observed=b'nowrap')
check("vi command Tab completion", 'one', b':set now\t\r', expected='one', vi=True, observed=b'nowrap')
check("vi command reverse completion", 'one', b':set no\t\t\x1b[Z\r', expected='one', vi=True, observed=b'nowrap')
check("vi command line jump", 'one\ntwo\nthree', b':2\rIX\x1b', expected='one\nXtwo\nthree', vi=True)
check("vi command undo redo", 'one', b'iX\x1b:undo\r:redo\r', expected='Xone', vi=True)
check("vi command quit guard", 'one', b'iX\x1b:q\r', expected='Xone', vi=True, observed=b'E37')
check("vi command F5 retains quit guard", 'note(60).gain(0)', b'I//\x1b\x1b[15~:q\r',
      expected='//note(60).gain(0)', vi=True, observed=b'E37')
check("vi command cancel", 'one', b':set nowrap\x1bIX\x1b', expected='Xone', vi=True)
check("vi command unknown stays local", 'one', b':missingcommand\r', expected='one', vi=True, observed=b'E492')
check("vi command shell stays local", 'one', b':!touch should-not-run\r', expected='one', vi=True, observed=b'E492')
check("vi command write quit", 'note(60).gain(0)', b':wq Saved.json\r', expected='note(60).gain(0)',
      vi=True, command_files=True, saved_file='Saved.json')
check("vi command write failure stays open", 'one', b':wq missing/song.json\r', expected='one',
      vi=True, command_files=True, observed=b'save failed')
check("vi command overwrite guard", 'one', b':w Existing.json\r', expected='one',
      vi=True, command_files=True, observed=b'E13')
check("vi command forced overwrite", 'one', b':w! Existing.json\r', expected='one',
      vi=True, command_files=True, saved_file='Existing.json')
check("vi command filename completion", 'one', b':e songs/My\t\r', expected='note(72).gain(0)',
      vi=True, command_files=True)
check("vi command quoted filename completion", 'one', b':e "songs/My\t\r', expected='note(72).gain(0)',
      vi=True, command_files=True)
check("vi command edit guard", 'one', b'iX\x1b:e Existing.json\r', expected='Xone',
      vi=True, command_files=True, observed=b'E37')
check("vi command forced edit", 'one', b'iX\x1b:e! Existing.json\r', expected='note(72).gain(0)',
      vi=True, command_files=True)
check("vi command forced quit", 'one', b'iX\x1b:q!\r', expected='Xone', vi=True)
check("prompt command completion", 'one', b'\x1bOQ/wra\t of\t\r', expected='one', observed=b'wrap off')

# Tutorial startup is deliberately silent; run from an unrelated directory.
if not CASE_FILTER or 'tutor' in CASE_FILTER or CASE_FILTER in 'tutor shortcut':
    tutorial = Path(BINARY).resolve().parent/'examples/strudeltutor.json'
    tutorial_before = tutorial.read_bytes()
    tutor_intro = json.loads(tutorial_before)['code'].rstrip('\n')
    check('tutor slash shortcut', 'one', b'\x1bOQ/tutor\r', expected=tutor_intro, command_files=True, tutor_saved=True)
    check('tutor vi shortcut', 'one', b':tutor\r', expected=tutor_intro, vi=True, command_files=True, tutor_saved=True)
    check('tutor slash completion', 'one', b'\x1bOQ/tu\t\r', expected=tutor_intro, command_files=True)
    check('tutor vi completion', 'one', b':tu\t\r', expected=tutor_intro, vi=True, command_files=True)
    check('tutor slash unsaved guard', 'one', b'X\x1bOQ/tutor\r', expected='Xone', observed=b'E37')
    check('tutor vi unsaved guard', 'one', b'iX\x1b:tutor\r', expected='Xone', vi=True, observed=b'E37')
    check('tutor slash force', 'one', b'X\x1bOQ/tutor!\r', expected=tutor_intro, command_files=True)
    check('tutor vi force', 'one', b'iX\x1b:tutor!\r', expected=tutor_intro, vi=True, command_files=True)
    check('tutor config save with w', 'one', b':tutor\r:w\r', expected=tutor_intro,
          vi=True, command_files=True, tutor_saved=True, observed=b'saved')
    check('tutor config slash save', 'one', b'\x1bOQ/tutor\r\x1bOQ/save\r', expected=tutor_intro,
          command_files=True, tutor_saved=True, observed=b'saved')
    progress='// my tutorial progress\n'+tutor_intro
    check('tutor Ctrl S saves config edits', 'one', b':tutor\rggI// my tutorial progress\r\x1b\x13\r',
          expected=progress, vi=True, command_files=True, tutor_saved=True)
    check('tutor reopen preserves saved edits', 'one', b':tutor\rggI// my tutorial progress\r\x1b:w\r:tutor\r',
          expected=progress, vi=True, command_files=True, tutor_saved=True)
    check('tutor existing copy resumed', 'one', b'\x1bOQ/tutor\r', expected=progress,
          command_files=True, tutor_existing=progress, tutor_saved=True)
    check('tutor force preserves existing progress', 'one', b'iX\x1b:tutor!\r', expected=progress,
          vi=True, command_files=True, tutor_existing=progress, tutor_saved=True)
    check('tutor save working copy', 'one', b':tutor\r:wq Work.json\r', expected=tutor_intro,
          vi=True, command_files=True, saved_file='Work.json')
    check('tutor disables song advancement', 'one', b'\x1bOQ/load Existing.json\r\x1bOQ/song on\r\x1bOQ/tutor!\r\x1bOS',
          expected=tutor_intro, command_files=True, observed=b'song=off')
    assert tutorial.read_bytes() == tutorial_before, 'tutor shortcut changed the bundled save'
check("Notcurses CSI-u completion", "", b'bp\x1b[32;5u', expected='bpm')
check("Notcurses shifted associated text", "", b'\x1b[97;2;65u\x1b[49;2;33u\x1b[57;2;40u', expected='A!(')
check("Notcurses shifted plain text", "", b'A!(){}_+"', expected='A!(){}_+"')
check("Notcurses associated Unicode text", "", b'\x1b[101;1;101;769u\x1b[50;2;233u', expected='e\u0301é')
check("Notcurses shifted repeat and release", "", b'\x1b[49;2:1;33u\x1b[49;2:2;33u\x1b[49;2:3;33u', expected='!!')
check("Notcurses key release ignored", "", b'\x1b[88;1:3u\x1b[120;1:1u', expected='x')
large_paste = '// ' + 'café ' * 2048
check("Notcurses bounded input pipe preserves large paste", "",
      b'\x1b[200~' + large_paste.encode() + b'\x1b[201~', expected=large_paste)
check("Selected block unindent", "one\ntwo", b'\x01\t\x1b[Z', expected='one\ntwo')
check("Live theme file watching", "note('c4').gain(0)", expected="note('c4').gain(0)", theme=True)
check("categorized sound search and insertion", "", b'\x1b[17~supersaw\r',
      expected='note("c4").s("supersaw")')
check("sound category navigation", "", b'\x1b[17~\x1b[C\x1b[C\x1b[Csine\r',
      expected='note("c4").s("sine")')
check("GM synth search and insertion", "", b'\x1b[17~gm_flute\r', expected='note("c4").s("gm_flute")')
check("GM synth category", "", b'\x1b[17~\x1b[C\x1b[C\x1b[Cgm_acoustic_bass\r',
      expected='note("c4").s("gm_acoustic_bass")')
check("sound search survives category changes", "", b'\x1b[17~supersaw\x1b[C\x1b[C\x1b[C\r',
      expected='note("c4").s("supersaw")')
check("sound search persistent categories", "", b'\x1b[17~',
      observed=b'user sounds', stop_signal=signal.SIGTERM)
check("manifest machine bank list", "", b'\x1b[17~fixture909',
      observed=b'fixture909/ (2)', manifest=True, stop_signal=signal.SIGTERM)
check("manifest bank component insertion", "", b'\x1b[17~fixture909\r\x1b[B\r',
      expected='s("fixture909_bd")', manifest=True)
check("mouse sound browser bank row", "", b'\x1b[17~fixture909\r\x1b[<0;80;10M\x1b[<0;80;10m\r',
      expected='s("fixture909_sd")', manifest=True)
check("pixel mouse sound browser bank row", "", b'\x1b[17~fixture909\r\x1b[<0;80;10M\x1b[<0;80;10m\r',
      expected='s("fixture909_sd")', manifest=True, mouse_pixels=True)
check("mouse sound browser scrolled row", "", b'\x1b[17~scrollfixture\x1b[6~\x1b[6~\x1b[6~\x1b[<0;80;8M\x1b[<0;80;8m\r',
      expected='s("scrollfixture09")', manifest=True)
check("pixel mouse sound browser scrolled row", "", b'\x1b[17~scrollfixture\x1b[6~\x1b[6~\x1b[6~\x1b[<0;80;8M\x1b[<0;80;8m\r',
      expected='s("scrollfixture09")', manifest=True, mouse_pixels=True)
check("manifest whole bank insertion", "", b'\x1b[17~fixture909\t',
      expected='s("bd sd").bank("fixture909")', manifest=True)
check("manifest local banks category", "", b'\x1b[17~\x1b[Dfixture909\t',
      expected='s("bd sd").bank("fixture909")', manifest=True)
check("manifest sample array stays one entry", "", b'\x1b[17~\x1b[Cfixturebell\r',
      expected='s("fixturebell")', manifest=True)
check("Strudel wavetable category and insertion", "", b'\x1b[17~\x1b[C\x1b[C\x1b[C\x1b[Cwt_test\r',
      expected='note("c4").s("wt_test")', manifest=True)
check("Strudel wavetable variants stay one bank", "", b'\x1b[17~wt_test', observed=b'wt_test (2)',
      manifest=True, stop_signal=signal.SIGTERM)
check("manifest covered files hidden", "", b'\x1b[17~z.wav', observed=b'0 sounds', manifest=True, stop_signal=signal.SIGTERM)
check("manifest uncovered file insertion", "", b'\x1b[17~stray.wav\r', expected='s(pure("stray.wav"))', manifest=True)
check("manifest uncovered file with spaces", "", b'\x1b[17~my drum.wav\r', expected='s(pure("My Drum.wav"))', manifest=True)
check("manifest bank back navigation", "", b'\x1b[17~fixture909\r\x7f',
      observed=b'fixture909/ (2)', manifest=True, stop_signal=signal.SIGTERM)
check("sound folder import and insertion", "", import_sounds=True, expected='s("my_drums")')
check("inline braille scope", "const melody=note('c4').gain(0); melody._scope()",
      expected="const melody=note('c4').gain(0); melody._scope()", observed=b"scope | pattern output")
check("inline braille piano roll", 'note("c4 e4 g4").gain(0)._pianoroll()',
      expected='note("c4 e4 g4").gain(0)._pianoroll()', observed=b'piano roll | pattern notes')
check("inline braille live piano roll without F5", 'note("60 64").gain(0)', b'\x1b[F._pianoroll()',
      expected='note("60 64").gain(0)._pianoroll()', observed=b'pattern notes')
check("inline braille live scope without F5", 'note("60").gain(0)', b'\x1b[F._scope()',
      expected='note("60").gain(0)._scope()', observed=b'pattern output')
check("inline braille live options without F5", 'note("60 64").gain(0)._pianoroll()',
      b'\x01\x1b[200~note("60 64").gain(0)._pianoroll({vertical:1})\x1b[201~',
      expected='note("60 64").gain(0)._pianoroll({vertical:1})', observed=b'piano roll | pattern notes')
check("inline braille loaded song without F5", 'note(60).gain(0)',
      expected='note("60 64").gain(0)._pianoroll()', observed=b'pattern notes', load_song=True)
check("Kitty live piano roll without F5", 'note("60 64").gain(0)', b'\x1b[F._pianoroll()',
      expected='note("60 64").gain(0)._pianoroll()', graphics='live')
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
check('Kitty Live theme file watching', graphics_code, expected=graphics_code, theme=True, graphics='supported')
check('Kitty terminal palette', graphics_code, expected=graphics_code, graphics='supported')
check('Kitty graphics capability, resize and editor input', graphics_code, b'X', expected='X'+graphics_code, graphics='supported')
check('Kitty unsupported braille fallback', graphics_code, expected=graphics_code, graphics='unsupported')
check('Kitty manual braille fallback', graphics_code, b'\x1bOQ/graphics braille\r', expected=graphics_code, graphics='braille')
check('Kitty hide inline cleanup', graphics_code, b'\x1bOQ/inline off\r', expected=graphics_code, graphics='supported')
scroll_graphics_code = graphics_code+'\n'+'\n'.join('// scroll '+str(i) for i in range(40))
check('Kitty scrolling removes images', scroll_graphics_code, expected=scroll_graphics_code, graphics='scroll')
