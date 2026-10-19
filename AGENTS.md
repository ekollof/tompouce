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
- `cmake --build build --target test-terminal-sanitize`
- `cmake --build build --target test-controls-reference-web`
- `cmake --build build --target test-csound-reference-web`
- `cmake --build build --target test-audio-backends`
- `node scripts/effects-reference-qa.mjs`
- `node scripts/companion-qa.mjs`
- `node scripts/csound-native-qa.mjs`

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
- `src/audio.c` renders stereo at 44.1 kHz with 128 voices and 12 orbits.
  `src/audio_output.c` selects PipeWire, PulseAudio, PortAudio JACK, then ALSA.
  Runtime disconnects must not silently restart playback.
- The audio callback must not allocate/free voice state, decode, fetch, execute
  JavaScript, perform FFT preparation or make blocking syscalls. Prepare immutable
  samples, stretch PCM, synth programs and IR kernels outside it; preserve their
  queued/voice references and bounded caches. Keep existing lock windows short.
- Accepted notes, shared-effect tails and duck callbacks survive live slider,
  tempo and program changes. Duck callbacks use a bounded independent timeline;
  control-only events reserve no voice and emit no MIDI or note history. Preserve
  phase anchors and transport revisions rather than restarting the scheduler.
- Direct sliders are query-time references with source identity and source/undo
  synchronization. Aliased sliders follow the web's static fallback. Explicit
  evaluation/start behavior and failed-evaluation retention must match the REPL.
- `src/csound_host.c` owns a persistent confined renderer and compiler channel.
  Preserve asynchronous orchestra merges, the merge fence, held audio and loaded
  definitions. Do not weaken file/network/process/thread confinement to run an
  orchestra. Initialization/worker failures must remain observable.
- `src/tui.c` and its private headers own foreground editor/modal state.
  Notcurses owns keyboard and mouse protocols. Keep input decoding, wrapped-row
  geometry, cursor placement, selection and undo consistent. Palette replies
  must be consumed before editor input; do not add raw mouse-mode escapes.
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

## Security and code quality

- Use C11/C17 and preserve warning-as-error builds. Keep functions focused and
  state within its owning module; use `strtok_r` in reentrant/multilayer parsers.
- Validate external JSON, terminal/AI input, environment values and paths for
  type, length and range. Bound copies and writes, verify termination, and check
  allocation and API failures. Do not use unbounded string operations.
- Never pass untrusted strings to a shell, `system()` or an exec command. Fixed
  argv helpers must retain path confinement and size limits.
- Keep persistence under the intended config/storage roots. MCP session names
  are sanitized; do not expose unrestricted save/load/import paths.
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
Remaining areas requiring independent checks include missing duck targets,
nested/control-output routing, shared delay/reverb automation and tails,
voice-pool policy, large backend render bursts, Csound loader failure side
effects/diagnostics and controller coverage. Preserve current execution budgets
and distinguish CPU work from scheduling delays when investigating timeouts.
Do not claim complete MP3 decoding, synthesis or full-song waveform parity from
the maintained shared-PCM and selected browser audio gates.
