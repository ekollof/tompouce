# AGENTS.md

Tompouce is a terminal live-coding environment for music in C, with an embedded
Strudel runtime, a browser companion and MCP support.

## Working rules

- Use dedicated editing tools for text changes; do not substitute shell or
  runtime scripts for file editing.
- Preserve unrelated changes and untracked source, bundles and licenses.
- Leave the user's running engine, source, playback, audio routing and desktop
  settings untouched. Use owned processes and isolated state for verification.
- Check the executable actually used by the user. The installed command usually
  resolves from `~/.local/bin/tompouce` to `~/.local/lib/tompouce/tompouce`;
  rebuilding the checkout does not update that installation. Install verified
  user-facing fixes with the command below. Let the user restart their session.
- Keep this file concise and current. It is guidance, not a dated work log.
  Put user documentation in `docs/` and outstanding tasks in `TODO.md`.
- `docs/wiki/*.md` (except the repo-local `README.md`) mirrors to the GitHub
  wiki; after editing those pages, sync them to a scratch clone of the
  `tompouce.wiki` repo and push. The wiki backend only exists after the first
  page is created in the browser, and plain `git` needs SSH auth for it.
  Cross-links must use rendered wiki URLs
  (`https://github.com/ekollof/tompouce/wiki/Page`). A `Page.md` link is served
  as raw Markdown.

## Website

`site/` is the GitHub Pages site at <https://ekollof.github.io/tompouce/>.
`.github/workflows/pages.yml` deploys that directory when a push to `master`
changes `site/**` or the workflow, and on `workflow_dispatch`. Pages uses the
Actions source. Do not publish `docs/` as the site root. A push to `master` is
what publishes the site.

Edit `site/index.html` and `site/styles.css`. Keep page asset URLs relative so
they resolve under `/tompouce/`. Preview from `site/index.html` before pushing.
The README logo is `docs/images/logo.jpg`; the site copies are `site/logo.jpg`
and the favicon. Update both when the mark changes. `site/screenshot.jpg` is
the landing-page capture; `docs/images/tompouce.png` stays the README screenshot.
Keep the page's claims aligned with the README.

Social previews read the Open Graph and Twitter card tags in `site/index.html`.
`site/og.jpg` is the 1200×630 card. Its `og:image` and `twitter:image` URLs are
absolute (`https://ekollof.github.io/tompouce/og.jpg`) and the declared
`og:image:width` and `og:image:height` must match the file. Regenerate the card
when the logo, title or description changes, and keep that tag text identical
to the page description. Link caches may keep an older card after deploy.

## Build and verification

```sh
cmake --preset default
cmake --build --preset default --parallel 4
ctest --test-dir build --output-on-failure --parallel 4
```

Run `./build/tompouce` for the UI or `./build/tompouce --mcp` for newline-delimited
JSON-RPC on stdio. `--mcp --ui` uses `/dev/tty`; `--mcp-port PORT` enables the
loopback TCP listener. Keep stdout exclusively JSON in MCP mode.

CMake 3.20+, Ninja and Notcurses 3.0.9+ are required. Dependency details and
minimal-build switches are in README. `CSOUND_PREFIX` and `TREE_SITTER_PREFIX`
support private dependencies. ccache is selected automatically when available;
`WITH_CCACHE=OFF` disables automatic selection, and an explicit
`CMAKE_C_COMPILER_LAUNCHER` takes precedence. The installer normalizes temporary
source paths through `CCACHE_BASEDIR` without replacing a user-provided value.

```sh
./install.sh --skip-deps --skip-banks --no-browser --jobs 4
```

Use temporary prefixes for installer tests; never install distro packages as a
test. Preserve existing banks and private Csound dependencies. Do not relink
executables while tests are running that spawn those executables as workers.

Useful focused checks:

- `cmake --build build --target test-tui`
- `cmake --build build --target test-sanitize`
- `cmake --build build --target test-simd-sanitize`
- `cmake --build build --target test-convolution-prepare-tsan`
- `cmake --build build --target test-room-reference-web`
- `cmake --build build --target test-supersaw-reference-web`
- `cmake --build build --target profile-long-room-backend`
- `cmake --build build --target test-terminal-sanitize`
- `cmake --build build --target test-security`
- `cmake --build build --target test-security-sanitize`
- `cmake --build build --target test-controls-reference-web`
- `cmake --build build --target test-duck-reference-web`
- `cmake --build build --target test-voice-reference-web`
- `cmake --build build --target test-csound-reference-web`
- `cmake --build build --target test-audio-backends`
- `node scripts/effects-reference-qa.mjs`
- `node scripts/companion-qa.mjs`
- `node scripts/csound-native-qa.mjs`
- `cmake --build build --target compare-audio` (Strudel.cc PCM; see Fidelity)

