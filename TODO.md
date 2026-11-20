# TODO

Active work only. Completed work and measurement details are preserved in the
[preparation/parity archive](docs/archive/preparation-parity-roadmap.md) and
[earlier history](docs/archive/todo-history.md). Current behavior and coverage
live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and
[architecture](docs/architecture.md). Follow [AGENTS.md](AGENTS.md) for security,
verification, installation and upstream terminal constraints.

## Visual performance

- [ ] `/hydra` command: a separate loopback visualizer is now implemented and
      independent of F12 — `/hydra` / `/hydra stop` spawn a browser tab through
      the companion helper with a private token, Host/Origin checks, one
      read-only `music_audio_scope` MCP tool (bounded 16–192 samples, polled at
      8 Hz) and a parent-pid session guard. `tests/test_companion.py` covers the
      tool allowlist and auth. A bounded Hydra-style function layer
      (`web/hydra.js`: `osc`/`shape`/`solid`/`noise`/`src` with `rotate`,
      `scale`, `repeatX/Y`, `color`, `out`) is served and checked. Still open:
      full WebGL Hydra compatibility. The scope tool now includes the accepted
      source's `generation`, and the page drops stale frames when it changes.
      Full WebGL compatibility remains open.

## Plugin hosting

LV2 instruments/effects first, headless (generic parameter sliders, no
`suil`/X11 editor embedding in the terminal UI). Run plugin DSP in a confined
worker process on the `csound_host` pattern: arbitrary plugin code never runs
in the engine process, preparation stays outside the audio callback, and
notices/attribution are preserved per plugin license.

In-between, use Carla instead of building format support: it already hosts
LADSPA/DSSI/LV2/VST2/VST3/SF2/SFZ plus Wine bridges (GPL-2.0-or-later, so no
license blocker), driven over JACK + MIDI CC + OSC with no engine changes.

- [x] Document the zero-code path: Tompouce's default PipeWire out into a Carla
      patchbay (via `pw-jack` or native JACK) hosting anything, including Wine
      VSTs, manual patching with `pw-link`, no pattern params.
      See [external plugin hosting](docs/plugin-hosting.md).
