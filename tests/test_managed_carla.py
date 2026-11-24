#!/usr/bin/env python3
"""Owned managed-Carla integration: project startup, MIDI link and shutdown.

The test generates a real Carla SF2 project through Carla's Python API, starts
one Tompouce daemon with an isolated config, captures the project output, and
verifies that stopping playback removes the owned Carla node and process. It
uses a unique null sink, MIDI client and project name and restores the previous
default sink. Unsupported systems skip with a message.
"""
import array
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = os.environ.get("TOMPOUCE_TEST_BINARY", str(ROOT / "build" / "tompouce"))
CARLA_LIB = Path("/usr/lib/carla/libcarla_standalone2.so")
CARLA_BACKEND = Path("/usr/share/carla/carla_backend.py")
SOUNDFONT = Path("/usr/share/soundfonts/FluidR3_GM.sf2")


def log(message):
    print(message, flush=True)


def skip(message):
    log(f"managed-carla skipped: {message}")
    raise SystemExit(0)


def command(argv, timeout=15, check=True):
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                          check=check)


def wait_for(predicate, timeout, message):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        time.sleep(0.2)
    raise RuntimeError(message)


def pw_dump():
    return json.loads(command(["pw-dump"], timeout=10).stdout)


def node_id(name):
    return any(obj.get("info", {}).get("props", {}).get("node.name") == name
               for obj in pw_dump())


def link_text():
    return command(["pw-link", "-l"], timeout=10).stdout


def link(source, target):
    result = subprocess.run(["pw-link", source, target], capture_output=True,
                            text=True, timeout=10)
    if result.returncode and "File exists" not in result.stderr:
        raise RuntimeError(f"pw-link {source} -> {target}: {result.stderr.strip()}")


def daemon_call(daemon_dir, name, arguments=None):
    result = subprocess.run([
        sys.executable, str(ROOT / "scripts" / "tompoucectl.py"),
        "--daemon-dir", str(daemon_dir), "--name", name,
        "--args", json.dumps(arguments or {})], capture_output=True, text=True,
        timeout=15)
    if result.returncode:
        raise RuntimeError(f"{name}: {result.stdout.strip()} {result.stderr.strip()}")
    return result.stdout.strip()


def generate_project(root, target):
    project = root / "config" / "tompouce" / "carla" / "managed.carxp"
    project.parent.mkdir(parents=True, exist_ok=True)
    script = root / "make-project.py"
    script.write_text(
        "import sys, time\n"
        "sys.path.insert(0, '/usr/share/carla')\n"
        "import carla_backend as cb\n"
        f"h = cb.CarlaHostDLL({str(CARLA_LIB)!r}, True)\n"
        "h.set_engine_option(cb.ENGINE_OPTION_PROCESS_MODE, "
        "cb.ENGINE_PROCESS_MODE_PATCHBAY, '')\n"
        "h.set_engine_option(cb.ENGINE_OPTION_AUDIO_DRIVER, 0, 'JACK')\n"
        "if not h.engine_init('JACK', 'managed-carla-test'): "
        "raise SystemExit(h.get_last_error())\n"
        "if not h.add_plugin(cb.BINARY_NATIVE, cb.PLUGIN_SF2, "
        f"{str(SOUNDFONT)!r}, {target!r}, {target!r}, 0, None, 0): "
        "raise SystemExit(h.get_last_error())\n"
        "time.sleep(1); h.engine_idle()\n"
        f"if not h.save_project({str(project)!r}): "
        "raise SystemExit(h.get_last_error())\n"
        "h.engine_close()\n")
    result = subprocess.run(["pw-jack", sys.executable, str(script)],
                            capture_output=True, text=True, timeout=90)
    if result.returncode or not project.is_file():
        raise RuntimeError("Carla project generation failed: " + result.stderr[-500:])
    return project


def capture_rms(path):
    with wave.open(str(path)) as audio:
        frames = audio.readframes(audio.getnframes())
        samples = array.array("h")
        samples.frombytes(frames)
    if not samples:
        return 0, 0
    peak = max(abs(value) for value in samples)
    rms = (sum(value * value for value in samples) / len(samples)) ** 0.5
    return peak, rms


