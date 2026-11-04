# TODO

Active work only. Completed work and measurement details are preserved in the
[preparation/parity archive](docs/archive/preparation-parity-roadmap.md) and
[earlier history](docs/archive/todo-history.md). Current behavior and coverage
live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and
[architecture](docs/architecture.md). Follow [AGENTS.md](AGENTS.md) for security,
verification, installation and upstream terminal constraints.

## Visual performance

- [ ] Add a separate `/hydra` command that opens a browser window or tab running
      a Strudel-compatible Hydra visualizer, connected to Tompouce's audio
      outputs so Hydra can react to the music. Keep it independent of F12,
      with authenticated output access, bounded audio/analysis streaming and
      explicit browser/session lifecycle and source-generation guards.

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

## Remaining REPL parity work

Use independent web comparisons and explicit acceptance criteria for each gap.
Preserve thresholds, historical fixtures and execution budgets; completed gates
and measured divergences are recorded in the archive.

- [ ] The Bakery corpus's nine initial event differences now match the saved
      2026-10-27 REPL events, including coastline (78185). Triangle fastcat
      controls are sampled in C; uniform saw, sine, square, and noise signals
      stay in JavaScript. IDs 13587 and 78179 still exceed the existing 200 ms
      query budget. Native fraction clones, span operations, hap transformations
      and fused triangle queries reduced isolated one-cycle `queryArc` profiles
      from roughly 375–425 ms to 241–279 ms (539 haps); these compile-stage probes
      do not establish a passing query-budget gate. Bounded pure-query and
      common-control-key fast paths now reduce paired five-run medians from
      287/282 ms to 268/268 ms, still 539 haps and still failing both 200 ms
      gates. Use `scripts/profile-pattern-query.mjs` for repeatable probes.
      A follow-up attribution pass (see
      [where the time goes](docs/audio-comparison.md#where-the-remaining-query-time-goes))
      shows the cost is per hap at ~0.47–0.49 ms with ~5 ms fixed, and that it
      is JavaScript call depth: in-place hap mutation leaves the time
      unchanged, `Object.create` copies are slower, fused `withValue` chains
      preserve values but add 7–14 ms, and native host crossings total about
      10 ms (17k fraction clones at 240 ns, 8.3k pure queries, 4.1k common-key
      scans at 520 ns). Each hap still crosses ~38 value transforms, 21 span
      transforms, 58 context copies and 18 context merges through the pinned
      combinator graph. Do not repeat allocation or single-operation
      micro-optimizations; the remaining options are a native applicator that
      batches the hot `Hap`/`Pattern` operations while invoking JavaScript
      callbacks in order, reducing combinator depth only where semantics allow,
      or the scoped V8 migration above. The Hydra example can now evaluate
      musical branches through inert visual placeholders; actual graphics
      remain covered by the separate `/hydra` task. See
      [Bakery corpus results](docs/audio-comparison.md#bakery-composition-corpus).

- [ ] Optional target: migrate from QuickJS to standalone embedded V8 through a
      confined C++ query worker, without a Node.js runtime dependency. Node-based
      probes remain diagnostics only. Keep QuickJS available during migration;
      replacing it is conditional on the integration and parity gates below.
      The [suitability assessment](docs/v8-assessment.md) finds 41 Bakery event
      matches and substantial scheduling headroom, but no production host/IPC
      integration is implemented. A transport spike measured worker
      spawn+bootstrap+compile at 72–107 ms and a warm 50 ms window round-trip at
      7.8 ms median (8.9 ms in-process), so a separate process fits the budgets
      as long as compilation stays off the audio path. No standalone V8 exists
      on this host (Node is static; no libnode/libv8 package), so the transport
      prototype now exists in `src/js_worker.c` + `scripts/query-worker.mjs`
      with parity and failure gates (`tests/test_js_worker.c`). The next
      milestone is engine delegation behind an explicit opt-in: port the JS
      host callbacks (prebakes, Csound, MIDI snapshots, live sliders), preserve
      generation/source identity and failed-evaluation retention, then gate
      native bindings, all-thread confinement, external-memory limits, worker
      OOM/death, cold/one-CPU behavior and real audio continuity. A standalone
      from-source V8 embedder build (depot_tools, gn/ninja, several GiB,
      ICU/snapshot/notices) remains the dependency-free alternative for later.
      Cover reproducible builds, packaging, dependency notices and deployment
      footprint. Preserve existing execution budgets and host behavior before
      considering QuickJS removal.

- [x] Extend Kabelsalat checks beyond the verified `audioin()` effect tails.
      Graph-source `fxr`, gain/shape/filter/pan and 128-frame onset/gate
      startup now have independent web comparisons. Nested `FX(...)` chains
      remain outside that coverage.
- [ ] Extend shared-effect checks beyond the 22 send/orbit/fixed-IR/lifetime
      cases: panned/stretched/graph source connections, source eviction, live
      reconnections, kernel-change automation, nested/control-output routing and
      live edits remain open. Very long modulation uses a bounded conservative
      connection deadline; distinguish these limits from established PCM parity.
- [ ] Assess remaining visualization options and shared-effect tail attribution:
      browser styling/layout options, per-pattern shared delay/reverb/duck tails
      and isolated Csound taps. Document intentional terminal adaptations rather
      than claiming browser-pixel parity.
- [ ] Finish Switch Angel integration checks beyond the 81 setup/helper/API
      cases and selected audio gates: live oneshot evaluation/grid timing,
      bank-specific sounds, bmod/FX routing and external devices. See
      [prebake support](docs/prebakes.md) for the supported startup/file contract.
- [ ] Extend MIDI maps beyond inline per-program definitions: confined remote
      manifests and explicit persistence across evaluations need design and
      independent reference checks. Keep current 32-map/128-entry/32-send bounds.
- [x] Add web-compatible string-form MIDI sysex with bounded parsing and
      rejection tests, preserving current byte-array framing and send ordering.
      `sysexid("0x77")` and `sysexdata("0x01:0x02")` accept hex (0x) or decimal
      tokens separated by `:`, for both scalar and `midicmd` array forms. The
      deployed REPL throws on its own documented strings, so this is a recorded
      superset; malformed tokens, >255 values and over-long data are rejected
      while the note still commits.
- [ ] Measure physical-device MIDI note-off delivery timing under load. Channel
      arrays now have independent web gates and an owned ALSA loopback check;
      this does not measure USB/DIN transport or device response. The maintained
      gate covers named CCs, inline maps, NRPN,
      controller/options defaults, duplicate scalar/command sends and exact
      note-on/off timestamp differences. Pinned numeric-string coercion drift
      remains explicit; do not replace the runtime or historical fixtures.

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
