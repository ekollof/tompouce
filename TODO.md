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
      tool allowlist and auth. `web/hydra.js` now renders through WebGL: GLSL
      generators (`osc`, `shape`, `solid`, `noise`, `voronoi`, `gradient`,
      `src`) and effects (`rotate`, `scale`, `repeat`, `repeatX/Y`, `kaleid`,
      `pixelate`, `posterize`, `color`, `colorama`, `hue`, `saturate`,
      `brightness`, `contrast`, `invert`, `luma`, `modulate`, `blend`, `add`,
      `sub`, `mult`, `diff`, `layer`) chain through a bounded pool of
      ping-pong framebuffers. `src` reads the audio scope texture. The page
      switches from its 2D fallback to the WebGL stage only while a custom
      graph is set. `hydra_webgl` is a headless Chromium smoke test for
      generator output, effect chains, binary effects, the scope source and the
      real page's WebGL switch. The scope tool includes the accepted source's
      `generation`, and the page drops stale frames when it changes. Remaining:
      upstream hydra-synth API beyond this bounded layer (camera/screen
      sources, feedback and the full function set).

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
- [x] Drive a managed Carla instance from patterns. `Pattern.carla(project,
      target)` resolves a bounded `.carxp` under `$TOMPOUCE_CARLA_ROOT` /
      `$XDG_CONFIG_HOME/tompouce/carla`, validates it without loading plugin
      code in the engine, and starts one owned Carla process per project and
      target. A background thread waits for the project node, links the existing
      ALSA MIDI output exposed as `Midi-Bridge:<client>:out (capture)` to
      `<target>:events-in`, and never stalls the audio callback. `.midi()`
      patterns then send notes and controllers through the normal MIDI path.
      Instances stop on transport stop and engine shutdown. `test_carla_host`
      covers bounds, reuse and fake-process lifecycle; `test_managed_carla.py`
      generates a real SF2 project, verifies the ready/link state, captures
      non-silent output (peak 6614, RMS 740) and confirms `carla=1/1` becomes
      `carla=0/0` with no node left after stop.
- [x] Browse and select LV2 plugins in the F6 sound browser. A bounded
      `lv2 plugins` category uses the metadata-only catalog, shows declared
      control counts plus `[instrument]`/`[effect]` markers, and inserts a
      quoted `s("lv2:<uri>")` snippet (with `note("c4").` for instruments)
      without loading plugin code in the engine. The catalog is scanned lazily
      and cached. `test_lv2` checks catalog rows and snippets; the PTY sound
      browser test selects the MIDI fixture and verifies the inserted code.
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
      env-selected plugin skips the hash. Web compare gates are inapplicable
      because LV2 plugins have no strudel.cc counterpart.
- [x] Timed LV2 MIDI dispatch in the worker model. Up to 16 bounded 1-3 byte
      note on/off/CC events are carried per 512-frame block with a frame offset
      (offsets beyond the processed block are clamped to its last frame) and
      inserted in time order into the plugin's atom sequence. Note/CC events
      are staged with `lv2_worker_send_midi_at`; the existing offset-0 helper
      remains. `s('lv2:<uri>')` now sends a note-on at the note onset, a note-off
      at the event duration, and CC 74 derived from a resolved low-pass cutoff;
      effects without an atom input ignore them, so the default gain fixture's
      pinned hash is unchanged. A self-contained
      `urn:tompouce:test:midi-synth` fixture renders a deterministic saw whose
      frequency follows the MIDI note, amplitude follows velocity, and CC
      scales the output, so the worker and engine paths are tested without
      external plugins. `test_lv2_worker` covers sample-accurate offsets,
      note-off, CC, event-queue bounds, engine note/cutoff dispatch and the
      sanitized worker (`TOMPOUCE_LV2_WORKER_SANITIZE=1`).