PTY and integration scripts accept `TOMPOUCE_TEST_BINARY` to verify the installed
executable. `TOMPOUCE_TEST_FILTER` selects PTY cases. Browser/audio integration
checks must own their browser profile, engine and silent sink, verify routing
before nonzero gain, and clean up their resources. Temporary captures and old
reports are evidence, not build inputs or proof that the current tree passes.

After runtime changes, run `npm --prefix js run build`, then rebuild C to embed
the bundle. Rebuild companion assets through the `companion-bundle` CMake target.
Use `git diff --check -- ':!js/dist/runtime.js'` for authored whitespace checks.

## Architecture and invariants

- `src/javascript.c` owns the serialized QuickJS context. `js/runtime.mjs`
  adapts pinned Strudel 1.2.6; `js/dist/runtime.js` is the committed bundle.
  Preserve package locks and historical event references. Do not regenerate
  baselines or loosen thresholds to make a regression pass.
- `src/track.c` owns source/program changes; `src/scheduler.c` owns generation,
  transport, accepted events and history. `src/js_audio.c` prepares native events
  from haps. `current_code` is heap-owned through `current_code_set`; compiled
  source may differ after a failed evaluation or unapplied editor change.
- `src/audio.c` renders stereo at 44.1 kHz with 128 current sources, up to 128
  retiring graphs and 12 orbits. Retiring references remain callback-safe.
  `src/audio_output.c` selects PipeWire, PulseAudio, PortAudio JACK, then ALSA.
  Runtime disconnects must not silently restart playback.
- The audio callback must not allocate/free voice state, decode, fetch, execute
  JavaScript, perform FFT preparation or make blocking syscalls. Prepare immutable
  samples, stretch PCM, synth programs and IR kernels outside it; preserve their
  queued/voice references and bounded caches. Keep existing lock windows short.
  Convolution selects its CPU kernel before rendering; retain the portable C
  fallback, per-kernel target flags and `TOMPOUCE_SIMD` verification override.
  Mono-IR input-width transitions pause the right convolution history; preserve
  both channel clocks through cloning, reuse and stereo reconnection.
  Room-send width may end at natural sample completion before hap/FX cleanup;
  derive modulated lifetimes with bounded preparation work, never in rendering.
  Coalesce identical kernel preparations outside the callback. Reserve reverb
  random segments under the cache lock, generate from private state, and keep
  cache-hit random advancement logarithmic while preserving the stream.
  `src/periodic.c` prepares immutable periodic waves and looping noise buffers;
  never build or evict their tables in rendering. Keep pulse's source gain,
  render-quantum startup and stereo routing separate from square synthesis.
  Pulse/supersaw worklets skip quantum heads at or before Float32 begin;
  supersaw phases must not advance during those silent startup blocks.
  Pitch ADSR defaults and segment minima follow the REPL. Preserve oscillator
  a-rate automation alignment for partial first quanta; do not shift amplitude
  envelopes, sample transport or worklet startup to emulate it.
  FM depth scales by modulator frequency; carrier pitch automation must not
  retune the modulator. Keep its no-envelope default and first-quantum alignment.
  Explicit FM ADSR uses the shared AudioParam endpoint scheduler, exponential
  default curve and REPL segment minima; never square a linear envelope.
  Static detune is unison spread, not generic source transposition.
  BufferSource detune is k-rate; preserve first-evaluation event clamping and
  cache its ratio per 128-frame quantum without moving source onset.
  Worklet gate/fade/cleanup anchors retain the original hap and raw release;
  natural sample endings and source ADSR minima must not move those clocks.
  Final output volume uses a lock-free integer target, read once per block
  after voices/effects/limiting; preserve unity PCM and live tail continuity.
  Preserve floating-point operation order and test forced C/SSE2/AVX/AVX2 PCM
  equivalence; lower-ISA kernels must not emit newer instructions.