- [x] Verify the zero-code path on a configured box and record latency notes:
      a real Carla project with the bundled LV2 gain plugin was generated via
      Carla's Python API and run headless. A bug in the `out` control was found
      and fixed: WirePlumber ignored `target.object` for the JACK-bridged DSP
      node, so named streams linked to the default sink; explicit PipeWire
      links now connect `tompouce-named` to the target's audio input ports.
      Verified with `pw-link -l` (only Carla input links) and `pw-record` from
      the plugin output (peak 28006, RMS 73.26, non-silent). Realtime is
      configured (rtkit active, `realtime` group, rtprio 98, memlock unlimited).
      A relative MIDI-to-audio onset latency was later measured through a Carla
      SF2 synth with the isolated daemon and one owned PipeWire capture: median
      4.308 ms (18 pairs) and 4.354 ms (13 pairs) in two clean runs. This bounds
      the added delay of MIDI output, the JACK bridge and the synth against the
      local audio path; it is not absolute device latency. Repeatable with
      `scripts/midi-latency-qa.py` (diagnostic, not a ctest gate). See
      [measured latency](docs/plugin-hosting.md#measured-midi-to-audio-latency).
- [x] `out` control for named PipeWire targets (exact node-name match):
      `s('saw').out('carla')` routes voices to a per-name stream (up to four,
      from the `TOMPOUCE_OUTPUT_NAMES` allowlist) with one shared render pass;
      unknown names warn once and use the default stream. Candidates are
      logged at startup and the default bus stays byte-identical. Named streams
      carry dry voices only, bypassing shared effects. See
      [named output targets](docs/plugin-hosting.md#named-output-targets-out).
      Sources (capture) stay out of scope until an input path exists.
- [ ] Drive a managed Carla instance from patterns: project files for plugin
      graphs, notes/CC through existing MIDI output for params, instance
      lifecycle owned by the engine without stalling playback on failure.
- [x] LV2 discovery via `lilv` (optional build dependency, auto-detected): scan
      LV2_PATH including the user dir, allowlist audio/MIDI port classes and
      refuse non-`file://` binaries, traversal and remote/executable metadata.
      `test_lv2` covers the bounded catalog (210 audio plugins on this box).
- [x] LV2 instantiation in a confined worker (`src/lv2_worker.c`): plugin code
      runs in a forked child with fixed limits and a minimal feature set; the
      engine only sends bounded stereo PCM blocks and control values. Tests
      cover confinement, gain control, PCM and dead/missing plugin failures.
- [x] LV2 audio plumbing into the engine: `s('lv2:<uri>')` is prepared off the
      audio callback through the confined worker (bounded 4 s render) and stored
      as an owned playback sample; `test_lv2_worker` covers the engine path.
      Worker crash/missing-plugin failures are covered. `test_lv2_worker` also
      checks a pinned native PCM hash (FNV over the 13,231-frame gain render at
      44.1 kHz), the deterministic reference gate for the default plugin; an
      env-selected plugin skips the hash. MIDI note/CC plumbing into a live
      plugin remains open; bounded note on/off and CC message plumbing through
      the worker's atom MIDI input is implemented and tested with
      `midichanfilter`. Web compare gates are inapplicable because LV2 plugins
      have no strudel.cc counterpart.
- [ ] Audio + note/CC plumbing through the worker model with bounded buffers;
      params/presets through LV2 state mapped to existing sliders; per-plugin
      crash isolation must not take down or stall playback.
- [ ] One instrument and one effect behind compare gates (rendered output vs
      a pinned reference render), plus failure tests (missing/broken plugin,
      dead worker, preset rejection) and sanitizer coverage of the host glue.
- [x] Document supported classes, the no-UI ceiling, per-user plugin setup
      (licenses/installs stay with the user) and the rtkit/Wine notes.
      See [external plugin hosting](docs/plugin-hosting.md#supported-classes-and-the-ui-ceiling).

VST3 is optional and strictly after LV2: it needs the GPLv3 SDK vendored
under `thirdparty/` with notices intact plus a C++ bridge for the COM-style
API, on top of the same worker model. VST2 is out (deprecated, hostile
licensing). Wine-bridged plugins (yabridge) need a VST3 host first and stay
user-side (per-user prefixes, licenses and realtime setup such as
rtkit/wine-rt cannot be regression-tested here); measure round-trip
MIDI-to-audio latency under load on a configured box before claiming
live-coding suitability.

## Remaining REPL parity work

Use independent web comparisons and explicit acceptance criteria for each gap.
Preserve thresholds, historical fixtures and execution budgets; completed gates
and measured divergences are recorded in the archive.

- [x] Close the Bakery query-budget gap with an opt-in worker: all nine initial
      event differences match the saved 2026-10-27 REPL events, including
      coastline (78185), and the full corpus now matches all 42 compositions.
      The two songs that exceed 200 ms on QuickJS (IDs 13587 and 78179) run at
      139/140 ms compile-stage medians with the opt-in native V8 worker (Node:
      128/129 ms), with 398/398 events each in the full comparison. QuickJS
      remains the default and still fails those two gates; the gap is closed by
      `TOMPOUCE_QUERY_BACKEND=native`, not by changing the budget or a baseline.
      Native fraction clones, span/hap fast paths and C-sampled triangle
      fastcat controls remain for the default backend; the attribution pass
      (see [where the time goes](docs/audio-comparison.md#where-the-remaining-query-time-goes))
      shows the residual is per-hap JavaScript call depth (~0.47–0.49 ms per
      hap, ~38 value transforms, 21 span transforms, 58 context copies and 18
      context merges), with native host crossings around 10 ms total. Do not
      repeat allocation or single-operation micro-optimizations there. Use
      `scripts/profile-pattern-query.mjs` or the non-gating
      `cmake --build build --target profile-bakery-query` for repeatable probes.
      The Hydra example can evaluate musical branches through inert visual
      placeholders; actual graphics stay with the separate `/hydra` item. See
      [Bakery corpus results](docs/audio-comparison.md#bakery-composition-corpus)
      and the [native V8 prototype](docs/v8-assessment.md#native-embedded-v8-worker-prototype-2026-11-20).

- [x] Optional target: migrate from QuickJS to standalone embedded V8 through a
      confined C++ query worker, without a Node.js runtime dependency. Node-based
      probes remain diagnostics only. Keep QuickJS available during migration;
      replacing it is conditional on the integration and parity gates below.
      The [suitability assessment](docs/v8-assessment.md) finds 41 Bakery event
      matches and substantial scheduling headroom, but no production host/IPC
      integration is implemented. A transport spike measured worker
      spawn+bootstrap+compile at 72–107 ms and a warm 50 ms window round-trip at
      7.8 ms median (8.9 ms in-process), so a separate process fits the budgets
      as long as compilation stays off the audio path. The transport prototype
      exists in `src/js_worker.c` + `scripts/query-worker.mjs` with parity and
      failure gates (`tests/test_js_worker.c`); a standalone V8 embedder is now
      built by the AUR recipe in `packaging/aur/libv8/` (see below).
      `TOMPOUCE_QUERY_BACKEND=node|v8` delegates queries for plain musical
      programs; visuals, prebakes, Csound, live sliders and `midin` readers
      stay on QuickJS, one shared worker follows the newest compatible source,
      and a failed worker falls back to the retained QuickJS evaluation. Each
      evaluation carries a compile serial so re-evaluating identical source
      starts fresh worker state.
      The confined startup prebake is now forwarded in the compile frame and
      tested, and the worker now rebuilds
      its VM context per compile to match QuickJS's fresh-evaluation semantics
      (a persistent-context `preCount` drift was found and fixed). Named
      `loadPrebake()` calls are now forwarded as a confined name→text table in
      the compile frame and are tested; the worker's prebake load window closes
      after evaluation like the host's. Live sliders are also forwarded (bind
      indices plus current values at compile, values on each query) and tested
      with a mid-session slider_set; slider programs now delegate. `midin` CC
      snapshots are forwarded per query (all nonzero CCs, bounded) and tested;
      `midikeys` still stays on QuickJS because it consumes a host event queue.
      Csound loads are now accepted by a bounded worker callback while the
      engine applies the real orchestra, and are tested for event parity.
      Visual programs now delegate too (scopeMask from the shared bundle,
      visual sites still configured in QuickJS) and are tested. All formerly
      excluded callback categories now have worker paths except `midikeys`.
      OOM behavior is covered (the worker reports a
      range error for an oversized allocation and stays alive); one-CPU
      `profile-prep-backend` held 3/4 with and without the worker, with two
      2.7 ms late burst reservations and no xruns — identical in both modes, so
      the worker is not the cause. Failed-evaluation retention is cross-checked:
      a rejected live evaluation keeps the last playable program delegating
      with identical events. Packaging is documented
      (the installer ships only the worker script, the engine sends its bundle
      over IPC, and Node's optional runtime notices are in NOTICE/README). The
      worker is confined (own group, parent-death signal, no-new-privs, fixed
      `RLIMIT_CORE`/`NOFILE`/`CPU`/`AS`, sanitized environment) and its limits
      are tested; `profile-prep-backend` with the backend enabled held all six
      real-backend gates in two consecutive runs (zero xruns, drops or
      overruns), with one earlier run showing a single 2.7 ms late reservation
      worth watching. A standalone from-source V8
      embedder build is now demonstrated with pinned V8 16.0.94 and an AUR
      `libv8` recipe: the opt-in native C++ worker (`-DWITH_V8=ON`,
      `TOMPOUCE_QUERY_BACKEND=native`) matched 42/42 Bakery compositions,
      including 398/398 events on both heavy songs, at 139/140 ms compile-stage
      medians (Node: 128/129 ms). QuickJS stays the default and the acceptance
      gates in [the assessment](docs/v8-assessment.md#native-embedded-v8-worker-prototype-2026-11-20)
      remain open. The default migration is now implemented: with no backend
      env the native worker is discovered beside the executable and used, with
      QuickJS as fallback and `TOMPOUCE_QUERY_BACKEND=quickjs` as explicit
      opt-out; `install.sh` builds and installs the worker when a `v8`
      pkg-config package is present. Hardening gates stay documented in the
      assessment. Preserve existing execution budgets and host behavior.

- [x] Extend Kabelsalat checks beyond the verified `audioin()` effect tails.
      Graph-source `fxr`, gain/shape/filter/pan and 128-frame onset/gate
      startup now have independent web comparisons. Nested `FX(...)` chains
      remain outside that coverage.
- [ ] Extend shared-effect checks beyond the 22 send/orbit/fixed-IR/lifetime
      cases: two new `shared-room-source-` gates cover a panned source
      (`.pan(.7)`, correlation 0.99999998) and a downward-stretched source
      (`.speed(.5)`, 0.999705) feeding the mono IR. Two `shared-room-kernel-`
      gates cover kernel-change automation (`.roomsize` and `.size` patterns),
      both at correlation 0.99999999. Graph source connections, source eviction,
      live reconnections, nested/control-output routing and live edits remain
      open. Fractional IR
      rates are a measured divergence: `.irspeed(.5|.75)` with matching source
      speed gave correlations -0.78/-0.92; an interpolation attempt was
      reverted rather than tuned to the capture. See
      [shared sends](docs/audio-comparison.md#shared-sends-and-mono-ir-tail-width).
      Very long modulation uses a bounded conservative connection deadline;
      distinguish these limits from established PCM parity.
- [x] Assess remaining visualization options and shared-effect tail attribution:
      documented in [visual feedback](docs/javascript.md#live-coding-and-visual-feedback)
      — browser-only styling is a deliberate ceiling, shared orbit tails and
      the combined Csound stream are intentionally not attributed per pattern,
      and terminal geometry/raster is checked against the web renderer without
      claiming browser-pixel parity.
- [ ] Finish Switch Angel integration checks beyond the 81 setup/helper/API
      cases and selected audio gates: `test_oneshot_grid` now covers the pinned
      helper's grid gating with a local confined fixture (eight first-cycle
      haps over a four-cycle query); `test_switchangel_fx` also checks a
      pinned-style FX helper attaches its stage to the query event, and
      `test_switchangel_humanize` checks its deterministic onset/velocity bounds,
      and `test_switchangel_bmod` checks a pinned-style helper attaches a bus
      modulation entry to the query event. Bank-specific sounds and external
      devices remain open. See
      [prebake support](docs/prebakes.md) for the supported startup/file contract.
- [x] Extend MIDI maps beyond inline per-program definitions: `await midimaps('name')`
      loads a bounded, confined JSON manifest from the user's config store and
      can be reloaded on later evaluations (explicit persistence). Traversal,
      oversized files and malformed JSON are rejected; 32-map/128-entry/32-send
      bounds are unchanged. `tests/test_javascript.c` checks mapped CCs,
      reload and rejection. Documented in [JavaScript/native audio](docs/javascript.md).
- [x] Add web-compatible string-form MIDI sysex with bounded parsing and
      rejection tests, preserving current byte-array framing and send ordering.
      `sysexid("0x77")` and `sysexdata("0x01:0x02")` accept hex (0x) or decimal
      tokens separated by `:`, for both scalar and `midicmd` array forms. The
      deployed REPL throws on its own documented strings, so this is a recorded
      superset; malformed tokens, >255 values and over-long data are rejected
      while the note still commits.
- [ ] Measure physical-device MIDI note-off delivery timing under load. Channel
      arrays have independent web gates; `test_note_off_under_load` now adds two
      busy CPU threads to the owned ALSA loopback check and still requires every
      20 ms note-off before 30 ms of audio time. USB/DIN transport and device
      response remain unmeasured. The maintained
      gate covers named CCs, inline maps, NRPN,
      controller/options defaults, duplicate scalar/command sends and exact
      note-on/off timestamp differences. Pinned numeric-string coercion drift
      remains explicit; do not replace the runtime or historical fixtures.
      An isolated resident daemon is available for testing through
      `--mcp-socket <path>` (0600 named Unix socket, no TCP port) with
      `scripts/tompoucectl.py` and an optional disabled-by-default systemd user
      unit from `scripts/install-daemon.py`; `test_daemon` covers auth, isolation
      and SIGTERM socket cleanup. The daemon was used to measure relative
      MIDI-to-audio onset latency through a separate SF2 synth (see
      [measured latency](docs/plugin-hosting.md#measured-midi-to-audio-latency));
      an absolute device-latency figure still needs external MIDI injection
      permissions or an in-engine capture hook.

Input beyond CC/note-on has been evaluated against the deployed stack:
note-off, bend, touch and program messages do not become native CC entries.
The web's per-device stores and non-CC store pollution remain documented
divergences. Preserve native merged semantics and clean CC stores.

Scope limits: generated multi-room tails use different random streams; browser
wall-clock duck timers under contention have no exact native audio-clock
equivalent. Slider/expression changes retain event-level coverage where no
bare-REPL counterpart exists. Full-song waveform parity, complete decoder/synth
equivalence, remote-download deadlines, Hydra/browser input-device APIs and
arbitrary npm resolution remain non-goals unless explicitly re-scoped.
Kabelsalat device-input/mouse nodes and custom JavaScript UGens also require
explicit scope and confinement design before implementation. The separate
Hydra task above covers visualizer integration, not general browser input APIs.
Non-PipeWire backend underruns remain unmeasured.

## Terminal blockers awaiting upstream

Large paste flood/drop and split-UTF-8 hangs await Notcurses
[#2948](https://github.com/dankamongmen/notcurses/issues/2948), related to
[#2548](https://github.com/dankamongmen/notcurses/issues/2548), and
[#2947](https://github.com/dankamongmen/notcurses/issues/2947).
Retain system Notcurses and its protocol ownership; do not add local fixes while
upstream fixes are pending. Keep `tests/test_paste_stress.py` reproductions
separate from maintained acceptance cases and report failures honestly.

- [x] Low priority: verified the split-UTF-8 fix for
      [#2947](https://github.com/dankamongmen/notcurses/issues/2947) (PR #2954,
      commit `2cb71b8`, milestone 3.1.0) in an owned build: the upstream
      `input-utf8-split` ctest passes and the 4096 × U+65E5 pipe finishes
      (exit 0), while the same input against the parent commit hangs (timeout
      124). The system Notcurses and its protocol ownership are untouched, and
      the `tests/test_paste_stress.py` reproductions stay separate from
      maintained acceptance cases. The confirmation was posted to the issue by
      the reporter ([comment](https://github.com/dankamongmen/notcurses/issues/2947#issuecomment-7023660207)).
      The paste flood/drop issue [#2948](https://github.com/dankamongmen/notcurses/issues/2948)
      remains open and must be tracked separately.
