# TODO

Active work only. Completed work and measurement details are preserved in the
[preparation/parity archive](docs/archive/preparation-parity-roadmap.md) and
[earlier history](docs/archive/todo-history.md). Current behavior and coverage
live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and
[architecture](docs/architecture.md). Follow [AGENTS.md](AGENTS.md) for security,
verification, installation and upstream terminal constraints.

## Visual performance

- [x] Per-pattern `analyze`/`fft` controls. Native events now carry
      `analyze(id)` and the `fft` exponent (default 8), replay them through
      `FX(...)`, and feed up to 32 bounded analyser taps with generation tags.
      `fftSize` follows Superdough's `2 ** (fft + 5)` mapping, capped by the
      4096-sample ring; `music_audio_spectrum` selects the tap with `id`
      (0 = master) and computes the FFT in the MCP thread, never the audio
      callback. `test_javascript` covers direct, `_scope()` and nested/outer
      values; `test_audio`/`test_audio_sanitize` cover tap energy, FFT sizes,
      generation replacement, staged blocks and unchanged PCM;
      `test_mcp_security` bounds the `id` argument and response.
- [x] Native `analyze` control warning. `warn_unknown_controls` treated
      `analyze` and `fft` as unknown even though `audio_event_values` and
      `FX_OP_ANALYZE`/`FX_OP_FFT` handle them. Both are now in the known
      event-control list; `test_analyser_controls_do_not_warn` schedules a
      pattern and checks the console ring, while `test_javascript`/`test_audio`
      keep the event fields and analyser taps unchanged.
- [x] Hydra learn-page examples. The browser page now bundles the pinned
      Strudel pattern packages for `H(pattern)`, resolves function-valued Hydra
      parameters per frame, maps `o0`–`o3` to previous-frame feedback, feeds
      `detectAudio`'s `a.fft` from a bounded master-mix `music_audio_spectrum`
      tool, attaches a Strudel-style scope canvas as `s0` for `feedStrudel`,
      and renders on `requestAnimationFrame` while analysis stays at 8 Hz.
      `hydra_webgl` runs the four `strudel.cc/learn/hydra/` examples; the
      terminal runtime's inert placeholders are unchanged, and
      `test_javascript` evaluates each example's musical branches.