- Accepted notes, shared-effect tails and duck callbacks survive live slider,
  tempo and program changes. Duck callbacks use a bounded independent timeline;
  control-only events reserve no voice and emit no MIDI or note history. Preserve
  phase anchors and transport revisions rather than restarting the scheduler.
- Dominant MIDI gates use hap duration/clip in cycles and retain fractional
  milliseconds. Preserve mapped-CC-before-note order, complete NRPN transactions
  and bounded note-off admission; never emit an on whose off cannot be queued.
  Channel lists preserve order/duplicates under the 16-destination bound;
  reserve note-offs for the whole fan-out before any note-on. Channel messages
  fan out individually; transport and SysEx remain once per hap.
- Direct sliders are query-time references with source identity and source/undo
  synchronization. Aliased sliders follow the web's static fallback. Explicit
  evaluation/start behavior and failed-evaluation retention must match the REPL.
- `src/csound_host.c` owns a persistent confined renderer and compiler channel.
  Preserve asynchronous orchestra merges, the merge fence, held audio and loaded
  definitions. Do not weaken file/network/process/thread confinement to run an
  orchestra. Initialization/worker failures must remain observable.
  Live loaders apply at their source call sites; successful loads survive later
  JavaScript failure and fetch rejections are catchable. Never replay applied
  loads after evaluation or permit query-time loading.
