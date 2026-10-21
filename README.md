# Tompouce

Source: [ekollof/tompouce](https://github.com/ekollof/tompouce).

A terminal live-coding environment for music, inspired by Strudel, built in C.

New to Strudel? [Strudeltutor](#learn-with-strudeltutor) teaches live coding
through 22 interactive lessons inspired by vimtutor. Press **F2**, enter
**`/tutor`**, or use **`:tutor`** in vi normal mode to start.

![Tompouce running Bell dub with inline waveform and spectrum visualizations and console](docs/images/tompouce.png)

## Why?

Writing Strudel shouldn't require running a full browser or Electron app.
Tompouce embeds Strudel's JavaScript runtime in QuickJS and generates audio in C,
bringing the editor, sound engine and live visualizations into your terminal.

- **A small memory footprint.** More RAM stays available for samples, effects
  and other music tools. Effect, room and orbit buffers are allocated only when
  a session actually uses them: a measured live session (piano plus drums with
  room and delay, playing) peaked around 66 MiB of resident RAM, and the whole
  binary reserves 23 MiB of static storage. Usage varies with the composition
  and loaded samples.
- **A keyboard-driven workflow.** Write, evaluate, browse sounds and watch patterns
  in one place, with vi editing and your terminal's colors.
- **Local, offline music making.** Installed sound banks work without a network
  connection. Downloaded or imported samples stay available locally.
- **Native Linux audio.** Connect through PipeWire, PulseAudio, JACK or ALSA,
  alongside the rest of your audio setup.

The Chromium companion is optional: open it when you want browser controls,
while the audio engine keeps running natively. For terminal-only installation,
use `./install.sh --no-browser`. See [JavaScript and native audio](docs/javascript.md)
for supported Strudel features and differences from the browser version.

## Install

Clone the complete project and install on Linux x86_64:

```sh
git clone git@github.com:ekollof/tompouce.git
cd tompouce
./install.sh
~/.local/bin/tompouce
```

Alternatively, use `gh repo clone ekollof/tompouce` with an authenticated GitHub
CLI. The checkout includes the bundled runtimes, vendored dependencies and
examples; the installer downloads the sound banks separately.

The installer supports Arch/Omarchy, Debian/Ubuntu and Fedora. Run it as your
normal user; it uses sudo only for distro packages. It installs build libraries,
Chromium when needed, a private pinned Csound 7, and a compatible private
tree-sitter when the distro library is too old. It builds in a temporary directory
and installs the binary, companion assets, licenses and examples under
`~/.local/lib/tompouce`, with a command in `~/.local/bin`. No Node/npm is
needed for installation: both JavaScript bundles are included in the project.

Default sound banks: Dirt-Samples, Salamander piano, tidal drum machines and
uzu-drumkit and AKWF wavetable banks (roughly 456 MiB). They are installed beside the binary and work
offline. `./install.sh --all-banks` also installs the VCSL and mridangam catalog
samples (roughly 3 GiB total). Repository revisions are pinned in
`scripts/banks.lock.json`; downloaded files get SHA-256 receipts for safe resume,
and upstream README/license files are retained. Existing modified sound files
are not silently overwritten. Eight VCSL manifest paths omit a directory prefix;
the installer fetches the original files from their pinned actual locations while
preserving the manifest's local sample names and slot order.

Use `./install.sh --dry-run` to inspect the plan, `--prefix /absolute/path` for
another installation, `--skip-deps` for preinstalled system packages, or
`--no-browser` for terminal-only use. `--skip-banks` is available for build-only
setups. `--csound-prefix /path` reuses existing Csound 7 headers/libraries.
Installation needs HTTPS access and a UTF-8 terminal. Examples are in
`~/.local/lib/tompouce/examples`; press Ctrl+O to load one. Press F2 and enter
`/companion` to launch the Chromium companion. The installer leaves existing
editor settings, sessions and live processes alone. If `~/.local/bin` is absent
from PATH, use the absolute command shown above.

The installer verifies the JavaScript grammar and decodes installed piano,
TR909 and uzu samples with networking disabled before reporting readiness.
`python3 tests/test_install.py` tests plans, confinement, receipts and resume;
the fresh-install and repeat-install flows are also exercised on Arch/Omarchy.
Debian/Ubuntu and Fedora package plans have unit coverage; their package-manager
execution still needs testing on those distributions.

## Learn with Strudeltutor

[`strudeltutor.json`](strudeltutor.json) is a hands-on Strudel tutorial inspired
by vimtutor. Its 22 lesson buffers contain playable starters, editing exercises,
listening checkpoints and commented hints. Work through notes, rhythm, scales,
layering, envelopes, filters, signals and variation, then write your own piece.
The starters use built-in synths and work offline; sample-bank exercises are optional.

In Tompouce, press F2 and enter `/tutor`, or use `:tutor` in vi normal mode.
The shortcut copies the bundled lessons to
`$XDG_CONFIG_HOME/tompouce/strudeltutor.json` (normally
`~/.config/tompouce/strudeltutor.json`) and opens that working copy. Existing
copies are reused, preserving progress. Automatic song advancement is turned off.
Save any unsaved editor changes first; `/tutor!` or `:tutor!` discards them.
Use Ctrl+S, `:w` or F2 `/save` to save to the config copy. To save elsewhere,
use F2 `/save ~/strudeltutor-work.json`. The first lesson starts silently and
shows how to make your first note audible. The bundled master stays unchanged.

Keep song mode off with F2 `/song off`. Use Ctrl+N / Ctrl+B for the next /
previous lesson and F9 for the lesson list. F5 evaluates and commits edits to
the current lesson; press it before switching lessons to retain your work.
Ctrl+S saves the whole tutorial. Allow about 30–45 minutes, or take one lesson
at a time. F1 remains the Tompouce keybinding reference.

## Components

- `src/audio.c` — native real-time synth + drum voices
- `src/audio_output.c` — PipeWire/PulseAudio/JACK/ALSA output selection
- `src/native_synth.c` — ZzFX, bytebeat and bounded Kabelsalat playback
- `src/osc_output.c` — timestamped native UDP OSC output
- `src/slider.c` — `slider()` live values and source-site bindings
- `src/scheduler.c` — cycle-based scheduler with event queue
- `src/track.c` — code-to-tracks compiler
- `src/ai.c` — LLM-assisted code generation (OpenAI-compatible)
- `src/tui.c` — Notcurses terminal interface (editor, console, AI prompt)
- `src/wavetable.c` — procedural bandlimited single-cycle tables (`inst wt:<name>`)
- `src/mcp.c` — MCP JSON-RPC server + UI orchestration
- `src/csound_host.c` — isolated native Csound orchestra renderer

## Docs

- [`docs/wiki/Home.md`](docs/wiki/Home.md) — project handbook: installation, live coding, sounds, visuals, integrations, development and troubleshooting
- [`docs/wiki/Learn-Strudel.md`](docs/wiki/Learn-Strudel.md) — official Strudel workshops, reference pages and learning resources
- [GitHub Discussions](https://github.com/ekollof/tompouce/discussions) — questions, ideas and musical showcases
- [Strudeltutor](#learn-with-strudeltutor) — interactive lessons, exercises and hints inside Tompouce
- [`docs/examples.md`](docs/examples.md) — copy-pasteable example gallery
- [`docs/architecture.md`](docs/architecture.md) — module map, data flow, threading, limits
- [`docs/acceptance.md`](docs/acceptance.md) — manual acceptance test checklist
- [`docs/javascript.md`](docs/javascript.md) — upstream JavaScript Strudel runtime,
  native C audio coverage, imports, live highlighting, and remaining limits
- [`docs/audio-comparison.md`](docs/audio-comparison.md) — repeatable browser/native captures

## Build

CMake 3.20+ and Ninja are required for the default build:

```sh
cmake --preset default
cmake --build --preset default --parallel 4
ctest --preset default
```

The executable is `build/tompouce`; generated headers, objects and test binaries
stay in `build/`. `compile_commands.json` is generated for editor tooling.
Equivalent commands without presets are `cmake -S . -B build -G Ninja`,
`cmake --build build`, and `ctest --test-dir build --output-on-failure`.
Other CMake generators are supported in separate build directories.

Builds automatically use `ccache` when installed, including the installer's
temporary builds. Disable it with `-DWITH_CCACHE=OFF`; an explicit
`CMAKE_C_COMPILER_LAUNCHER` takes precedence. Inspect cache reuse with
`ccache --show-stats`.

Requirements: `notcurses-core` (Notcurses 3.0.9 or newer), `portaudio-2.0`, `libcurl`,
`libcjson`, `leveldb`, ALSA and `tree-sitter` 0.25+ development libraries, plus
pkg-config and `xxd`. Optional packages detected at configuration time are
`libpipewire-0.3` (0.3.7+), `libpulse`, `samplerate` and `fluidsynth` (2.2+).
FluidSynth is only the offline fallback for `gm_` instruments; their primary
playback needs no soundfont. The installer includes these packages and FluidR3 GM.

For a minimal PortAudio build:

```sh
cmake -S . -B build-minimal -G Ninja -DWITH_PIPEWIRE=OFF -DWITH_PULSE=OFF \
  -DWITH_SAMPLERATE=OFF -DWITH_FLUIDSYNTH=OFF -DWITH_CSOUND=OFF
cmake --build build-minimal --parallel 4
```

Optional libraries default to enabled when found. Explicit `-DWITH_...=ON`
requires the corresponding package; `OFF` disables it. Csound is enabled when
found unless `-DWITH_CSOUND=OFF` is set. Use `-DCSOUND_PREFIX=/path` or
`-DTREE_SITTER_PREFIX=/path` for private installations; their library paths
remain available to the executable after the installer copies it.

For `.csound()`, `loadCsound()` and `loadOrc()`, install Csound headers/library.
The current upstream livecode orchestra requires Csound 7. On Linux x86_64,
`bash scripts/install-csound7.sh` installs a pinned engine under `~/.local/opt`
without replacing system libraries; then reconfigure and rebuild.

CTest includes the native suite, all 31 official Strudel examples / 1,207 stored
reference events, Csound rendering, installer/import/companion checks and terminal
PTY tests. Assertions remain enabled in test executables for Release builds.
Use `ctest --test-dir build -R examples --output-on-failure` for a focused check;
`cmake --build build --target test-sanitize` runs the callback/vocoder/synth/editor
ASan/UBSan checks. `cmake --build build --target test-terminal-sanitize` checks
terminal rendering in four owned PTY geometries under ASan/UBSan.
See [fixture provenance](tests/fixtures/README.md).

Normal builds use the committed JavaScript bundles and vendored QuickJS.
Node.js/npm are needed only for explicit `js-bundle`, `companion-bundle` or
`compare-audio` targets, e.g. `cmake --build build --target js-bundle`.
After rebuilding a bundle, run the normal build again to embed it.

## Run

```sh
./build/tompouce
```

Audio startup tries **PipeWire → PulseAudio → JACK → ALSA**, skipping unavailable
backends and cleaning up failed connections before trying the next. PipeWire and
PulseAudio use their native clients; JACK and ALSA use explicitly selected
PortAudio host APIs. The engine remains stereo at 44.1 kHz; libsamplerate handles
JACK/device clocks such as 48 kHz. Without libsamplerate, a backend that cannot
accept 44.1 kHz is skipped. F4 and MCP `music_health` show the selected backend;
the console records startup failures. With no usable output, editing and MCP
remain available with `stream=down backend=none`. Selection occurs at startup.

Convolution automatically selects a spectrum kernel in the order AVX2, AVX,
SSE2, then portable C, according to CPU/OS support. The rest of the engine retains
its normal build settings. `TOMPOUCE_SIMD=c` forces the portable path;
`TOMPOUCE_SIMD=sse2`, `avx` or `avx2` requests that specific kernel with safe C
fallback when unavailable. `TOMPOUCE_SIMD=auto` is the default.
Selection happens before rendering, never
through feature detection in the audio callback. See the
[benchmark and verification notes](docs/simd-benchmark.md).
F4 shows the active convolution kernel, startup selection mode, usable SIMD
capabilities and any fallback to C, for example
`SIMD conv=avx2 mode=auto cpu=sse2/avx/avx2`.

Generated reverb supports `.roomsize(10)` / `.size(10)`, matching the web REPL's
maximum. This prepares up to a 15-second IR; `.size(4)` songs work without
shortening their room. See [room limits and behavior](docs/javascript.md).

`cmake --build build --target test-audio-backends` checks fallback and cleanup with temporary null ALSA
devices and disposable silent server streams. Native daemon checks skip when
their sockets are absent; `TOMPOUCE_TEST_JACK=1 cmake --build build --target test-audio-backends` also
checks a running JACK-compatible server. These checks do not change desktop
audio configuration or the user's running Tompouce session.

For a Chromium companion with voice activity, transport, tempo, console and
source editing, press F2, type `/companion`, and press Enter. Stop its helper
with `/companion stop`; it also exits with the engine. The companion is
loopback-only; audio stays native. Standalone launching via
`python3 scripts/companion.py --open` remains available.

Native ZzFX (`s('z_sine')` and related sounds), bytebeat (`s('bytebeat').bbexpr('t')`)
and Kabelsalat graphs (`note(60).K(sine(sFreq).mul(.1).out())`) run without a browser.
Append `.osc()` to route a pattern to SuperDirt or another OSC receiver over UDP,
defaulting to `127.0.0.1:57120`; `.oschost()` and `.oscport()` select the destination.
These patterns use external output exclusively and also work with no local audio
device. See [native synthesis and OSC](docs/javascript.md#zzfx-bytebeat-and-kabelsalat)
for syntax, supported nodes and resource bounds.

`node scripts/companion-qa.mjs` checks the real companion in a disposable Chromium
session against an owned engine. It requires a PulseAudio-compatible server to
create a private silent sink, verifies the route before applying sound, and
captures desktop/mobile layouts under `/tmp/tompouce-companion-qa`.

The companion and F6 share a categorized sound catalog with sample counts,
search, and source snippet insertion. In F6, choose **import-sounds**, type or
paste a folder path, then press Enter; the companion offers a folder chooser.
WAV, MP3 and FLAC files are copied to the config directory’s
`tompouce/sounds/` storage and appear under **user** after import and on
future starts. Each folder becomes a sound name, with alphabetical file slots
(`s("my_drums:0")`). Importing the same filename replaces its slot; explicit
`samples()` maps take precedence. Limits: 8 MiB per file, 256 MiB / 2048 stored
files, 256 sound names. Local bank browsing remains available in F6.

The **wavetables** category includes 65 Strudel `wt_` banks with 4,358 waveform
variants, alongside the procedural `wt:` oscillators. For example:
`note("c4 e4 g4").s("wt_flute").n(3)`. Each manifest bank appears once;
`.n()` selects its variants in manifest order. Installed banks work offline,
including code using `samples('bubo:waveforms')`. Native playback loops each
single-cycle waveform at the requested pitch for the note's sustain and release;
the browser's advanced wavetable morph/warp processing is not implemented.
AKWF attribution and its author's public-domain statement are preserved in
`thirdparty/waveforms/NOTICE`.

The **synths** category also includes 125 Strudel-compatible `gm_` names,
such as `gm_piano`, `gm_flute` and `gm_acoustic_bass`. For example:
`note("c4 e4 g4").s("gm_flute")`. These play the same per-instrument sample
packs as the Strudel web REPL (MIT-licensed WebAudioFont data, preinstalled
under `assets/banks/gm-webaudiofont/` and fetched on first play when
missing), so levels, sustain loops and pitch match the browser; they do not
require an external MIDI connection. Native playback provides the web's
default (first) soundfont variant per name; the web client's additional
variants selected by `n` are not replicated. Explicit sample maps
and imported sounds with the same name retain priority. Standard envelopes,
pitch modulation, gain, pan and native effects apply; speed and stretch scale
native GM pitch. GM notes are always note-gated like the web trigger.
Without the packs and without network access, `gm_` sounds fall back to
FluidSynth when available, using the installed FluidR3 GM font — set
`TOMPOUCE_SOUNDFONT` before starting to choose another SF2/SF3 file
(512 MiB maximum). FluidSynth's own audio-device probes are disabled;
Tompouce's selected output backend plays the prepared PCM, and the audio
callback never loads fonts or calls FluidSynth.

The editor’s bottom mini scope and piano roll can be hidden with F2 →
`/scope off` (restored with `/scope on`). This preference persists across
restarts. F12 controls the separate large visualization pane.

Append `._scope()`, `._pianoroll()`, `._punchcard()`, `._spiral()`,
`._pitchwheel()`, `._spectrum()` or `._wordfall()` to a playing pattern for a visualization
inline beneath that source line. Adding, moving or changing a view takes effect
without F5; musical changes still wait for evaluation. Views also activate when
loading a song. Scope shows that pattern's
native voices before shared bus/master effects; spectrum analyzes that same
isolated signal. Note views follow downstream changes and show
recent/held/upcoming notes over four cycles on the actual playback timeline.
`tscope` is a scope alias. The spellings without `_` also render inline.
Widgets scroll with code and stay out of the
editable text. F2 → `/inline off` hides them; `/inline on` restores them.

Terminals supporting the Kitty graphics protocol automatically get pixel
waveforms, note views, pitch wheels, spirals and spectra, targeting 60 FPS while visible.
Quiet scopes scale to the signal;
the piano roll distinguishes sounding notes and shows pitch/time guides.
Graphics piano rolls use eight rows, separate overlapping notes of the same
pitch into lanes, and mark each onset so repeated notes remain distinct.
Their y-axis includes octave-labelled notes and a keyboard guide. Waveform
scopes label linear amplitude on the y-axis, using the current auto-scaled
range. Labels reserve their own gutter so they do not cover the plots.
Other terminals retain braille. F2 → `/graphics braille` selects braille;
`/graphics auto` restores automatic detection. This preference persists.
Graphics defaults follow the terminal palette, including guides and labels.
By default, text, highlights and RGBA graphics use Notcurses' startup palette.
F2 → `/theme omarchy`, `/theme pywal`, or `/theme auto` enables live recoloring;
`/theme off` restores the startup colors. This preference persists as
`theme_source` in config.json. Omarchy reads
`$XDG_STATE_HOME/omarchy/current/theme/colors.toml` (default
`~/.local/state/omarchy/current/theme/colors.toml`), with the older
`$XDG_CONFIG_HOME/omarchy/current/theme/colors.toml` as a fallback;
pywal reads `$XDG_CACHE_HOME/wal/colors.json` (default `~/.cache/wal/colors.json`).
Auto prefers a valid Omarchy palette, falling back to pywal. Files are checked
once per second on the UI thread, including atomic replacement and theme symlink
changes. Missing or incomplete palettes retain the last valid colors.
Backgrounds retain terminal transparency. Explicit
pattern and visual-option colors take precedence. Terminals that do not answer color
queries keep the built-in fallback palette, with a transparent background.
Direct tmux/screen sessions retain braille. The separate F12 pane also retains
its text display.

Keys:
- `Tab` — complete a word in insert mode; indent two spaces in leading whitespace or a selection. `Shift+Tab` cycles completion backward, otherwise unindents.
- Type code in the editor (left pane).
- `F5` — evaluate code (`Ctrl+R` also evaluates outside vi normal mode)
- `Ctrl+P` — play / stop. Stop ends new scheduling; submitted notes and tails finish. Play restarts at cycle zero.
- `F1` — toggle Tompouce keybinding cheatsheet over the console pane (`PgUp`/`PgDn` scroll)
- `Ctrl+L` — hide / restore the console; the editor expands when no other right-hand panels are open.
- `F2` — AI prompt (supports `/bpm N`, `/play`, `/stop`, `/clear`, `/save`, `/load`, `/song`, `/rescan`)
- `F4` — toggle internals/state pane: editor mode, pending evaluation/unsaved
  changes, undo history, engine generations, graphics/inline status, audio/MIDI,
  sample-bank index, song/hold state, voices, queue and per-track timing.
- `F3` — toggle vi editor mode; `:` opens commands in vi normal mode (Tab completes). Motions, operators and search are listed below.
- `Ctrl+Space` — explicit completion for Strudel functions/methods, note/sound/scale strings; repeated Tab/Shift+Tab cycles candidates.
- `F6` — browse samples, drum-machines, synths, wavetables, user sounds and import-sounds. The search bar sits above persistent category tabs; Left/Right changes category while retaining the query. Manifest arrays and pitch regions remain one sound entry; drum-machine components are grouped under their bank name. Enter opens a bank or inserts a sound; Tab inserts the selected bank using its listed instruments. All and local-banks include standalone audio files only when no loaded manifest references them. Custom Strudel JSON maps under the banks root are discovered at startup; known catalogs retain their existing precedence. Import-sounds accepts a folder path in its own field. Backspace with an empty query goes up a bank, then returns to All. Insertion replaces the source selection without evaluating; F5 applies it.
- `F7` — song mode: play track buffers in sequence from track 0 (`/song [on|off|hold]`)
- `F8` — hold current track (loop it inside song mode)
- `F9` — track list pane (shows buffers, active highlighted)
- `F10` / `F11` — narrow / widen the code pane
- `F12` — piano roll + braille scope pane (per-voice pitch bars, live output scope)
- `Ctrl+Left` / `Ctrl+Right` — nudge the live `slider()` on the cursor line (or the sole registered control). Mouse dragging also changes its live value. After the initial F5 registers the control, adjustments update upcoming notes without reevaluating source or restarting playback. The source literal updates too, so saving retains the performed value. The code pane also draws a one-row history of accepted notes and a one-row scope when it is tall enough.
- `Ctrl+T` — copy entire buffer to a new track (next in list)
- `Ctrl+N` / `Ctrl+B` — next / previous track (loads into editor)
- `Ctrl+D` — duplicate current line (vi `yy`+`p` does this too)
- `Ctrl+S` / `Ctrl+O` — save / load the whole song (all tracks) via a file picker (`/save [path]`, `/load <path>`)
- `Ctrl+Q` — quit (session auto-saved to `~/.config/tompouce/state.json`)
- Arrows / Enter / Backspace — edit

Editor selection and mouse:
- Shift+arrows / Shift+Home / Shift+End extend a selection; Ctrl+A selects all.
- Ctrl+C copies, Ctrl+X cuts, Ctrl+V pastes using the editor's internal clipboard.
- In vi normal mode, `v` selects text and `V` selects whole lines. Move with
  `hjkl`, arrows, `gg`, or `G`; `y` copies, `d` cuts, and `p`/`P` paste.
- Left-click positions the cursor; drag selects text. Click or drag a slider
  bar to set its live value. Click the right pane to focus it, or a song track
  row to load that buffer. Click the prompt row to enter a command.
- The wheel moves through code or scrolls the console/cheatsheet. Escape returns focus
  to the editor. Hold Shift for your terminal's native mouse selection.
- Terminal paste uses bracketed paste, so multiline text is inserted as text.

Optional startup settings live in `$XDG_CONFIG_HOME/tompouce/config.json`
(default `~/.config/tompouce/config.json`):

```json
{
  "vi_mode": true,
  "mouse": true,
  "kitty_graphics": true,
  "split_ratio": 0.6
}
```

Defaults are non-modal editing, mouse enabled, and a 60% code pane.
`split_ratio` accepts 0.3–0.85; invalid settings leave defaults intact.
F3 switches vi mode and saves the choice for future sessions. Vi mode starts
in normal mode after a restart. Escape leaves vi insert mode
when vi mode is enabled and clears selections in non-modal mode.

Vi normal mode supports:

- Counts such as `5w`, `3dd`, `2d3w` and `12G` (capped at 9,999).
- `h/j/k/l`, `w/b/e`, `W/B/E`, `0`, `^`, `$`, `gg`, `G`, `{` and `}`.
  `^` moves to the first nonblank character. `j/k` and `gj/gk` follow wrapped
  editor rows, skipping inline visualisations; operator `dj/dk` uses whole lines.
- `f/F/t/T` plus an ASCII character, `;` to repeat and `,` to reverse the find;
  `%` matches nested literal parentheses, brackets and braces.
- `d`, `c` and `y` with motions or text objects: `dw`, `ce`, `y$`, `diw`,
  `daw`, `ci"`, `da(`, `di[` and `ci{`. Word/WORD, quotes and bracket objects
  also work in visual selection. Quote objects stay within a logical line.
- `dd/cc/yy`, `D/C/Y`, `x/X`, `s`, `r`, and counted `p/P`.
  `c`-operator deletion and following typing form one undo step within the usual
  typing run; `u` undoes and `Ctrl+R` redoes.
- `/text` and `?text`, Enter to search, `n/N` to repeat, and `*/#` for a
  whole-word search. Search is literal, wraps the buffer, and Escape cancels.
- `i/a/I/A/o/O` and `v/V`; Escape returns to normal mode. Horizontal character
  motions and deletion respect UTF-8 code-point boundaries.

In vi normal mode, `:` opens a local command line; Escape cancels it. Tab
completes command names, settings and song filenames; repeat Tab or Shift+Tab
to cycle choices. F2 remains the AI prompt, with completion for `/commands`.

| Command | Action |
| --- | --- |
| `:w[rite][!] [song.json]` | Save the whole song; `!` allows overwriting another existing file. |
| `:e[dit][!] [song.json]` | Open a song; `!` discards unsaved editor changes. |
| `:q[uit][!]` | Quit; `!` bypasses the unsaved editor guard. |
| `:wq[!] [song.json]`, `:x[it][!] [song.json]` | Save and quit; `x` writes only when changed or given a filename. Failed writes keep the editor open. |
| `:undo`, `:redo` | Undo or redo. |
| `:42`, `:$` | Jump to a logical source line or the last line. |
| `:set wrap`, `:set nowrap`, `:set mouse`, `:set nomouse` | Change and persist editor settings; `:set` or `:set wrap?` shows their values. |
| `:help`, `:evaluate` | Open the cheatsheet or evaluate as with F5. |
| `:tutor[!]` | Open Strudeltutor; `!` discards unsaved editor changes. |

Existing application commands also work after `:`, for example `:stop`,
`:bpm 120`, `:theme auto` and `:graphics braille`. Unknown commands stay local.
Save/open commands use Tompouce's whole-song JSON format, as Ctrl+S/Ctrl+O do.
Filename completion lists JSON files and directories, including quoted names
with spaces. F5 evaluates music and leaves the unsaved-file guard intact.
Quitting still saves Tompouce's recovery session, including after `:q!`;
`q!` leaves the named project file unchanged.

Editor completion uses vocabulary generated from the pinned Strudel runtime
without evaluating edited code. For example, `.ga` completes to `.gain`, and
`.s("squ` completes to `.s("square`. Sound names reflect the loaded catalog.
Completion replaces the whole token at the cursor, preserves following arguments
and forms one undo step even when cycling candidates. Comments are left alone;
Tab still indents at the start of a line and indents selected lines as a block.

Application shortcuts (including playback, sliders, track switching, clipboard,
save/load and Ctrl+D duplication) retain their existing meanings. This is a
native vi subset: regex search, Ex ranges/substitution/shell commands, macros,
named registers, marks, block visual mode and dot-repeat are not implemented.

Editor regression checks: `ctest --preset default` covers buffer edits and selection bounds;
`python3 tests/test_tui_pty.py` checks real terminal input, mouse reports,
startup configuration, paste, and terminal restoration on quit and signals.
The PTY script builds the checkout executable before running. Set
`TOMPOUCE_TEST_BINARY=/path/to/tompouce` to test an existing binary without rebuilding.

Flags: `--bpm N`, `--block N` (audio frames 64–4096), `--code '...'`,
`--mcp-port N` (TCP instead of stdio), `--mcp [--ui]`, `--help`.

JavaScript Strudel is the only source language. Quoted mini-notation supports
replication, elongation, alternation, Euclidean rhythms and polymeter through
the pinned upstream runtime. The retired C parser and line dialect are removed;
use `setcpm(BPM / 4)` and method chains instead of `bpm N` and pipe transforms.
Session auto-restores on launch; `/rescan` picks up added banks.

Example code:

```js
setcpm(128 / 4)
stack(
  s("bd sd [bd bd] sd"),
  s("hh*8"),
  note("c2 e2 g2 c3").s("sawtooth").rev(),
  note("c4 e4 g4 c5").s("square")
)
```

AI uses `OPENAI_API_KEY` (and optional `OPENAI_BASE_URL`, `OPENAI_MODEL`). Without a key, offline templates are used.

### AI data egress
When `OPENAI_API_KEY` is set and the network library initializes, the AI prompt **and the current code buffer** are sent to the configured `OPENAI_BASE_URL` endpoint. Do not put secrets in the code buffer. Point `OPENAI_BASE_URL` at a local model if you need to keep data on-host. If curl initialization fails, AI silently falls back to offline templates.

## MCP (external agents)

Run as an MCP server over stdio:

```sh
./build/tompouce --mcp
```

TCP MCP requires a shared capability. Generate it in the launching shell:

```sh
export TOMPOUCE_MCP_TOKEN="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
./build/tompouce --mcp-port 18789
```

Every newline-delimited TCP request must include the top-level `"auth"` field
with that token. A separately launched companion uses the same environment
variable. Stdio MCP needs no token. The terminal's `/companion` command handles
credentials automatically through a private pipe; its browser page never gets
the MCP capability. TCP rejects HTTP preambles and unauthenticated frames,
expires unauthenticated connections after two seconds and idle clients after
ten seconds, and disconnects peers that cannot accept a response promptly.

Remote sample and orchestra downloads validate the actual connection address
on every connection, including redirects. They connect directly and ignore
proxy environment variables so proxy-side DNS cannot bypass the public-address
policy. AI endpoint configuration retains its separate network policy.

Tools: `music_set_code`, `music_evaluate`, `music_play`, `music_stop`, `music_set_bpm`, `music_get_status`, `music_get_code`, `music_get_console`, `music_ai`, `music_song`, `music_track`, `music_session`, `music_health`, `music_debug`. Example Claude Code config:

```json
{
  "mcpServers": {
    "tompouce": {
      "command": "/home/ekollof/Work/music/code/tompouce",
      "args": ["--mcp"]
    }
  }
}
```

## Terminal UI

The interactive UI uses Notcurses (`notcurses-core`) for Unicode cells, screen
diffs and terminal input, including Kitty. The editor's existing inline Kitty
graphics transport and braille fallback share the same native renderers.

Bracketed paste commits as one edit when its closing marker arrives. A pause
between chunks does not cancel it; shutdown or input EOF cancels an unfinished
paste without applying a partial edit. The maintained PTY tests cover delayed
chunks and a delayed closing marker. Larger ASCII/Unicode and split-codepoint
stress cases remain unresolved in the current terminal integration; reproduce
them with `python tests/test_paste_stress.py ascii`, `unicode`, or `fragmented`.
These are explicit failing diagnostics, not passing acceptance coverage.

If a checkout is moved and CMake reports that its cached source directory no
longer matches, preserve the old `build/` under another name and configure a
fresh `build/` with `cmake --preset default`. Reusing those old executables does
not verify the current source. PTY checks can select an existing executable with
`TOMPOUCE_TEST_BINARY`; render checks use `TOMPOUCE_RENDER_TEST_BINARY`.

## Sample banks (`assets/banks`)

Now live: `| bank NAME` (drums + pitched), `s <bank-path>` one-shots, F6
search. Licenses verified against the downloaded trees — this section is
the required attribution for the CC-BY / CC-BY-SA banks.

| Directory | Content | License | Author / upstream |
|---|---|---|---|
| `akwf-free` | ~4000 single-cycle waveforms (WAV + C/JS) | CC0-1.0 (`LICENSE.md` in tree) | Adventure Kid, via <https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE> |
| `dough-samples` | Strudel default manifests (mostly remote refs) + local `piano/` subset | mixed, per bank below | Felix Roos, <https://github.com/felixroos/dough-samples> |
| `dough-samples/piano` | Salamander piano subset | CC-BY 3.0 | Alexander Holm |
| `mrid` (+ `dough-samples/mrid` ref) | Mridangam samples | CC-BY-SA 4.0 — share adaptations alike | © Arthur Carabott 2022, performer Harishankar V Menon |
| `VCSL` | Community sample library | CC0 | Sam Gossner / Versilian Studios, <https://github.com/sgossner/VCSL> |
| `Dirt-Samples` | SuperDirt/TidalCycles sample set | per upstream repo | TidalCycles, <https://github.com/tidalcycles/Dirt-Samples> |
| `tidal-drum-machines` | Drum machine collection | per upstream repo | <https://github.com/ritchse/tidal-drum-machines> |
| `vsco2-ce` | Chamber orchestra (SFZ) | CC0 (`LICENSE` in tree) | Recorded by Sam Gossner & Simon Dalzell, <https://github.com/sgossner/VSCO-2-CE> |
| `salamander-grand` | Yamaha C5 grand (SFZ) | CC-BY 3.0 (`LICENSE` in tree) | Alexander Holm |
| `fluidr3` | General MIDI SoundFont (SF2 + SF3) | MIT (`original-files/COPYING` in tree) | © 2000-2002, 2008 Frank Wen |
| `gm-webaudiofont` | Pinned per-instrument packs behind every `gm_*` sound (same files the Strudel web REPL plays; fetched on first play when not preinstalled) | MIT (`licenses/webaudiofontdata/LICENSE` in tree) | Srgy Surkv / Felix Roos, <https://github.com/surikov/webaudiofontdata> |

Notes:
- CC0 / MIT / public-domain banks need no permission and are credited above anyway.
- CC-BY 3.0 banks (Salamander piano): keep this attribution when redistributing.
- CC-BY-SA 4.0 (`mrid`): adaptations must be shared under the same license.

### Adding banks

Drop directories of `.wav` / `.mp3` / `.flac` anywhere under `assets/banks/`
(or point `TOMPOUCE_BANKS` elsewhere). Layout tips: flat per-instrument
dirs for drums (`mydrums/bd/*.wav`), note-named files (`_C4_`) for pitched
use, keep playable files within 8 levels. Then `/rescan` (no restart
needed). Only libre-licensed material — keep the table above current.
The index caches in LevelDB (`~/.cache/tompouce/banks.ldb`).

## License

Tompouce is licensed under the **GNU Affero General Public License, version 3
or later** (`AGPL-3.0-or-later`). See [LICENSE](LICENSE) for the full terms and
[NOTICE](NOTICE) for the project copyright and third-party attribution pointers.

Third-party libraries, vendored code and sample banks retain their own licenses
and notices. The project license applies to Tompouce's original source unless
otherwise indicated; it does not relicense those dependencies or sound recordings.

When distributing binaries, provide the corresponding source under the AGPL,
including the files needed to build them. For modified versions offered over a
network, follow the source-offer requirements in section 13. The public
repository link above serves the corresponding source.