- [x] Scheduler-owned live LV2 instruments (`src/live_lv2.c`). `s('lv2:<uri>')`
      starts one confined worker and bounded render thread per instrument URI
      (at most eight, never reused) when the plugin reports atom MIDI input and
      no audio input. Note-on, CC 74 from the resolved low-pass cutoff, and
      note-off at the event duration are queued at absolute audio times; the
      render thread sends them and fills a bounded stereo sample ring. The
      audio callback only reads the ring into the master bus, with no IPC or
      allocation. The offline owned-sample path remains for effects and plugins
      without MIDI. `test_live_lv2` covers the ring, note/CC timing, silence
      after note-off, the bounded event queue, invalid arguments and shutdown;
      the sanitized worker passes with `TOMPOUCE_LV2_WORKER_SANITIZE=1`. A
      headless engine run measured `voices=0` and peak 0.504 for the fixture,
      and cutoff 20 (CC 74=0) silenced it.
- [x] Mid-note LV2 control-port changes. The worker captures bounded input
      control metadata (port index, display name, declared min/max/default) at
      startup; `live_lv2_control` resolves names case-insensitively, clamps to
      the declared range and queues timed updates for the render thread, which
      applies them with `lv2_worker_set_control` before the next block.
      `Pattern.lv2param(name, value)` attaches the value to the event (a
      slider or any pattern works); a pattern with no note becomes a
      control-only event, so a separate long note keeps sounding while the
      parameter changes. `test_live_lv2` covers the metadata, a mid-note Gain
      change and the bounded control queue under the sanitized worker;
      `test_lv2_worker` checks the worker metadata. A headless engine run
      measured a long note's scope RMS 0.291 and peak 0.502, and with
      `.lv2param('Gain', .25)` RMS 0.071 and peak 0.126, with `voices=0` and no
      gaps.
- [x] Installed LV2 presets that declare control-port values. The worker
      resolves `pset:Preset` resources linked from the bundle manifest, loads a
      preset by URI or display label, applies declared `pset:hasPort` values and
      reports updated control metadata back to the slot. `Pattern.lv2preset()`
      emits a control-only event when it has no note, so a held note keeps
      sounding. `test_lv2_worker` checks label and URI loading plus a sound-level
      change; `test_live_lv2` changes a preset mid-note under the sanitized
      worker. A headless engine run measured RMS 0.294 for a long note and 0.074
      with `.lv2preset('Quiet')`, with `voices=0`.
- [x] Arbitrary LV2 state-file restore with control mapping. `Pattern.lv2state`
      resolves a bounded relative path under `$XDG_CONFIG_HOME/tompouce/lv2-state`
      (or `$TOMPOUCE_LV2_STATE`), rejects traversal and symlinks escaping that
      root, and limits regular files to 256 KiB. The confined worker parses the
      file with `lilv_state_new_from_file`, verifies `lv2:appliesTo`, restores
      plugin `state:state` properties through `lilv_state_restore`, and maps
      declared `lv2:port` values to the cached control metadata and sliders.
      A property-only fixture state changes the sound without any port value; a
      combined state also updates Gain. `test_lv2_worker` covers properties,
      port mapping, wrong-plugin, traversal and missing-file rejection;
      `test_live_lv2` applies states mid-note under the sanitized worker. A
      headless engine run measured scope RMS 0.035 for the combined state and
      0.074 for the property-only state, with `voices=0` and no gaps. Live slots
      persist for the session (bounded at eight), receive all-notes-off when
      playback stops, and rely on the render thread for best-effort latency;
      per-plugin crash isolation does not take down playback. Discovery reads
      control-port metadata for all safe plugins — 3155 ports across 219 plugins
      on this box.
