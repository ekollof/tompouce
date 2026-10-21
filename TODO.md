# TODO

Active work only. Completed work and measurement details are preserved in the
[preparation/parity archive](docs/archive/preparation-parity-roadmap.md) and
[earlier history](docs/archive/todo-history.md). Current behavior and coverage
live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and
[architecture](docs/architecture.md). Follow [AGENTS.md](AGENTS.md) for security,
verification, installation and upstream terminal constraints.

## CPU optimization follow-up

Convolution selects AVX2, AVX or SSE2 accumulation at initialization according
to CPU/OS support, with portable C fallback and `TOMPOUCE_SIMD` overrides.
Per-sample voice paths skip provably-identity work through activity flags
cached in `dsp_init`, and never-used orbits skip their fallback reverb and
delay work. The [benchmark report](docs/simd-benchmark.md) records the
convolution speedups, simultaneous long-convolver scaling, idle/synth gains
and verification.

- [ ] Profile stretch preparation and decode latency for preparation-stage
      fast paths, after the callback work above. Preserve measured decode
      accuracy and the existing preparation budgets.

## Plugin hosting

LV2 instruments/effects first, headless (generic parameter sliders, no
`suil`/X11 editor embedding in the terminal UI). Run plugin DSP in a confined
worker process on the `csound_host` pattern: arbitrary plugin code never runs
in the engine process, preparation stays outside the audio callback, and
notices/attribution are preserved per plugin license.

In-between, use Carla instead of building format support: it already hosts
LADSPA/DSSI/LV2/VST2/VST3/SF2/SFZ plus Wine bridges (GPL-2.0-or-later, so no
license blocker), driven over JACK + MIDI CC + OSC with no engine changes.

- [ ] Document the zero-code path: Tompouce JACK out into a Carla patchbay
      hosting anything (including Wine VSTs), manual patching, no pattern
      params. Verify on a configured box and record latency notes.
- [ ] `out` control for named PipeWire targets (exact node-name match):
      `s('saw').out('carla')` routes those voices to a dedicated stream
      targeting that node; unknown names fall back to the default stream with
      a console warning (never silence). Lazily create streams per distinct
      name under a small cap; reuse the `TOMPOUCE_OUTPUT_TARGET` mechanism
      per stream. List candidate targets at startup/construction for
      discoverability; keep the default stream behavior byte-identical.
      Sources (capture) stay out of scope until an input path exists.
- [ ] Drive a managed Carla instance from patterns: project files for plugin
      graphs, notes/CC through existing MIDI output for params, instance
      lifecycle owned by the engine without stalling playback on failure.
- [ ] LV2 discovery/instantiation via `lilv` (installer dependency): scan
      standard paths plus a user dir, allowlist audio/MIDI plugin classes,
      refuse shell-y plugin metadata (executable paths, FDI downloads).
- [ ] Audio + note/CC plumbing through the worker model with bounded buffers;
      params/presets through LV2 state mapped to existing sliders; per-plugin
      crash isolation must not take down or stall playback.
- [ ] One instrument and one effect behind compare gates (rendered output vs
      a pinned reference render), plus failure tests (missing/broken plugin,
      dead worker, preset rejection) and sanitizer coverage of the host glue.
- [ ] Document supported classes, the no-UI ceiling, per-user plugin setup
      (licenses/installs stay with the user) and the rtkit/Wine notes below.

VST3 is optional and strictly after LV2: it needs the GPLv3 SDK vendored
under `thirdparty/` with notices intact plus a C++ bridge for the COM-style
API, on top of the same worker model. VST2 is out (deprecated, hostile
licensing). Wine-bridged plugins (yabridge) need a VST3 host first and stay
user-side (per-user prefixes, licenses and realtime setup such as
rtkit/wine-rt cannot be regression-tested here); measure round-trip
MIDI-to-audio latency under load on a configured box before claiming
live-coding suitability.

## Remaining parity work (optional)

Use independent web comparisons and explicit acceptance criteria for each gap.
Preserve thresholds, historical fixtures and execution budgets; completed gates
and measured divergences are recorded in the archive.

- [ ] Broaden Csound loader failure-side-effect coverage beyond URL caching,
      parked engines and console diagnostics.
- [ ] Extend MIDI output coverage for string CC names, MIDI maps, NRPN,
      `isController`/options-object defaults, duplicate scalar/array sends,
      note-off timestamps/durations and pinned numeric-string coercion drift.
- [ ] Evaluate input beyond native CC/note-on. Web per-device stores versus
      native merged stores and web non-CC store pollution are documented
      divergences; do not break merged semantics or reproduce pollution merely
      to turn a comparison green.

Scope limits: generated multi-room tails use different random streams; browser
wall-clock duck timers under contention have no exact native audio-clock
equivalent. Slider/expression changes retain event-level coverage where no
bare-REPL counterpart exists. Full-song waveform parity, complete decoder/synth
equivalence, remote-download deadlines, Hydra/browser input-device APIs and
arbitrary npm resolution remain non-goals unless explicitly re-scoped.
Non-PipeWire backend underruns remain unmeasured.

## Terminal blockers awaiting upstream

Large paste flood/drop and split-UTF-8 hangs await Notcurses
[#2948](https://github.com/dankamongmen/notcurses/issues/2948), related to
[#2548](https://github.com/dankamongmen/notcurses/issues/2548), and
[#2947](https://github.com/dankamongmen/notcurses/issues/2947).
Retain system Notcurses and its protocol ownership; do not add local fixes while
upstream fixes are pending. Keep `tests/test_paste_stress.py` reproductions
separate from maintained acceptance cases and report failures honestly.