- `src/tui.c` and its private headers own foreground editor/modal state.
  Notcurses owns keyboard and mouse protocols. Keep input decoding, wrapped-row
  geometry, cursor placement, selection and undo consistent. Palette replies
  must be consumed before editor input; do not add raw mouse-mode escapes.
  Known paste flood/drop and split-UTF-8 hangs await upstream fixes:
  [#2948](https://github.com/dankamongmen/notcurses/issues/2948) (related to
  [#2548](https://github.com/dankamongmen/notcurses/issues/2548)) and
  [#2947](https://github.com/dankamongmen/notcurses/issues/2947). Do not attempt
  local fixes for these failures or replace system Notcurses while upstream
  fixes are pending. Preserve the reproductions and report failures honestly.
  Missing Tree-sitter children are normal while typing; check for null nodes
  before calling byte/point accessors.
- `src/visual.c`, `src/visual_render.c` and `src/terminal_graphics.c` own bounded
  foreground visualization work. Rendering must not execute user JS; audio must
  not run visualization FFTs. Preserve visual generation/source mappings and
  guards against stale musical edits. Visual-only edits must not reevaluate music.
- `src/sounds.c` supplies F6, companion and MCP catalogs. Prefer bank manifests
  over individual components. Explicit sample maps override imports, then
  defaults. Import assets through the confined manifest-based sound store.
- JavaScript Strudel is the only source language. Keep mini-notation and tonal
  parsing in the pinned upstream runtime; do not restore the retired C parser.
- Prebakes run before song evaluation, within the same budgets. Keep their file
  reads confined and disabled at query time; external offsets must not bind
  song highlights/sliders. The optional Switch Angel starter stays a pinned
  opt-in download; retain its explicit native-profile diagnostics and attribution.

## Security and code quality

- Treat security as part of implementation and review, before a bug report.
  For new or changed input, IPC, network, filesystem and subprocess code, trace
  untrusted data to its effects and check authentication, confinement, resource
  limits and failure behavior. Reuse the existing guarded entry points; do not
  add an alternate route that bypasses their checks. Fail closed before side
  effects when validation or authorization fails.
- Use C11/C17 and preserve warning-as-error builds. Keep functions focused and
  state within its owning module; use `strtok_r` in reentrant/multilayer parsers.
- Validate external JSON, terminal/AI input, environment values and paths for
  type, length and range. Bound copies and writes, verify termination, and check
  allocation and API failures. Do not use unbounded string operations.
- Never pass untrusted strings to a shell, `system()` or an exec command. Fixed
  argv helpers must retain path confinement and size limits.
- Keep persistence under the intended config/storage roots. MCP session names
  are sanitized; do not expose unrestricted save/load/import paths.
  TCP MCP requires a capability on every frame; companion credentials travel
  through a private pipe, never browser HTML. Asset URL policy must check the
  actual connection address, including redirects, without a proxy bypass.
- Loopback listeners are trust boundaries: local users and browser-originated
  traffic must not gain unauthenticated control. Keep credentials out of logs,
  URLs and process arguments. Preserve companion Origin/Host checks in addition
  to authentication; validate complete protocol frames before dispatch.
- Bound input size, nesting, queued work, downloads and connection lifetimes.
  Slow, malformed or disconnected peers must not block playback, exhaust memory
  or terminate the engine. Check size arithmetic for overflow before allocation
  and retain ownership and cleanup on every error path.
- Enforce filesystem confinement at the operation, accounting for traversal,
  symlinks and check/use races; string prefix checks alone are insufficient.
  Give helper processes only the descriptors and privileges they need.
- Add focused regression tests for security fixes, covering rejected inputs and
  absence of side effects as well as valid behavior. Use owned local fixtures;
  never probe real internal services or expose credentials. Run `test-security`
  and `test-security-sanitize` when changing these boundaries, and document any
  remaining limitations without weakening protections to make tests pass.
- Use mutexes/atomics for shared state and signal-safe flags for handlers. Keep
  UI/terminal work on the foreground thread and logging observable through the
  console ring; do not add hidden control channels.
- AI requests can send the prompt and current source to the configured endpoint;
  keep that egress documented and bounded.
- Original project code is AGPL-3.0-or-later. Preserve LICENSE, NOTICE, dependency
  and sample attribution. The installer must retain runtime notices, including
  native synth/stretch licenses and the MIT fraction-conversion notice embedded
  from `js/query-time.mjs` into the bundle.

## Fidelity work

Target live-performance behavior against the actual Strudel web REPL, not only
the pinned event fixtures. Narrow green gates do not establish full parity.
See `docs/javascript.md`, `docs/audio-comparison.md` and `TODO.md` for coverage.

Compare native PCM with strudel.cc through `scripts/compare-audio.mjs`. Do not
add another capture harness. The browser side loads https://strudel.cc/ and
calls `renderPatternAudio`. Pattern evaluation, sample decoding and WebAudio
rendering come from that site. Only the source transpiler is the pinned 1.2.6
package. The native side is `build/tests/render_audio`, which runs the real
callback offline and opens no audio device. The harness owns a disposable
Chromium profile and does not attach to a running Tompouce session.

It is a network diagnostic, not part of `ctest`. It needs Chromium (`CHROMIUM`
overrides the executable), `npm ci --ignore-scripts` under `js/`, and HTTPS
access to strudel.cc. Sample catalogs come from `assets/banks` or the user-local
install; `TOMPOUCE_BANKS` selects another root. `TOMPOUCE_RENDER_AUDIO` overrides
the renderer path.

```sh
cmake --build build --target compare-audio
TOMPOUCE_COMPARE_FILTER=kabel- node scripts/compare-audio.mjs /tmp/tompouce-comparison
```

`TOMPOUCE_COMPARE_FILTER` keeps cases whose `name` contains that string. Pass
an output directory; the default is `/tmp/tompouce-comparison`. Each case writes
its source, native PCM, browser WAV, events and a `report.json`. A gated case
passes only when correlation is at least its `correlation` value (normally
0.99), the native/browser RMS ratio is inside 0.85–1.15, and the captures share
the requested duration at lag 0. There is no time shift or gain correction.
`correlation: null` is diagnostic and must stay that way. A failure is evidence
to inspect. Do not loosen a threshold, regenerate a baseline, or bump packages
to make it pass. The hosted player can change under an unchanged checkout.

Add a focused case in the script's case list when audible synthesis, envelopes,
effects or scheduling should match the site. Required fields are `name`, `code`,
`sound`, `cycles`, `cps`, `window` and `correlation`. The browser keeps haps
whose `s` equals `sound`, so set an explicit `.s(...)` that matches it; a `K()`
graph still needs that sound. Render length is `cycles / cps` seconds and
`window` must stay within it. Levels should stay below clipping. Record the
measured correlation in `docs/audio-comparison.md`. Passing a subset does not
establish full-song, decoder or synthesis parity.

Remaining areas requiring independent checks include missing duck targets,
nested/control-output routing, shared delay/reverb automation and tails,
voice-pool policy, large backend render bursts, additional Csound loader failure
cases and controller coverage. Preserve current execution budgets
and distinguish CPU work from scheduling delays when investigating timeouts.
Do not claim complete MP3 decoding, synthesis or full-song waveform parity from
the maintained shared-PCM and selected browser audio gates.