- [x] One instrument and one effect behind pinned reference gates, plus
      failure tests (missing/broken plugin, dead worker, preset rejection) and
      sanitizer coverage of the host glue. The instrument side has a pinned
      offline PCM hash for the LSP oscillator (13231 frames, deterministic
      across runs), the gain effect has its engine-path hash, and control-port
      rejection (invalid index, non-finite value) is tested alongside the
      missing-plugin and dead-worker cases. The self-contained
      `tests/fixtures/lv2-test` gain fixture now pins its own engine-path hash
      and runs under ASan/UBSan (`test-sanitize`), including worker plugin
      instantiation and PCM processing. External MIDI/instrument fixtures skip
      when those plugins are unavailable; the owned gain fixture does not.
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
      startup now have independent web comparisons. Nested `FX(...)` chains now
      have two gated cases (`fx-nested-`, correlation 0.99999996, RMS ratio
      1.0003): query-time materialization exposes bounded step descriptors and
      the native renderer applies the web chain's default 0.8 gain. Four `fx-`
      gates cover the graph/effect boundary: the web does not apply that chain
      gain to a `K()` graph source, so the native renderer skips it there
      (`fx-graph-source-lpf` 0.99999997/1.0003 and `fx-graph-out-channels`
      0.99999994/1.0004) while keeping it for `audioin()` worklet effects
      (`fx-worklet-effect-gain` and `fx-worklet-effect-tail`, both
      0.99999994/1.0004). Individual filter/send/pan steps are materialized but
      not replayed yet. See
      [Kabelsalat effect tails](docs/audio-comparison.md#kabelsalat-effect-tails-and-input-routing).
- [ ] Extend shared-effect checks beyond the 24 send/orbit/fixed-IR/lifetime
      cases: two `shared-room-source-` gates cover a panned source
      (`.pan(.7)`, correlation 0.99999998) and a downward-stretched source
      (`.speed(.5)`, 0.999705) feeding the mono IR. Two `shared-room-kernel-`
      gates cover kernel-change automation (`.roomsize` and `.size` patterns),
      both at correlation 0.99999999. Two `shared-*-graph-source` gates connect
      a Kabelsalat graph source to the shared mono IR (0.999999998) and the
      shared delay (0.9999990). Source eviction, live reconnections, general
      nested/control-output routing beyond the graph/effect FX boundary and
      live edits remain open. Fractional IR
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
OpenAL is opt-in (`TOMPOUCE_AUDIO_BACKEND=OpenAL`) and ships an app-specific
`alsoft.conf` with 64-frame stereo periods; our queue defaults to 256×2 (about
11.6 ms). The render profiler shows "Computer says no" peaking at 128 voices. After the
route-cache, two-lane SSE2, supersaw AVX2 and disabled-DSP changes, average
callback render is about 1.4–1.6 ms with occasional peaks around 3.0–3.3 ms
against the 2.9 ms PipeWire deadline, so rare xruns remain. 128-frame OpenAL
queues stopped/restarted the source before those changes; 256×2, 256×3 and
512×2 measured zero stops. The remaining low-latency path is the block/SoA
voice renderer below. Other non-PipeWire backend underruns remain unmeasured.

## Render and SIMD performance

- [ ] Full from-allocation SoA renderer. Target CPU-heavy songs such as
      "Computer says no" staying inside the 128-frame PipeWire deadline without
      xruns. Move voice source, envelope, filter, gate and route state into SoA
      arrays at allocation and retire them from those arrays; do not copy AoS
      voices into lanes at a block boundary.
      - [x] Four-frame routing/send kernel (`src/render_block.c`) with exact C
        and AVX2 paths and `test_render_block`.
      - [x] PCM-exact block callback split/staged route. Routing-only staging is
        neutral, so it is infrastructure rather than a speedup.
      - [x] Close the incremental AoS-grouping trials: four-frame lookahead
        (max ~4.4 ms, two gaps), four-voice AoS gather/scatter (max ~3.55 ms,
        13 over-3-ms callbacks), and block-boundary native SoA copy (max
        ~4.18 ms, one gap). All were PCM-exact and slower.
      - [x] Persistent SoA sample pool admitted at `voice_onset`, with lane
        state kept across staged blocks and owner sync only after fallback
        blocks. It passed the audio/retirement gates but measured slower again
        (max ~4.19 ms, one gap), so it was reverted. Sample-only SoA is
        exhausted; the next attempt must include the DSP low-pass/biquad and
        envelope work in the same lanes.
      - [x] Four-lane low-pass/biquad SoA primitive (`src/voice_soa_dsp.c`)
        with exact C/AVX2 paths, non-finite reset and `test_voice_soa_dsp`.
        Microbenchmark: ~0.42 ms AVX2 vs ~0.84 ms C for 100k frames over four
        lanes (about 2x).
      - [x] Integrate the biquad with source, ADSR, gate and route lanes in a
        four-voice block path. PCM gates passed, but "Computer says no"
        measured max ~4.02 ms with one gap (over-3 ms: 10), slower than the
        scalar path, so the integration was reverted. The block-boundary copy
        and four-voice grouping overhead still dominate.
      - [x] Build persistent LPF DSP lanes from onset with LPE/LFO coefficient
        updates and no per-block state copy. PCM gates passed, but the
        35-second "Computer says no" run measured 41 gaps and max ~3.33 ms, so
        it was reverted. Lane grouping/synchronization and per-frame filter
        updates still cost more than the scalar path.
      - [x] From-allocation SoA voice bank (`src/soa_voice.c`): sample and
        sine/saw/square/triangle sources, position/step, ADSR, gate/release,
        oscillator phase, low-pass with LPE/LFO coefficient updates, identity
        filtering for DSP-bypassed voices, and per-lane output state.
        `test_soa_voice` matches the scalar paths exactly, including four-lane
        group isolation and lane removal.
      - [x] Wire the bank into allocation and route order. Lanes are admitted
        at the next four-frame block boundary, so no state is copied per block;
        the scalar loop routes a lane at its own slot position (or stages it
        through `render_block`), preserving mixing order and pattern-scope taps.
        Retirement copies current lane state before eviction.
      - [x] "Computer says no" on PipeWire 128-frame quanta. The bank covers
        low-, high- and band-pass voices plus the no-filter clamp path, and is
        sized to the whole primary pool (128 lanes) so no qualifying voice is
        denied at peaks. Roughly **86 lanes per block** now cover the
        source/envelope/filter/routing hot path; no primary triangle or sample
        voice remains scalar. Repeated 35-second runs measure average render
        **~1.19-1.20 ms**, maximum **2.30-2.86 ms**, zero callbacks over 3 ms
        and zero gaps. One earlier 53 ms outlier occurred while lock waits
        stayed at 8-22 us, so it was an external stall rather than callback
        work.
      - [ ] The remaining scalar population is supersaw (~8.8 voices/block)
        and the procedural HH drum (~1.3). Supersaw's 100-oscillator kernel is
        already AVX2 and would need ~1.6 KB of phase/ratio state per lane, so
        lane admission would not reduce its main cost; the next useful step is
        the compact active-voice list, not more per-sample effects. Vowel,
        phaser, chorus and distortion are not used by this composition and
        still render scalar; add lanes for them only with a matching browser
        gate and a measured song that needs them.
- [ ] Later SIMD kernels, only after measurement:
  - per-voice stereo DSP/filters and FM oscillators; IIR recurrences need the
    block/SoA layout rather than frame lanes
  - four-frame routing/mix and sample interpolation; the current two-lane SSE2
    route and paired source interpolation were only incremental or neutral
  - stereo delay and independent reverb combs; feedback state must stay per
    lane, and the convolution reverb is already SIMD
  - stretch/FFT and visualizer FFT only if preparation or UI becomes the
    bottleneck; the audio callback stays FFT-free
- [ ] Keep the existing `TOMPOUCE_SIMD` runtime policy, C fallback, per-kernel
      target flags and no-FMA rule. Every new kernel needs byte-exact PCM
      comparisons and forced-ISA runs; do not add a global `-mavx2` build.

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