- [x] `/hydra` command: a separate loopback visualizer is now implemented and
      independent of F12 — `/hydra` / `/hydra stop` spawn a browser tab through
      the companion helper with a private token, Host/Origin checks, read-only
      `music_audio_scope`, `music_audio_spectrum` and `music_get_code` MCP tools
      (bounded 16–192 sample waveform, 64-band master FFT while `detectAudio`
      is enabled, polled at 8 Hz) and a parent-pid session guard. `tests/test_companion.py`
      covers the tool allowlist and auth. `web/hydra.js` renders through WebGL: generators
      (`osc`, `shape`, `solid`, `noise`, `voronoi`, `gradient`, `src`, `prev`),
      coordinate and color effects (`rotate`, `scale`, `pixelate`, `posterize`,
      `shift`, `repeat`, `repeatX/Y`, `kaleid`, `scroll`, `scrollX/Y`,
      `invert`, `contrast`, `brightness`, `luma`, `thresh`, `color`, `saturate`,
      `hue`, `colorama`, `r/g/b/a`, `sum`), binary and coordinate modulators
      (`modulate`, `modulateRepeat/X/Y`, `modulateKaleid`, `modulateScrollX/Y`,
      `modulateScale`, `modulatePixelate`, `modulateRotate`, `modulateHue`,
      `blend`, `add`, `sub`, `mult`, `diff`, `layer`, `mask`) through a bounded
      ping-pong pool with previous-frame feedback. `s0`–`s3` support bounded
      camera/screen streams, images, videos and MediaStreams; `src` keeps the
      audio scope path; `setResolution` and `hush` are available. The page
      switches from its 2D fallback to the WebGL stage only while a custom
      graph is set. `hydra_webgl` is a headless Chromium smoke test for
      generator output, the full effect set, feedback, owned canvas-backed
      camera/screen stubs, image sources, resolution changes, source-driven
      graph switching, the code overlay and the four learn-page examples. The scope tool includes the
      accepted source's `generation`, and
      the page drops stale frames when it changes. The page reads source
      through `music_get_code`, evaluates visual calls with inert musical
      stubs, and overlays the code. Custom `setFunction` GLSL and
      patchbay stream names stay outside the bounded backend.

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
- [x] Native LV2/Carla control warning cleanup. `lv2preset`, `lv2state`,
      `carlaProject`, `carlaTarget` and the `lv2param_*` prefix are valid event
      controls rather than unknown fields. `warn_unknown_controls` now accepts
      them, and `test_audio` schedules each form and checks the console ring
      for `control '...' is not implemented`.
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
      have three gated cases (`fx-nested-lpf`, `-room`, `-delay`, correlation
      0.99999997, RMS ratio 1.00033): query-time materialization exposes
      bounded step descriptors; callable chains retain only the web chain's
      default 0.8 gain, while pattern filter/send/pan controls replay. Four `fx-`
      gates cover the graph/effect boundary: the web does not apply that chain
      gain to a `K()` graph source, so the native renderer skips it there
      (`fx-graph-source-lpf` 0.99999997/1.0003 and `fx-graph-out-channels`
      0.99999994/1.0004) while keeping it for `audioin()` worklet effects
      (`fx-worklet-effect-gain` and `fx-worklet-effect-tail`, both
      0.99999994/1.0004). See
      [Kabelsalat effect tails](docs/audio-comparison.md#kabelsalat-effect-tails-and-input-routing).
- [x] Extend shared-effect checks beyond the 24 send/orbit/fixed-IR/lifetime
      cases: two `shared-room-source-` gates cover a panned source
      (`.pan(.7)`, correlation 0.99999998) and a downward-stretched source
      (`.speed(.5)`, 0.999705) feeding the mono IR. Two `shared-room-kernel-`
      gates cover kernel-change automation (`.roomsize` and `.size` patterns),
      both at correlation 0.99999999. Two `shared-*-graph-source` gates connect
      a Kabelsalat graph source to the shared mono IR (0.999999998) and the
      shared delay (0.9999990).
- [x] Shared-effect source eviction and live reconnection. `test_audio` now
      renders a delayed source, fills the current-source pool so the source is
      moved to a retiring graph, and verifies its shared delay samples survive.
      A new source then reconnects to the same orbit while the tail is active;
      the tail remains audible after the dry onset ends. Changing the orbit
      delay time and feedback through preparation keeps the existing delay line
      and reads it at the new offset. A second regression sends a source through
      the fallback room network, evicts it, and verifies reverb tail energy after
      the retiring graph has stopped. These are callback-level PCM checks, not
      new browser captures.
- [x] Nested FX shared/filter/pan routing and live edits. Materialized
      `_fxSteps` now replay bounded `room`, `size`/`roomsize`, `dry`, `delay`,
      `delaytime`, `delayfeedback`, `lpf`/`lpa`/`lpd`/`lps`/`lpr`/`lpenv`/`lpq`,
      the corresponding high-pass and band-pass controls, `pan`, `coarse`,
      `ftype`, `shape`, `crush`, `distort`, `distortvol`, `distorttype`,
      `postgain`, `chorus`, `shapevol`, `phaser` and its rate/depth/center/sweep,
      `tremolo` and its depth/skew/phase/shape, the compressor/pcurve
      controls, FM index/ratio/envelope/wave and numbered `fmi` matrix
      entries, `ir`/`irspeed`/`irbegin`, and `roomfade`/`roomlp`/`roomdim`
      into the event. Outer controls win because the web applies them after
      `FX(...)`; a `K()` graph source still skips the whole chain.
      `test_javascript` checks nested routing, outer precedence and the graph
      boundary; `test_audio` triggers a delayed FX source, renders its shared
      tail, then starts a live room+filter+pan+coarse+shape+distort+chorus edit
      with FM/IR values on the same orbit and verifies the delay tail remains
      audible.
- [x] Match the current site's two FX argument forms. Pattern arguments
      (`FX(gain(.5), lpf(800), pan(.5), coarse(4), ...)`) carry serializable
      controls and replay on the event; callable arguments (`FX(x=>...)`) reach
      the site as empty objects and are ignored, so native now ignores their
      steps and keeps only the chain's default 0.8 gain. New `fx-pattern-`
      gain/lpf/pan/coarse gates pass at correlations 0.99999967+ with RMS
      ratios near 1.0003; the three `fx-nested-` callable cases pass at
      0.99999997/1.00033.
- [x] Fractional IR-rate parity. `convolution_resample_ir` reproduces the web
      `adjustLength` index math, including truncation and the negative-index
      path for rates below unity. `test_convolution_prepare` pins `.5/.75/2/
      -2/0/1`; new browser gates `shared-room-lifetime-rate-half` and
      `-rate-threequarter` measure correlations **0.99999999/0.99999998** at
      the unchanged 0.99 correlation and 0.85-1.15 RMS thresholds.
- [x] Shared-room source-unity diagnostic investigated. `.irspeed(.5)` with a
      unity source renders near-silence in both the deployed player (peak
      0.000305 versus 0.767 at IR speed 1) and native (0.00033). Mono-source,
      longer-clip/release, stereo-IR and dry probes isolate the residual to
      the browser's low-level IR-speed path, not source width or the IR kernel.
      The 1.8 s active window now passes at correlation **0.99029** and RMS
      ratio **1.12166** with the unchanged 0.99/0.85-1.15 thresholds; 1.9 s
      falls to 0.98975. The full 2 s window stays diagnostic at
      0.98935/1.12449 (RMS inside). Per-100 ms windows localize the residue to
      the near-silent IR-speed edges: 0-0.5 s 0.966-0.989 at RMS 1.24-1.13,
      0.5-1.6 s 0.989-0.997 at 1.08-1.13, and 1.7-2.0 s 0.944-0.986 at
      1.30-1.32. The absolute RMS error is 1.65e-5 at peaks below 3.3e-4, so
      only correlation misses, by 0.00065. `diag-ir-tail` (0.2 ms source)
      isolates the sub-unity IR tail and matches at 0.99918/1.032 over the
      full 2 s, so the resampler and IR edge are not the residual; the
      deployed `adjustLength` is the backward index resampler native mirrors,
      not a phase vocoder. The harness now captures the main render as
      float32 (`float32: true`), and the original .03 full 2 s window gates at
      **0.999996/0.99208** without changing gain or thresholds; the int16
      capture measured 0.98935. The same path at gain 1 also gates on int16
      at **0.999988/0.9959**. The residual was the 16-bit capture floor, not a
      room-send envelope. The 1.8 s active window remains gated at 0.99029.
- [x] Seeded Arpoon opening gate. The previously diagnostic random-room
      `arpoon-opening` case now uses the shared `0xc0ffee` generated-room seed
      and passes at correlation **0.99986** and RMS ratio **1.0030** with the
      unchanged 0.99/0.85-1.15 thresholds. Unseeded multi-room sequences remain
      diagnostic because of the browser's incidental chain-ID draw.
- [x] Deterministic fractional switch gates. Generated-room IRs are produced
      asynchronously, so the offline render previously captured the dry source
      only and was zero between haps. The harness's `awaitRooms` option waits
      for every convolver in the main context to receive its buffer before
      `startRendering`; with it the full 2 s `pcm-onset-fractional-roomswitch`
      window gates at **0.998397/0.99724**. The first and second rooms also
      gate at **0.99935/0.98384** and **0.99556/1.01511** (windowStart 1.137 s)
      with the unchanged thresholds. Suppressing `AudioNode.disconnect` did not
      help; the truncation was IR readiness, not node release.
- [x] Generic `.lfo()` gain and cutoff paths. Gain uses a relative-depth
      triangle; `lfo-generic-gain` passes at **0.99965/0.99920**. Cutoff maps
      the deployed worklet parameters (`depth=1000`, `dc=-.5`, `min=-480`,
      `max=23500`, triangle, 0.5 Hz) onto the scalar sample-rate filter LFO;
      both the full 0.6 s window (**0.998685/1.00196**) and the steady
      `.05-.6 s` section (**0.9999996/1.00009**) pass the unchanged thresholds;
      the direct-form-I onset model is recorded in the next item.
      A 50 ms native base-cutoff hold was tested
      and rejected: full window 0.6076, steady gate 0.9580. A 50 ms unfiltered
      hold was also rejected: full 0.5459, steady 0.8557. A true 50 ms filter
      bypass with zero state was rejected too: full 0.6215, steady 0.99962.
      A tap on the deployed `BiquadFilterNode` output shows the first 50 ms at
      RMS 0.2039 and peak 0.39985 while the native scalar filter at the LFO
      minimum is nearly silent; the deployed filter passes the source before
      its LFO connection settles. A 50 ms dry-to-wet crossfade was rejected
      too: full 0.6590, steady 0.99977. The connection trace shows the LFO
      `AudioParam` connected last at time 0; the first filter quanta are
      RMS 0.106/0.281/0.372/0.398 then decay to 0.019 by the ninth quantum,
      which is a filter transient rather than the source. A one-quantum base
      hold improves full correlation to 0.8336 but overshoots (RMS 2.52, peak
      1.0) and is rejected. The onset is a more complex graph/filter connection
      response. Other targets keep their static value and log
      one `LFO modulation is implemented only for gain and cutoff` warning.
      `test_audio` locks the cutoff mapping, SoA bypass and dynamic-vs-static
      PCM. Filter-specific `lp`/`hp`/`bp` LFOs, vibrato, pulse-width LFO and
      tremolo remain separate supported controls.
- [x] Cutoff-LFO full onset. The full 0.6 s window passes at
      **0.998685/1.00196** and the steady section at **0.9999996/1.00009**.
      The pattern without `.lfo()` matches the complete window at 0.99999995,
      and the unfiltered source peaks at 0.2336 versus 0.39985 for the LFO
      case, so the onset is a filter overshoot. The per-sample cutoff path now
      uses direct-form-I filter state (Chromium's form), holds the base cutoff
      for the render quantum containing the LFO start, starts the phase at the
      absolute node start, and damps the previous output history by 0.77 at
      the connection. That reproduces the deployed overshoot (native peak
      0.399 versus browser 0.3998). The earlier one-quantum hold alone railed
      the transposed direct-form-II state (0.826, RMS 2.54, peak 1.0); the
      50 ms hold/bypass/crossfade and ramp/state-reset models remain rejected.
      `test_audio` and 53 `source-` cases stay green.
- [x] Real post-source filter/pan FX chain. Pattern `lpf`/`hpf`/`bpf` and
      `pan` stages run in recorded order before source pan, with independent
      left/right filter state. Source filter plus FX filter, multiple FX filter
      arguments and source pan plus FX pan now pass `fx-overlap-` gates at
      correlations 0.9999988+ and RMS ratios near 1.0003. Each FX argument
      applies its own default 0.8 gain, so two-argument chains compose.
      Coarse remains event-level and matches 0.99999997.
- [x] Pattern partials/loop windows. The site flattens `partials` arrays to a
      numeric control and does not apply them as arrays; it also ignores source
      loop windows in pattern FX. Native matches with `fx-pattern-partials`
      (0.9990/0.9939) and `fx-pattern-loop` (0.99999994/1.0003) gates.
- [x] Post-source shape and delay FX. Pattern `shape` runs after source shape
      with the FX waveshaper (`k=2*shape`); `fx-pattern-shape` measures
      0.99978/0.932 and `fx-overlap-source-shape` 0.99979/0.943. Pattern
      delay/IR sends are ignored by the site, so source delay remains
      (`fx-overlap-source-delay` 0.9999996/1.0005).
- [x] Pattern FX room send. FX room presence selects a mono-left room
      feed plus the source R/L ratio, and the ratio is held through the room
      tail. The source-pan feed gain is fitted at 1.423 center, 1.86 at half
      pan and 2.012 at full pan. `fx-overlap-source-room` measures
      0.9999994/0.9938, `fx-overlap-source-room-pan` 0.9999991/1.00056, and
      `test_fx_room_routing` covers routing, ratio and pan gain.
- [x] Source distort/chorus parity. Default `distort` now uses the deployed
      processor shape (`expm1(distort)`, postgain clamped to 0.001..1) and a
      post-source FX distort stage composes with source distortion.
      `fx-source-distort` measures 0.999999999/1.00006; `fx-pattern-distort`
      0.99954/1.127 and `fx-overlap-source-distort` 0.99993/1.065. The
      deployed player exposes no chorus control, so source, pattern and
      overlap chorus are no-ops; their gates pass at 0.9999999+.
- [x] ADSR and speed FX controls. Pattern `attack`/`decay`/`sustain`/
      `release`/`speed` are ignored by the deployed player, leaving source
      values/defaults in force. `fx-pattern-adsr`, `fx-overlap-source-adsr`,
      `fx-pattern-speed` and `fx-overlap-source-speed` all pass at
      0.9999997+ with RMS ratios near 1.0003; `test_javascript` checks the
      event fields stay at their source/default values.
- [x] Note/source/bank and string filter controls. Pattern `note`, `n`, `s`,
      `bank` and string `ftype` are ignored by the deployed player, leaving
      source values in force. `fx-pattern-note`, `-n`, `-s`, `-bank` and
      `-ftype-string` pass at 0.9990+; `test_javascript` checks the source
      frequency, token and filter type stay unchanged.
- [x] FX `distorttype` string variants. The pattern distort stage applies the
      deployed `t = expm1(distort)` argument to every algorithm, and all nine
      variants now pass the unchanged thresholds at 0.9999999+ correlation
      and 1.00011–1.00018 RMS ratio. The chebyshev gap was gain ordering: the
      site applies each FX-stage gain before its effect and the main pattern
      gain after the chain. Native now carries `fx_chain_gain` separately for
      pure FX chains, defaults `distorttype` to the site's index-0 `scurve`,
      and maps numeric values through the worklet key order. Source DSP
      voices keep the established folded chain gain. `test_fx_distort_fold`
      pins the fold curve; `test_fx_graph_gain` pins the gain separation.
      Callable argument replay remains intentionally absent because the
      current site ignores it; revisit if that changes. Very long modulation
      uses a bounded conservative connection deadline; distinguish these
      limits from established PCM parity. See
      [shared sends](docs/audio-comparison.md#shared-sends-and-mono-ir-tail-width).
- [x] Assess remaining visualization options and shared-effect tail attribution:
      documented in [visual feedback](docs/javascript.md#live-coding-and-visual-feedback)
      — browser-only styling is a deliberate ceiling, shared orbit tails and
      the combined Csound stream are intentionally not attributed per pattern,
      and terminal geometry/raster is checked against the web renderer without
      claiming browser-pixel parity.
- [x] Switch Angel external-device integration. The pinned `voc` register
      extension and `window.voc` helper now keep distinct toplevel meanings
      (an explicit setup name is not overwritten by a same-named `register`).
      `test_midi` loads a confined fixture of the pinned CC 34 helpers,
      checks note+CC and pitch-less CC-only events, verifies the helper's
      key mapping (`f#` 0, `g` 13, `c` 64, `a` 38), and delivers note/CC 34
      through an owned ALSA receiver while
      the native single output broadcasts `.midi('tout')` to subscribers.
      Earlier `test_oneshot_grid`, `test_switchangel_fx`,
      `test_switchangel_humanize` and `test_switchangel_bmod` remain. An
      owned engine also loaded the real pinned starter with its installer
      header and confirmed `window.voc` stayed the setup helper, not the
      curried `register` extension.
- [x] Starter bank-specific TR-909 example. Compact Strudel bank names
      `tr505`–`tr808`/`tr909` resolve to the installed `RolandTR*`
      tidal-drum-machines directories. `test_websamp` resolves `tr909` through
      the same `bank_find_dir` path used by audio, checks the target directory,
      and decodes `bd`, `hh`, `sd` and `cp` pieces from the installed manifest;
      an owned engine played `s("bd hh sd hh").bank("tr909")` with decoded
      samples (`bd` len 20547, `hh` len 6888). The starter's active
      `s("wt_digital")` wavetable also resolves from the installed
      uzu-wavetables catalog. The commented TR-909 example now resolves.
- [x] Pinned `roller2`/`bstab` sample path. The public installed manifests
      have no `bstab` default; only the unrelated generic Dirt `stab` bank is
      present. The native profile keeps the starter's three haps and rejects
      `bstab` individually with the existing sample-unavailable warning rather
      than substituting `stab`. `test_switchangel_roller2` checks the
      `bstab`/`supersaw`/`white` query names; `test_websamp` checks that
      `bstab` has no installed default and that generic `stab` exists. An
      owned engine confirmed the two oscillator voices play alongside the
      rejected sample.
- [x] Full drum-machine bank alias table. `src/banks.c` mirrors the pinned
      web `tidal-drum-machines-alias.json`: compact names such as `Linn`,
      `DMX`, `MPC60`, `SP12`, `HR16`, `D70` and `9000` resolve to their
      installed `Akai*`, `Oberheim*`, `Alesis*`, `Roland*` or `Linn*`
      directories. An explicitly installed directory with the compact name
      still wins. `test_websamp` resolves `Linn`, `DMX` and `9000`, checks the
      target directories and decodes an AkaiLinn bass drum; an owned engine
      played `s("bd sd hh cp").bank("Linn")` with `smp=0`, no sample warning.
- [x] Failed/slow stacked sources no longer suppress siblings. The scheduler
      previously discarded a sibling whose onset crossed while an earlier
      source prepared (a missing `bstab` lookup made this visible), and
      `commit_source_hap` also dropped late jobs before its late-onset path.
      Late siblings now skip duck timers but commit immediately, preserving
      their original end time; ended notes are still skipped. `test_audio`
      simulates the preparation delay with a prep observer and requires the
      failed sample plus both sibling oscillators to queue; an owned engine
      started `roller2`'s `supersaw`/`white` voices as soon as the missing
      sample lookup returned.
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
- [x] Owned MIDI note-off delivery under CPU load. `test_note_off_under_load`
      keeps two busy CPU threads running and verifies both the direct
      `midi_notes_for` path and a scheduler-generated
      `note(64).fast(4).midi().clip(0.04)` pattern through an owned ALSA
      receiver: every 20 ms note-off arrives between 19 and 31 ms of audio
      time. This covers generation, queue publication, the consumer and the
      MIDI note-off worker together. The maintained gate covers named CCs,
      inline maps, NRPN, controller/options defaults, duplicate
      scalar/command sends and exact note-on/off timestamp differences. Pinned
      numeric-string coercion drift remains explicit; do not replace the
      runtime or historical fixtures.
      An isolated resident daemon is available for testing through
      `--mcp-socket <path>` (0600 named Unix socket, no TCP port) with
      `scripts/tompoucectl.py` and an optional disabled-by-default systemd user
      unit from `scripts/install-daemon.py`; `test_daemon` covers auth, isolation
      and SIGTERM socket cleanup. The daemon was used to measure relative
      MIDI-to-audio onset latency through a separate SF2 synth (see
      [measured latency](docs/plugin-hosting.md#measured-midi-to-audio-latency));
      an absolute device-latency figure still needs external MIDI injection
      permissions or an in-engine capture hook.
- [x] Physical USB MIDI input verified with the attached AKAI LPK25
      (`32:0`): `midikeys('LPK25')` produced 5–9 concurrent sine voices from
      live key presses in an owned engine. The Behringer UMC404HD (`16:0`)
      exposes MIDI I/O, but no receiving synth or OUT→IN cable is present, so
      physical note-off output timing and device response are left to
      community measurement. See
      [physical output timing](docs/plugin-hosting.md#physical-usbdin-output-timing-community-follow-up)
      for the host-side coverage and remaining hardware setup.

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

## Prebake coverage

- [x] Switch Angel helper reference coverage. The gate now compares **90**
      cases after adding `toMajorKey`, `key_to_cc_value_for_autotune`, `sq`,
      `irando`, `roller`, `comb`, `bend`, `disperse` and `spinor`; `getCue` is
      exercised through the existing `cue`/`oncue` cases. `comb`/`disperse`
      compare after ignoring native's internal `_fxSteps` replay plan, and
      `roller` uses a 1e-12 tolerance for a 1e-16 delay-feedback difference.
- [x] `spinor` zero-delay Kabelsalat feedback. Native graph back-edges are now
      compiled as bounded one-sample feedback (at most 32 edges); only a
      delayed node's first input keeps the existing delay line. `spinor(1)`
      is back in the 90-case reference gate, and `test_native_synth` renders a
      `.sub(fn)` feedback graph for 2,048 samples with finite output.
- [ ] Custom Switch Angel `UISlider` widgets. Only the active `Volume`
      callback is mapped (to F4 output volume). Any other callback, including
      the commented `djfilt` example that uses `document` and
      `getSuperdoughAudioController()`, has no mapping. Decide whether to
      support a confined widget facade or make the unsupported path fail
      explicitly instead of a silent no-op.
- [ ] `roller2`'s `bstab` sample. No public installed manifest provides it;
      the native profile rejects that hap with the sample-unavailable warning
      while `supersaw`/`white` continue. Track a manifest/licensing path or a
      documented user-provided sample directory; do not substitute `stab`.
- [ ] Browser-only starter helpers. `document`, `bmod`, `FX` and arbitrary
      `getSuperdoughAudioController()` access are unavailable in the sandbox.
      Either add confined facades where semantics are well-defined or record
      each as an intentional ceiling with one observable warning; do not add
      silent fallbacks.
- [ ] Prebake PCM breadth. Only `prebake-glide`, `prebake-accent` and
      `prebake-delay` have audio gates. Add reference audio cases for
      `trancegate`, `tgate`, `acid`, `stxt`, `humanize`, `glitch`, `fmtime`,
      `strum` and the active `Volume` slider at a nonzero setting, using the
      existing correlation/RMS thresholds.
- [ ] Switch Angel external-device breadth. Note + CC 34 `voc` delivery is
      covered through an owned ALSA receiver; pitch bend, aftertouch, clock and
      multi-device/port routing remain unmeasured. Keep physical DIN/USB output
      timing in the separate hardware-blocked item.

## Render and SIMD performance

- [x] Full from-allocation SoA renderer. Target CPU-heavy songs such as
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
      - [x] Compact sorted active-voice list. The callback, onset dispatch,
        SoA admission and staged-route loops now visit only active slots via a
        sorted list, preserving slot/mix order and re-adding retirement slots.
        `test_active_voice_list` covers slot order, removal and reuse; the
        audio/sanitize and example PCM gates still pass.
      - [x] Supersaw from-allocation SoA lanes. The lane points at the
        voice's oscillator state created at allocation, uses the existing AVX2
        supersaw kernel, and owns the source/envelope/filter clocks and stereo
        biquad state. `test_soa_voice` compares the lane output exactly with
        the scalar supersaw source/envelope path; `test_audio` verifies a
        triggered supersaw voice is admitted and renders through its lane.
      - [x] Procedural-HH exclusion measured. Its scalar callback costs about
        **4.85 us per voice per 256 frames** on this box; the target song's
        ~1.3 HH voices/block therefore account for roughly **0.1 us per
        four-frame block**, below the benchmark/measurement noise and far
        below the ~1.2 ms render budget. A lane would also have to preserve a
        shared `rand()`/high-pass stream in slot order, which is not equivalent
        to the SoA group order. Lane admission is not cost-effective; the
        scalar path stays. Vowel, phaser, chorus and distortion are not used by
        this composition and still render scalar; add lanes for them only with
        a matching browser gate and a measured song that needs them.
- [x] Stereo sample-interpolation AVX2 candidate measured. The kernel is
      byte-exact against C in `test_dsp` (including forced direct AVX2 calls),
      but 5,000,000 interpolations measured **14.7 ms C vs 19.3 ms AVX2**
      (0.76x) on this box, so it is not used by the audio path. The scalar C
      path remains; this kernel needs a block/batch layout before it can pay
      for conversion and call overhead.
- [x] Four-lane polyBLEP saw AVX2 kernel. `src/oscillator_simd.c` is
      byte-exact against four scalar `dsp_oscillator` calls in `test_dsp`
      (including forced direct AVX2 calls); 2,000,000 groups measured
      **53.008 ms C vs 13.096 ms AVX2 (4.05x)**. SoA saw lanes now use the
      runtime-selected kernel; `test_soa_voice` remains exact and forced
      `TOMPOUCE_SIMD=c`/`avx2` runs pass.
- [x] Four-lane polyBLEP square AVX2 kernel. `dsp_square4` shares the
      polyBLEP helper and is byte-exact against four scalar
      `dsp_oscillator(1, ...)` calls in `test_dsp` (including forced direct
      AVX2 calls); 2,000,000 groups measured **68.625 ms C vs 18.462 ms AVX2
      (3.72x)**. SoA square lanes (including pulse widths) use the
      runtime-selected kernel; `test_soa_voice` remains exact and forced
      `TOMPOUCE_SIMD=c`/`avx2` runs pass.
- [x] Four-lane ADSR AVX2 candidate measured. `src/envelope_simd.c` is
      byte-exact against four scalar `dsp_envelope` calls in `test_dsp`
      (including forced direct AVX2 calls), but 2,000,000 groups measured
      **7.686 ms C vs 41.063 ms AVX2 (0.19x)**, so it is not integrated. The
      branchy per-lane envelope arithmetic is faster scalar.
- [x] Four-comb fallback-reverb candidate measured. `src/reverb_simd.c` is
      byte-exact against the scalar comb loop (line writes and read samples,
      including forced direct AVX2 calls), but 2,000,000 frames measured
      **88.2-88.6 ms C vs 91.3-92.3 ms AVX2 (0.96-0.97x)**, so it is not
      integrated. The scalar `tanh` call and four independent line addresses
      dominate; the convolution kernel remains the SIMD room path.
- [x] Stereo shared-delay layout and SIMD candidate. The delay line and
      feedback quantum are now interleaved L/R, and `src/delay_simd.c` carries
      a byte-exact C kernel plus an SSE2 candidate. Over 1,000,000 frames the
      old separate-plane scalar path measured **3.66-3.75 ms**, interleaved C
      **2.72-2.75 ms (1.35-1.38x)**, and SSE2 **2.71-2.79 ms (neutral versus
      interleaved C)**, so production uses the C kernel. Forced C/SSE2
      comparisons and the delay PCM gates pass.
- [x] Stretch phase-vocoder FFT kernel. `src/stretch_fft_simd.c` vectorizes
      the complex butterfly stages (length 8 and up) with an interleaved AVX2
      complex multiply; bit reversal, length 2/4 stages and the final divide
      stay scalar. It is byte-exact against C in both directions; 2,000
      forward+inverse pairs measured **60.2-61.5 ms C vs 45.8-47.5 ms AVX2
      (1.29-1.34x)**. Production `fft()` uses it and the stretch PCM gates
      pass.
- [x] Visualizer FFT kernel. `src/visual_fft_simd.c` precomputes the
      per-length twiddles once with `pthread_once` and shares them between the
      C and AVX2 butterflies; stages of length 8 and up are vectorized. 500
      4096-point transforms measured **126.6-128.3 ms legacy (sin/cos per
      butterfly), 15.6-16.5 ms table C, 11.0-11.5 ms AVX2** (about 11.3x
      versus legacy and 1.4x versus table C), byte-exact against the legacy
      loop order.
- [x] Persistent per-voice stereo filter. The byte-exact two-lane
      `DspBiquad2` kernel now keeps coefficients and state in the voice
      instead of rebuilding them per sample. `test_voice_soa_dsp` compares C
      and direct AVX2 calls byte-for-byte; `test_dsp` matches
      `dsp_stereo_filter_step` against two scalar `dsp_process` calls over
      4096 frames. Production triggers enable it for stereo samples the SoA
      bank cannot admit (loops and other fallback voices) and for supersaw;
      the SoA bank keeps its own four-lane path. A 256-block callback
      benchmark measured **1.02-1.04x** over scalar (ASAN 1.00x), while the
      kernel-only 200,000-frame batch is **1.21-1.28x**. Callback runs with
      `TOMPOUCE_SIMD=c` and `TOMPOUCE_SIMD=avx2` both pass the audio gates.
      This is a measured fallback-path gain, not a whole-callback speedup.
- [x] Forward real stretch FFT assessed. A 10 s stretch preparation measured
      ~206 ms; the forward real transform is ~48 ms (23.5%) and the complex
      transform ~74 ms. `src/stretch_real_fft_simd.c` is byte-exact against
      the legacy layout, but AVX2 measured **19.1-19.8 ms versus 14.4 ms for
      C** over 2,000 calls (0.73-0.75x), so production keeps the scalar real
      transform. The visualizer FFT is separately precomputed and vectorized.
- [x] Later SIMD kernels assessed. Saw, square and the visualizer/stretch
      complex FFTs are integrated; sample interpolation, ADSR, reverb combs,
      the stereo delay lane and the forward real stretch FFT were measured and
      rejected; routing/mix was neutral and FM's per-frame sine is scalar
      libm. No remaining candidate has a measured win; add one only after a
      profile shows it is needed.
- [x] `TOMPOUCE_SIMD` runtime policy audited. Production kernels use per-kernel
      `target("avx2,no-fma")` attributes with the C fallback; the only
      `-mavx2` build is the `EXCLUDE_FROM_ALL` benchmark translation unit.
      Existing kernels have byte-exact C/direct-AVX2 comparisons and forced
      `TOMPOUCE_SIMD=c`/`avx2` runs. Keep applying this rule to any new kernel;
      do not add a global `-mavx2` build.

## Code quality

- [x] Code style sweep. Audit the C sources against AGENTS.md: KNF style,
      `switch`/`case` instead of large `if`/`else` chains, C11 facilities where
      they fit, focused functions and state owned by its module. Converted:
      `audio.c` note parsing, drum token/instrument dispatch, LFO wave/target,
      oscillator DSP selection and pattern distortion variant/table dispatch;
      `tui.c` search-mode keys, the `/command` table/switch, the file-palette
      and command-input keys, and the main editor key dispatch;
      `mcp.c` tool plus `music_track` action dispatch; `dsp.c` FM shape
      selection; `sampler.c` drum slots; `websamp.c` note names; `midi.c` ALSA
      event dispatch. A contiguous-run audit now leaves only short
      mixed-condition selections (no run of six or more `else if`), so the
      remaining occurrences are local validation branches rather than
      action dispatch chains.

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