def main():
    if os.environ.get("TOMPOUCE_TEST_MANAGED_CARLA") == "0":
        skip("disabled")
    for tool in ("carla", "pw-jack", "pw-link", "pw-dump", "pw-record", "pactl"):
        if not shutil.which(tool):
            skip(f"{tool} unavailable")
    if not CARLA_LIB.is_file() or not CARLA_BACKEND.is_file() or not SOUNDFONT.is_file():
        skip("Carla backend or soundfont unavailable")
    if not Path(BINARY).is_file():
        skip(f"binary unavailable: {BINARY}")

    root = Path(tempfile.mkdtemp(prefix="tompouce-managed-carla-"))
    for name in ("config", "cache", "state", "daemon"):
        (root / name).mkdir()
    target = f"tompouce-managed-{os.getpid()}"
    midi_client = f"tompouce-carla-{os.getpid()}"
    sink = f"tompouce-carla-sink-{os.getpid()}"
    recorder_name = f"tompouce-carla-rec-{os.getpid()}"
    module = None
    old_sink = None
    engine = None
    recorder = None
    try:
        old_sink = command(["pactl", "get-default-sink"]).stdout.strip()
        module = command(["pactl", "load-module", "module-null-sink",
                          f"sink_name={sink}",
                          f"sink_properties=device.description={sink}"]).stdout.strip()
        command(["pactl", "set-default-sink", sink])
        wait_for(lambda: node_id(sink), 10, "owned null sink did not appear")

        project = generate_project(root, target)
        token = os.urandom(32).hex()
        token_file = root / "daemon" / "token"
        token_file.write_text(token)
        token_file.chmod(0o600)
        env = dict(os.environ,
                   XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CACHE_HOME=str(root / "cache"),
                   XDG_STATE_HOME=str(root / "state"),
                   TOMPOUCE_MCP_TOKEN=token,
                   TOMPOUCE_AUDIO_BACKEND="PipeWire",
                   TOMPOUCE_MIDI_CLIENT=midi_client,
                   TOMPOUCE_CARLA_ROOT=str(project.parent),
                   TOMPOUCE_CARLA="/usr/bin/carla")
        code = (f"note(\"c4 ~ c4 ~\").ccn(7).ccv(0.8).midi('PipeWire')"
                f".carla('managed.carxp','{target}').gain(.8)")
        engine_log = (root / "engine.log").open("wb")
        engine = subprocess.Popen([
            str(Path(BINARY).resolve()), "--mcp-socket", str(root / "daemon" / "mcp.sock"),
            "--bpm", "120", "--code", code], env=env, stdout=engine_log,
            stderr=subprocess.STDOUT)
        socket_path = root / "daemon" / "mcp.sock"
        wait_for(socket_path.exists, 20, "engine socket did not appear")
        daemon_call(root / "daemon", "music_play")
        wait_for(lambda: "carla=1/1" in daemon_call(root / "daemon", "music_health"),
                 30, "managed Carla did not become ready; see engine.log")
        wait_for(lambda: node_id(target), 10, "Carla project node did not appear")
        source = f"Midi-Bridge:{midi_client}:out (capture)"
        destination = f"{target}:events-in"
        wait_for(lambda: source in link_text() and f"|-> {destination}" in link_text(),
                 10, "managed Carla MIDI link did not appear")

        wav = root / "capture.wav"
        recorder_log = (root / "record.log").open("wb")
        recorder = subprocess.Popen([
            "pw-record", "-P", f'{{ node.name = "{recorder_name}" }}',
            "--target", "0", "--rate", "44100", "--channels", "2", str(wav)],
            stdout=recorder_log, stderr=subprocess.STDOUT)
        wait_for(lambda: any(line.strip().startswith(recorder_name + ":input_FL")
                             for line in command(["pw-link", "-i"]).stdout.splitlines()),
                 15, "capture node did not appear")
        link(f"{target}:out-left", f"{recorder_name}:input_FL")
        time.sleep(2.0)
        if recorder.poll() is None:
            recorder.terminate()
            recorder.wait(timeout=10)
        recorder = None
        peak, rms = capture_rms(wav)
        assert peak > 100 and rms > 10, f"Carla output was silent (peak={peak}, rms={rms:.1f})"

        daemon_call(root / "daemon", "music_stop")
        wait_for(lambda: "carla=0/0" in daemon_call(root / "daemon", "music_health"),
                 10, "managed Carla did not stop with playback")
        wait_for(lambda: not node_id(target), 10, "Carla node remained after stop")
        engine.terminate()
        engine.wait(timeout=10)
        engine = None
        log(f"managed Carla ok: node={target} peak={peak} rms={rms:.1f}")
        return 0
    except (RuntimeError, subprocess.SubprocessError) as error:
        log(f"managed-carla: {error}")
        return 1
    finally:
        if engine and engine.poll() is None:
            engine.terminate()
            try:
                engine.wait(timeout=10)
            except subprocess.TimeoutExpired:
                engine.kill()
        if recorder and recorder.poll() is None:
            recorder.kill()
        if old_sink:
            command(["pactl", "set-default-sink", old_sink], check=False)
        if module:
            command(["pactl", "unload-module", module], check=False)
        subprocess.run(["pkill", "-f", str(root / "config" / "tompouce" / "carla" / "managed.carxp")],
                       capture_output=True, text=True, check=False)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
