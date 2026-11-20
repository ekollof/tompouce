#!/usr/bin/env python3
"""Relative MIDI-to-audio latency through a separate Carla SF2 synth.

Diagnostic only: not part of ctest and not a parity gate. It needs a built
tompouce with --mcp-socket support, Carla's Python backend and a SoundFont,
pw-jack/pw-link/pw-record/pw-dump, pactl, numpy and scipy. Set
TOMPOUCE_TEST_BINARY or pass --binary to check an installed executable.

Everything here is owned: a unique temp dir, one PipeWire null sink, one
headless Carla instance, one tompouce daemon and one recorder. The script never
kills or reconfigures the user's engine, sink or desktop routing. Cleanup only
touches the PIDs and the sink module it created.

The engine's local output and the synth's output are linked to the same owned
null sink and captured from its monitor, so both onset streams share one audio
clock. A pattern alternates a local onset and a `.midi()` onset one second
apart within a two-second cycle. The reported figure is the extra delay of
each synth onset after the preceding local onset -- a relative number, not
absolute sound-card or round-trip latency. See docs/plugin-hosting.md.

Usage:
  scripts/midi-latency-qa.py [--root DIR] [--binary PATH] [--duration SECONDS]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parent.parent
PATTERN = ('stack(note("c5 ~ ~ ~").s("sine").gain(.6),'
           'note("~ ~ c5 ~").s("sine").midi().gain(.9))')
EXPECTED_GAP = 1.0
PROCS = []


def log(message):
    print(message, flush=True)


def command(argv, timeout=15):
    return subprocess.run(argv, check=True, capture_output=True, text=True,
                          timeout=timeout)


def wait_until(predicate, timeout, message):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        time.sleep(0.25)
    raise RuntimeError(message)


def pw_nodes():
    return json.loads(command(["pw-dump"], timeout=10).stdout)


def node_id(name, nodes=None):
    if nodes is None:
        nodes = pw_nodes()
    found = [obj.get("id") for obj in nodes
             if obj.get("info", {}).get("props", {}).get("node.name") == name]
    return found[0] if len(found) == 1 else None


def port_exists(port, direction):
    lines = command(["pw-link", direction], timeout=10).stdout.splitlines()
    return any(line.strip().split(" -> ")[0] == port for line in lines
               if " -> " in line) or any(line.strip() == port for line in lines)


def link(source, destination):
    listing = command(["pw-link", "-l"], timeout=10).stdout
    if f"{source} -> {destination}" in listing:
        return
    result = subprocess.run(["pw-link", source, destination],
                            capture_output=True, text=True, timeout=10)
    if result.returncode and "File exists" not in result.stderr:
        raise RuntimeError(f"pw-link {source} {destination}: {result.stderr.strip()}")


def generate_project(root, soundfont, carla_lib):
    project = root / "sf2.carxp"
    script = root / "make_sf2.py"
    script.write_text(
        "import sys, time\n"
        "sys.path.insert(0, '/usr/share/carla')\n"
        "import carla_backend as cb\n"
        f"h = cb.CarlaHostDLL({str(carla_lib)!r}, True)\n"
        "h.set_engine_option(cb.ENGINE_OPTION_PROCESS_MODE, "
        "cb.ENGINE_PROCESS_MODE_PATCHBAY, '')\n"
        "h.set_engine_option(cb.ENGINE_OPTION_AUDIO_DRIVER, 0, 'JACK')\n"
        "if not h.engine_init('JACK', 'latency-qa'): raise SystemExit(h.get_last_error())\n"
        "if not h.add_plugin(cb.BINARY_NATIVE, cb.PLUGIN_SF2, "
        f"{str(soundfont)!r}, 'sf2synth', 'sf2synth', 0, None, 0): "
        "raise SystemExit(h.get_last_error())\n"
        "time.sleep(2); h.engine_idle()\n"
        f"if not h.save_project({str(project)!r}): raise SystemExit(h.get_last_error())\n"
        "h.engine_close()\n"
    )
    command(["pw-jack", "python3", str(script)], timeout=60)
    if not project.is_file():
        raise RuntimeError("Carla did not write the SF2 project")
    return project


def start(argv, env, logfile):
    handle = logfile.open("wb")
    process = subprocess.Popen(argv, env=env, stdout=handle,
                               stderr=subprocess.STDOUT)
    PROCS.append(process)
    return process


def call_daemon(daemon_dir, name, arguments=None):
    argv = [sys.executable, str(ROOT / "scripts" / "tompoucectl.py"),
            "--daemon-dir", str(daemon_dir), "--name", name,
            "--args", json.dumps(arguments or {})]
    result = subprocess.run(argv, capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise RuntimeError(f"{name}: {result.stdout.strip()} {result.stderr.strip()}")
    return result.stdout.strip()


def onsets(path, channel):
    rate, samples = wavfile.read(path)
    data = samples.astype(float)
    if data.ndim == 1:
        if channel:
            raise RuntimeError("capture has only one channel")
    else:
        data = data[:, channel]
    width = 44
    energy = np.sqrt(np.convolve(data * data, np.ones(width) / width,
                                 mode="same"))
    high = energy > 1000
    rising = np.where(high & ~np.r_[False, high[:-1]])[0]
    detected = []
    for index in rising:
        when = index / rate
        if not detected or when - detected[-1] > 0.05:
            detected.append(when)
    return detected


def latency(local, midi):
    if len(local) < 4 or len(midi) < 4:
        raise RuntimeError(f"too few onsets: {len(local)} local, {len(midi)} MIDI")
    pairs = []
    for when in midi:
        previous = [o for o in local if o < when]
        if previous:
            gap = when - previous[-1]
            if EXPECTED_GAP - 0.1 < gap < EXPECTED_GAP + 0.1:
                pairs.append((gap - EXPECTED_GAP) * 1000)
    if len(pairs) < 4:
        raise RuntimeError(f"only {len(pairs)} paired onsets; MIDI link may be missing")
    return np.array(pairs)


def cleanup(root, sink_module, keep=False):
    for process in reversed(PROCS):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    if sink_module:
        try:
            command(["pactl", "unload-module", sink_module], timeout=10)
        except subprocess.SubprocessError as error:
            log(f"warn: could not unload null sink {sink_module}: {error}")
    if not keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        log(f"kept owned state in {root}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", help="parent dir for owned temp state")
    parser.add_argument("--binary", default=os.environ.get(
        "TOMPOUCE_TEST_BINARY", str(ROOT / "build" / "tompouce")))
    parser.add_argument("--duration", type=float, default=28.0,
                        help="capture seconds (default: 28)")
    parser.add_argument("--keep", action="store_true",
                        help="keep the owned temp dir (for debugging)")
    parser.add_argument("--soundfont",
                        default="/usr/share/soundfonts/FluidR3_GM.sf2")
    parser.add_argument("--carla-lib",
                        default="/usr/lib/carla/libcarla_standalone2.so")
    args = parser.parse_args()

    binary = Path(args.binary)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"tompouce executable not found: {binary}")
    if not Path(args.soundfont).is_file():
        parser.error(f"SoundFont not found: {args.soundfont}")
    for tool in ("pw-jack", "pw-link", "pw-record", "pw-dump", "pactl", "carla"):
        if not shutil.which(tool):
            parser.error(f"missing tool: {tool}")

    base = Path(args.root) if args.root else Path(tempfile.gettempdir())
    root = Path(tempfile.mkdtemp(prefix="tompouce-midi-latency-", dir=base))
    sink_module = None
    sink = f"tompouce_lat_{os.getpid()}"
    env = os.environ.copy()
    env.update(XDG_CONFIG_HOME=str(root / "config"),
               XDG_CACHE_HOME=str(root / "cache"),
               XDG_DATA_HOME=str(root / "data"))
    try:
        (root / "daemon").mkdir(mode=0o700)
        for folder in ("config", "cache", "data"):
            (root / folder).mkdir()
        token = os.urandom(32).hex()
        token_file = root / "daemon" / "token"
        token_file.write_text(token)
        token_file.chmod(0o600)

        sink_module = command([
            "pactl", "load-module", "module-null-sink",
            f"sink_name={sink}", f"sink_properties=device.description={sink}",
        ], timeout=10).stdout.strip()
        wait_until(lambda: node_id(sink) is not None, 10,
                   f"owned null sink {sink} did not appear")

        project = generate_project(root, args.soundfont, args.carla_lib)
        start(["pw-jack", "carla", "-n", str(project)], env,
              root / "carla.log")
        wait_until(lambda: node_id("sf2synth") is not None, 30,
                   "sf2synth node did not appear; check carla.log")

        env["TOMPOUCE_MCP_TOKEN"] = token
        start([str(binary), "--mcp-socket", str(root / "daemon" / "mcp.sock"),
               "--bpm", "120", "--code", "note(60)"], env, root / "engine.log")
        wait_until(lambda: (root / "daemon" / "mcp.sock").exists(), 20,
                   "daemon socket did not appear; check engine.log")

        call_daemon(root / "daemon", "music_set_code", {"code": PATTERN})
        time.sleep(2)

        midi_out = "Midi-Bridge:tompouce:out (capture)"
        if not port_exists(midi_out, "-o"):
            raise RuntimeError(f"MIDI source port not found: {midi_out}")
        link(midi_out, "sf2synth:events-in")
        link("tompouce:output_FL", f"{sink}:playback_FL")
        link("sf2synth:out-left", f"{sink}:playback_FL")

        recorder_name = f"tompouce-lat-rec-{os.getpid()}"
        wav = root / "capture.wav"
        start(["pw-record", "-P", f'{{ node.name = "{recorder_name}" }}',
               "--target", "0", "--rate", "44100", "--channels", "2",
               str(wav)], env, root / "record.log")
        wait_until(lambda: node_id(recorder_name) is not None, 15,
                   "recorder node did not appear; check record.log")
        link("tompouce:output_FL", f"{recorder_name}:input_FL")
        link("sf2synth:out-left", f"{recorder_name}:input_FR")
        log(f"capturing for {args.duration:g}s ...")
        time.sleep(args.duration)
        recorder = PROCS[-1]
        if recorder.poll() is None:
            recorder.terminate()
            recorder.wait(timeout=10)

        pairs = latency(onsets(wav, 0), onsets(wav, 1))
        log(f"paired onsets: {len(pairs)}")
        log(f"relative MIDI-to-audio latency: median={np.median(pairs):.3f} ms "
            f"min={pairs.min():.3f} ms max={pairs.max():.3f} ms "
            f"std={np.std(pairs):.3f} ms")
        if not 0 <= np.median(pairs) < 100:
            raise RuntimeError("implausible latency; inspect the capture")
        return 0
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        log(f"midi-latency-qa: {error}")
        return 1
    finally:
        cleanup(root, sink_module, args.keep)


if __name__ == "__main__":
    sys.exit(main())
