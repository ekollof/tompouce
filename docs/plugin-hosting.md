# External plugin hosting

Tompouce does not host plugins inside the engine. Arbitrary plugin code runs in
separate processes, outside the audio callback and outside the QuickJS contexts.
The first usable path needs no Tompouce code at all.

## Zero-code path: Carla as a separate patchbay

[Carla](https://github.com/falkTX/Carla) is a standalone host for LADSPA, DSSI,
LV2, VST2, VST3, SF2 and SFZ plugins, including Wine bridges such as yabridge.
It is GPL-2.0-or-later, so using it as a separate process does not change
Tompouce's licensing. Plugins, their installers and their licenses stay with the
user.

On a PipeWire desktop, keep Tompouce on its default PipeWire output (F4 shows
`backend=PipeWire`) and bridge Carla in through PipeWire's JACK compatibility
layer. No Tompouce setting or pattern change is needed:

1. Load the plugins and build the graph in Carla (GUI: `pw-jack carla`; headless
   needs a saved project: `pw-jack carla -n /path/to/project.carxp`). On a native
   JACK box, run Carla directly in the JACK graph instead of using `pw-jack`.
2. In the Carla patchbay, connect Tompouce's outputs to the plugin chain's audio
   inputs, and the chain's outputs to the hardware sink. With PipeWire you can
   also patch from the shell with `pw-link`: list nodes with `pw-link -l` or
   `pw-dump`, then connect the `tompouce` playback ports to Carla's JACK input
   ports and Carla's outputs to the destination sink.
3. Start Tompouce normally. It renders as usual; Carla processes the signal
   downstream. MIDI CC from `.midi()`/`.midicmd()` patterns can drive Carla over
   JACK MIDI or the PipeWire MIDI bridge if the user routes those ports too.
   There are no pattern parameters for the plugin graph on this path.

This path does not touch Tompouce's source, playback or desktop routing. For
tests, use an owned silent sink and owned browser/profile state, verify the
route before applying nonzero gain, and restore the previous routing afterwards.

### What is and is not verified here

The development box has Carla 2.5.10, `pw-jack` and `pw-link`, and PipeWire is
running. Carla's headless mode requires a project file, and Tompouce's default
output is a PipeWire stream named `tompouce`. The existing
`test-audio-backends`, `profile-prep-backend` and `TOMPOUCE_OUTPUT_TARGET`
checks only cover Tompouce to an owned sink; they do not measure a Carla chain,
Wine bridges or device latency. A relative MIDI-to-audio latency through a
separate SF2 synth was measured on this box (see below). Absolute round-trip
latency under load, Wine bridges and device latency remain measurements to make
on a configured box: keep the graph quantum low and stable, enable realtime
scheduling (rtkit for native plugins, rtkit/wine-rt for Wine bridges) and avoid
an intermediate PulseAudio hop. Record those numbers before claiming
live-coding suitability; do not infer them from the native engine benchmarks or
from the relative figure.

The reproducible part was checked on the development box. A real Carla project
with the bundled LV2 `audiogain_s` plugin can be generated through Carla's own
Python API (`/usr/share/carla/carla_backend.py`: `CarlaHostDLL` on
`libcarla_standalone2.so`, `set_engine_option(ENGINE_OPTION_PROCESS_MODE,
ENGINE_PROCESS_MODE_PATCHBAY)` then `add_plugin(BINARY_NATIVE, PLUGIN_LV2,
/usr/lib/lv2/carla.lv2/carla.so, "gain", "<uri>", 0, None, 0)` and
`save_project(...)`). `pw-jack carla -n <project>` then exposes a PipeWire node
named `Carla` (and a `gain` node with `Audio Input 1/2`, `Audio Output 1/2` and
`events-in` ports). Testing the `out` control against it exposed a real bug:
WirePlumber ignores the declarative `target.object` property for a JACK-bridged
DSP node (no `media.class`), linking the named stream to the default sink
instead. Named streams now skip `PW_STREAM_FLAG_AUTOCONNECT` and create explicit
PipeWire links after enumerating the target's ports by node name, so
`.out('gain')` connects only to `gain:Audio Input 1/2`. The path was verified
with `pw-link -l` and by recording the plugin output with `pw-record` (peak
28006, RMS 73.26, non-silent). Realtime is configured (rtkit active, `realtime`
group, rtprio 98, memlock unlimited).

### Measured MIDI-to-audio latency

A separate measurement used the isolated resident daemon
(`--mcp-socket <path>`, see [the daemon](../README.md)), one headless Carla
instance hosting a FluidR3 GM SF2 synth (`sf2synth`), and one owned WirePlumber
capture. The resident pattern alternated a local sine onset and a `.midi()`
sine onset one second apart within a two-second cycle:

```
stack(note("c5 ~ ~ ~").s("sine").gain(.6),
      note("~ ~ c5 ~").s("sine").midi().gain(.9))
```

`Midi-Bridge:tompouce:out (capture)` was linked to `sf2synth:events-in`, and
both `tompouce:output_FL` and `sf2synth:out-left` were linked to the same
`pw-record` node, so both onset streams share one audio clock. The extra delay
of each synth onset after the preceding local onset was measured as the median
over paired onsets. Two clean runs gave **4.308 ms** (18 pairs, std 0.306 ms)
and **4.354 ms** (13 pairs, min 3.016 ms, max 4.354 ms, std 0.357 ms); expected
local-to-MIDI spacing was 1.000 s. Two later runs of the committed diagnostic
below reproduced 4.308 ms (12 pairs) and 4.354 ms (9 pairs). A control run with
the MIDI link removed showed only the local onsets at two-second spacing,
confirming the second onset came through the synth.

This is a relative figure: it bounds the added latency of the engine's MIDI
output, the PipeWire JACK bridge and the SF2 synth against the engine's own
audio path in the same capture. It is not an absolute round-trip or sound-card
latency measurement, and it was not taken under additional CPU load. External
ALSA MIDI injection from a second client is not permitted on this box, so the
onsets were generated by the engine itself. Do not quote it as device latency
or infer Wine bridge latency from it; measure those on the configured box.

The measurement is repeatable with `python3 scripts/midi-latency-qa.py`
(diagnostic, not a ctest gate). It needs Carla's Python backend, a SoundFont,
the PipeWire CLI tools, numpy and scipy, and a built `tompouce` (override with
`TOMPOUCE_TEST_BINARY` or `--binary`). It creates only owned state — a unique
temp dir, a null sink, one headless Carla instance, one daemon and one stereo
recorder with the engine and synth on separate channels — and cleans it up
without touching the user's session or routing.

## Supported classes and the UI ceiling

Today the supported plugin classes are exactly those the separate Carla process
supports: LADSPA, DSSI, LV2, VST2, VST3, SF2 and SFZ, plus whatever Wine bridges
the user has configured. Tompouce itself has no native plugin loader, no `lilv`
dependency and no VST3 SDK; it neither scans nor instantiates plugins, and it
never loads plugin code into the engine process. Do not claim native LV2/VST3
support from the Carla path.

There is no embedded plugin UI. The terminal UI cannot host `suil`/X11 editors,
and this project does not add a browser-based plugin window. In the zero-code
path, control happens in Carla's own window (or its project file), not in
Tompouce's F1–F6 panes or the companion. The planned native LV2 host would expose
generic parameter sliders only, mapped to the existing slider system; it would
still have no editor embedding. That is the ceiling by design, not a temporary
gap to be filled with X11 or browser UI code.

## Per-user setup

Plugins, their licenses, their installation and their host-side configuration
remain the user's responsibility. Tompouce does not install or redistribute
plugin binaries, Wine prefixes or VST SDKs, and its installer never touches them.
For Wine VSTs, configure yabridge in a per-user prefix and keep its routing and
realtime setup outside the engine. Realtime scheduling is likewise user-side:
use the distro's rtkit setup for native plugins and rtkit/wine-rt for Wine
bridges. These steps cannot be regression-tested on the development box; the
relative figure above is not a substitute for measuring Wine bridge, device and
loaded-system latency on your own machine.

## Named output targets (`out`)

`s('saw').out('carla')` attaches an exact output name to a pattern's voices.
The engine only opens streams for names in the bounded `TOMPOUCE_OUTPUT_NAMES`
allowlist (comma-separated, at most four, 64 characters each); candidates are
logged at startup. An unknown name emits one console warning and uses the
default stream, so a mistyped target never silences a pattern. Each allowlisted
name gets its own PipeWire stream with `PW_KEY_TARGET_OBJECT` set to that exact
node name. The default stream performs the only render pass; named streams
replay a snapshot of the dry voices assigned to them, bypassing shared orbit
reverb/delay/duck and master effects. The default bus keeps its original
operation order and stays byte-identical: a named-target voice contributes
nothing to it. Sources/capture remain out of scope until an input path exists.

## Planned, not implemented

- A managed Carla instance driven from patterns, with project files for plugin
  graphs and note/CC through the existing MIDI output.
- LV2 discovery through `lilv` is implemented (`src/lv2_host.c`, `test_lv2`):
  it reads only metadata from LV2_PATH (including the user bundle dir), keeps a
  bounded catalog of plugins with audio or event ports, and refuses binaries
  that are not plain `file://` `.so` paths without traversal or remote/download
  metadata. Instantiation now happens in a confined worker (`src/lv2_worker.c`):
  a forked child with a parent-death signal, no-new-privs, no core dumps, a 64-fd
  limit and a 2 GiB address-space limit loads the plugin, supplies bounded URID
  map/options/bounded-block features, and runs it on fixed 512-frame stereo
  blocks. The engine never dlopens plugin code. `test_lv2_worker` checks the
  limits, a gain control, finite PCM and dead/missing plugin failures. Wiring
  LV2 discovery now recognises atom MIDI ports (`atom:supports midi:MidiEvent`)
  in addition to legacy event ports. The worker carries up to 16 bounded 1–3
  byte note on/off/CC messages per 512-frame block with a frame offset
  (`lv2_worker_send_midi_at`) and writes them into an atom sequence in time
  order. `s('lv2:<uri>')` dispatches the event's note, its duration as
  note-off, and CC 74 from a resolved low-pass cutoff; effects without an atom
  input ignore the events. A self-contained `urn:tompouce:test:midi-synth`
  fixture (note frequency, velocity amplitude, CC gain) gates the worker and
  engine paths under the normal and sanitized worker; the default gain
  fixture's pinned PCM hash is unchanged. A live scheduler-owned plugin
  instance for real-time mid-note CC/parameter changes remains open.

  Engine audio plumbing is in place: `s('lv2:<uri>')` is prepared off the audio
  callback through the worker (bounded to four seconds), and the render becomes
  an owned playback sample via `smp_store_owned`. `test_lv2_worker` checks that
  a pattern-resolved event produces finite PCM and pins an FNV hash of the
  default gain plugin's 13,231-frame render at 44.1 kHz as the deterministic
  native reference (an env-selected plugin skips the hash). Web compare gates
  are inapplicable because strudel.cc has no LV2 host. Pinned native reference
  gates remain necessary: LV2 plugins have no strudel.cc counterpart, so the
  web comparison harness cannot supply a reference. VST3 is optional and strictly after LV2; VST2 is out.

See [TODO.md](../TODO.md) for the current state of those items and
[audio backends](../README.md#run) for Tompouce's output selection rules.
