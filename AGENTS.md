# AGENTS.md

Project: **Tompouce** – a terminal live-coding environment for music in C, with MCP support.

### Terminal-render test geometry (2026-10-18)

The user's `make test` abort was tests/test_terminal_render.c:230, confirmed from
the SIGABRT core for PID 330778. Its cell assertions assumed an 80x24 screen,
but right_geom reloaded the caller's 174x52 dimensions from S-Lang. Setting only
the editor's cols/rows did not constrain its backing screen. The harness now sets
SLtt_Screen_Cols/Rows after terminfo and before S-Lang initialization. The new
tests/test_terminal_render_pty.py runs it in owned 80x24, 174x52, 60x16 and 240x72
PTYs with isolated state, and is included in `make test`. The 174x52 reproduction
failed before the fix and all four sizes pass afterward. Extracted diagnostic
core copies were deleted. Full `make -j4 test` passes, including Csound and all
1,207 reference events. Production rendering and the user's app were untouched.

### Sample phase-vocoder stretching (2026-10-18)

`src/stretch.c` replaces the sample grain stretcher with the pinned reference's
2048-frame peak-region vocoder, 128-frame hop and Float32 Hann overlap-add. The
generator prepares immutable stereo PCM before queueing; audio only reads it and
runs existing effects. No FFT/decoding/allocation/JS runs in the callback. Sample
ADSR and pitch modulation precede the vocoder; ordinary effects follow it. Native
preparation accounts for source anticipation and quantum graph disconnect, while
musical event/highlight times stay unchanged. Low-level precursors before the
musical onset are omitted. Oscillator stretching retains direct pitch scaling.

Prepared PCM has a 90-second input-lifetime limit and a 128 MiB/128-buffer cache.
Atomic references keep queued/sounding buffers valid; unreferenced entries can be
evicted. Gain/filter/effect changes reuse source PCM. Refusal reports a console
error. Preserve include/stretch.h, src/stretch.c and the new Phaze/FFT licenses;
the installer explicitly retains both notices. Strudel versions and event
baselines remain unchanged.

Original stretched-piano correlation improved from 0.0805 to 0.9999 without
capture shifts or normalization. The maintained matrix now includes compensation,
fractional speed, upward shift, reverse, actual loop wraps, envelope/filter order
and downward shared PCM. Independent MP3 downward shifting remains diagnostic at
~0.97: using browser-decoded piano in the native renderer gave ~0.9999, confirming
input sensitivity. The reference's last downward peak region reads intermediate
upper-half real-FFT bins; the native real transform retains that layout. A shared
generated PCM16 WAV gate isolates DSP from MP3 decoding, recording its source hash.
Do not loosen thresholds or claim complete MP3/full-song parity.

Callback tests exercise compensated pitch, reverse playback, exact stereo polarity,
duration, cache reuse/eviction and queue/voice ownership; ASan/UBSan includes the
callback and vocoder. Sanitizers also exposed an old test fixture with one float
where the sampler contract requires interleaved stereo; the fixture now has two.
See docs/audio-comparison.md and TODO.md for the remaining decoder investigation.
The user's engine/source/playback and desktop settings remain untouched.
Validation passed: warning-as-error native/Csound suite and all 31 examples /
1,207 event references; callback/vocoder ASan/UBSan; all 67 PTY cases (~56 FPS);
all 20 browser audio gates; OSC and installer tests. An owned Pulse null-sink
engine also scheduled stretched piano notes with active voice snapshots after
its silent route was verified. Updated ~/.local/bin/tompouce through the installer
with --skip-deps --skip-banks --no-browser, preserving private Csound and banks.
Restart normally to load the new executable; no user engine was restarted.

### Backlog continuation: native synths, OSC and browser QA (2026-10-18)

The historical C statement/mini parser is marked for removal in TODO.md; retain
it until bundled examples, editor helpers and tests migrate. Its archived roadmap
is no longer an implementation plan. JavaScript uses the pinned Strudel 1.2.6
runtime; no upstream package or reference-event upgrades were made.

`js/native-synth.mjs` prepares bounded bytebeat expressions and Kabelsalat graphs;
`src/native_synth.c` implements their audio in C alongside streaming ZzFX. Explicit
Kabelsalat dependencies use the already locked 0.4.0/0.4.1 packages. Unsupported
nodes/expressions reject compilation. Programs have 256 operations, refcounted
eviction and queued/voice ownership; graph delays and the shared state budget are
bounded. Voice state allocation/free stays outside the callback and voice lock.
Graph ADSR sources retain their own envelope; graph effect tails currently remain
bounded by the source voice. This is an adaptation, not a complete browser host.
Preserve new source files, the bundled runtime and thirdparty/native-synth licenses;
the installer includes their notices explicitly.

`src/osc_output.c` prepares bounded native UDP /dirt/play packets on the generator
and sends timestamped bundles on the consumer. `.osc()` is exclusive external
output, with literal IP/localhost targets and validated numeric ports; no DNS or
browser bridge. A monotonic clock allows OSC/MIDI when no local audio backend
starts. Stop/start and no-output behavior have actual loopback integration tests.

Browser comparisons exposed default mono upmix attenuation, stock saw/square
normalization and bytebeat AudioParam precision/128-frame startup differences.
Native generated rooms now use bounded convolution following the reference decay,
fade and gradual low-pass algorithm, with a reproducible seed. Longer rooms/cache
exhaustion retain the existing reverb with a console notice. Thirteen maintained
gates cover synths, ADSR/filter/pan, dry piano, custom and seeded generated rooms.
Ordinary randomized wet Arpoon and native overlap-add versus browser phase-vocoder
stretch remain diagnostic; stretch waveform parity remains in TODO.md.

`node scripts/companion-qa.mjs` owns its engine, bridge, disposable Chromium and
private Pulse-compatible null sink, verifying the stream route before applying
nonzero gain. Real CodeMirror input/apply, tempo, voices, search, stop/play and
desktop/mobile layouts pass. Inspected full-page captures and report are under
/tmp/tompouce-companion-qa (temporary evidence). Native warning-as-error/Csound
suite, all 31 examples/1,207 references, ASan/UBSan synth checks, all 67 PTY cases
(~57 FPS), OSC integration, installer and companion tests pass. User engines,
source, playback and desktop configuration were not changed by these checks.
Updated the actual ~/.local/bin/tompouce installation with --skip-deps
--skip-banks --no-browser, retaining private Csound and existing banks. Installed
OSC/no-audio integration and real companion QA also pass. Restart normally to
load this executable; no user engine was restarted automatically.

### Continuous, colored spectrum (2026-10-18)

Spectrum history now fills observed time intervals independently of pixel width,
removing frame-spacing gaps at both graphics and braille resolutions. Long UI
observation gaps and actual silence remain empty. Sub-bin logarithmic bass bands
interpolate FFT magnitudes rather than remaining falsely silent. Default intensity
colors interpolate terminal blue/cyan/green/yellow/foreground with translucent
quiet energy; explicit visual and patterned note colors retain priority. All FFT,
history and color work remains on the foreground thread; audio is unchanged.
Renderer freeze retains its existing span/end. Raster regressions cover a broadband
impulse, continuous tone at irregular 20 Hz across three speeds and two resolutions,
freeze, real gaps, transparency, explicit colors and palette roles. The impulse
regression failed before the fix. Native/Csound suite and all 1,207 reference events
passed, along with all 67 PTY cases; Kitty spectrum transmission was about 57 FPS.
An inspected synthetic raster is /tmp/tompouce-spectrum-preview.png (temporary
evidence, not a build input). Updated the actual installed command using the
installer with --skip-deps --skip-banks --no-browser, preserving private Csound.
The user's live source/process and desktop settings were not changed; restart
normally to load the new renderer.

### Audio output backend priority (2026-10-18)

`src/audio_output.c` / `include/audio_output.h` now own output startup/shutdown:
native PipeWire, native PulseAudio, PortAudio JACK, then PortAudio ALSA. Native
connection waits are bounded to two seconds; each failed attempt closes its
resources before fallback. PortAudio initialization/open/start errors are checked.
The same C stereo renderer stays at 44.1 kHz; optional libsamplerate state is
allocated before startup for other PortAudio device clocks. No JS, sample I/O or
new allocations were added to the DSP callback. Selection occurs at startup;
runtime disconnects report inactive rather than silently restarting playback.
F4 and MCP `music_health` include the selected backend. Make detects optional
PipeWire/PulseAudio/libsamplerate packages; their distro development packages are
included in installer plans. PipeWire-specific compiler flags stay on its adapter,
not on the DSP/editor. Minimal builds can disable the three WITH_* switches.
`make test-audio-backends` uses owned silent streams and temporary null ALSA
configs; `TOMPOUCE_TEST_JACK=1` enables a running JACK-server check. Unit tests
cover priority, failed-open/start cleanup, missing devices and stereo/rate fidelity.
Validation passed: full warning-as-error native/Csound suite and all 1,207 event
references, all 67 PTY cases (~57 FPS), actual PipeWire/PulseAudio/JACK/ALSA and
no-output checks, six installer tests, and a build without optional audio libraries.
Updated the actual installed command with --skip-deps --skip-banks --no-browser,
retaining private Csound. The user's live PID/source and desktop audio settings
were left untouched; restart normally to load this executable.

Routing follow-up: the reported silent PipeWire stream had two active links to
the analog sink and was unmuted; read-only MCP showed `playing=no`. The user
started playback and confirmed audio. No production routing change was needed.
Backend integration tests now inspect two fresh owned silent streams with
`pw-dump`, asserting playback classification, autoconnect and two active stereo
links to one audio sink without creating links or changing desktop settings.
`make test-audio-backends` passed (five integration cases, optional JACK skipped).

### Highlighting after visual edits (2026-10-18)

Adding a scope attached its feed but stopped moving highlights because
`fill_play_mask` still required exact editor/evaluated text equality. The generator
now compares musical ASTs while ignoring visual wrappers/directives, comments and
layout, then publishes original/editor literal ranges. The UI maps cached spans
through those ranges, including UTF-16/UTF-8 conversion, without running user JS.
Musical edits and failed evaluation retain the stale-highlight guards. Scope
addition, relocation, options, later-line offsets, templates, patterned colors,
bold attributes and advancing notes have native/backing-cell regressions; the new
scope test failed before the fix. Validation passed: full C/Csound suite and all
1,207 reference events, 67 PTY cases (~57 FPS), installer startup, and five installed
no-F5 PTY cases. Updated the actual `~/.local/bin/tompouce` installation with
`--skip-deps --skip-banks --no-browser`. The live process/source remain untouched;
the user must restart to load this update.

### Immediate visual activation (2026-10-18)

Visual calls on running JavaScript branches now attach, move, change literal
options and disappear without F5. Song loading activates existing views normally.
The runtime observes bounded branches after transpilation, preserving mini source
offsets and musical event values. Scheduling includes hidden branch tags so new
scopes can read already sounding voices. UI requests are parsed on the generator;
they do not execute edited user code, change tempo, clear the queue or reset time.
Unchanged branches survive comments and unrelated edits. Musical changes and
options requiring JavaScript still require F5; unmatched branches show waiting.
Automatic feeds share the existing 32 tap slots with explicit sites. Per-kind
history stays bounded to 32 foreground caches. See docs/javascript.md for limits.
Validation: warning-as-error native suite, Csound, all 31 examples / 1,207 reference
events, actual callback attachment, and 67 PTY cases including new calls, options,
song loading and Kitty pixels without F5. Graphics transmission measured ~57 FPS.
The user's live process/source were not restarted or changed; restart normally
to load the rebuilt executable and bundled runtime.
Installed-copy follow-up: the shell command resolves to
`~/.local/bin/tompouce` → `~/.local/lib/tompouce/tompouce`, independently of the
checkout binary. The installed copy still had “evaluate to attach”; it is now
updated using `./install.sh --skip-deps --skip-banks --no-browser --jobs 4`,
retaining its private Csound rpath and existing banks. Installed MCP startup and
the five no-F5 PTY regressions pass. The existing process retains the old inode
and must be closed/reopened by the user. For future user-facing fixes, check the
actual executable path and update that installation, not just the checkout.

### Expanded vi editing (2026-10-18)

`src/tui_vi.h` is a private implementation included by tui.c after its editor
helpers, keeping all modal state on the UI thread. Preserve this header in
source transfers; the installer's source copy includes .h files. Counts (up to
9,999), word/WORD and paragraph/file motions, f/F/t/T and repeats, literal
bracket matching, d/c/y motions and text objects, counted paste, replacement,
literal wrapping searches and */# whole-word search are supported. j/k and
gj/gk retain wrapped-row navigation; operator dj/dk is linewise. Ctrl+R redoes
in vi normal mode; F5 still evaluates. Existing app shortcuts retain priority.
Search and find arguments do not edit the musical source. Escape cancels modal
prefixes, and mode/buffer changes reset them. UTF-8 motions/deletions stay on
code-point boundaries. c-operator deletion and subsequent typing share undo.
Undo now strips only editor_flatten's artificial final newline and restores
through editor_replace, preserving genuine blank lines/tabs. Fast Escape plus
an ordinary command is read as separate keys rather than discarded by S-Lang.
Tests exercise exact undo/redo buffers, UTF-8, operator counts, objects and actual
PTY input; README and F1 cheatsheet list supported keys and remaining vi limits.
Validation passed: warning-as-error build, native suite including Csound and all
1,207 published reference events, backing-cell highlight/state tests, and all
62 PTY cases (~58 FPS for graphics). No running user process or musical source
was changed. Regex/Ex/macros/registers/marks/block visual/dot-repeat remain outside
the implemented native vi subset.

### Terminal graphics palette (2026-10-18)

After Kitty capability detection, the foreground UI queries OSC 10/11 and the
16 ANSI colors using OSC 4, then polls every five seconds. Bounded OSC replies
accept BEL/ST termination and 1..4-digit RGB channels; only replies within the
query window can update colors. Input handling consumes these replies before
editor/undo logic. Foreground palette roles color all production graphics
defaults, guides, onset marks, labels and keyboard rails; transparent backgrounds
retain terminal opacity. Explicit visual options and patterned note colors win.
Unanswered queries retain the original defaults. Palette generations invalidate
smear/spectrum UI history on changes; no audio-thread changes or desktop config
edits. Unit tests cover parsing, rejection, fallback and explicit-color priority;
the Kitty palette PTY test decodes transmitted RGBA and checks actual colors and
unchanged source. See src/terminal_graphics.c and src/visual_render.c.
Validation passed: native suite (31 examples/1,207 event references and Csound),
all 51 PTY cases (~58 FPS), and a real owned Kitty preview whose live palette
was changed through its isolated remote-control socket. The scope changed to
magenta and axis text to the new foreground after polling; screenshot evidence
is /tmp/tompouce-terminal-theme.png. The preview and its socket were closed;
the user's ws3 app, song and desktop settings were not changed.

### F4 diagnostics (2026-10-18)

The state pane now shows editor mode, wrap/mouse/selection, pending evaluation
and saved/unsaved text, undo/redo depth, JS/native track counts and compile/queue
generations, graphics capability and cell geometry, inline placements, stream/
MIDI/bank-index status and song/hold position. Existing timing/voice/queue and
per-track rows remain. Panel height adapts to available terminal height (10,
14 or 18 rows). Reads use existing foreground-safe snapshots; no callback work
was added. Backing-cell tests cover modes, pending/dirty text, forced braille and
compact clipping; the F4 PTY case verifies actual keyboard activation. Tests
use isolated settings, and the user's running engine/source remain untouched.

### Native visual feedback continuation (2026-10-18)

Mixed vertical roll follow-up: screenshots of live Bass fugue showed drum
categories each filling a third of the chart and overlapping compressed bass
notes. The renderer now excludes drums from melodic auto-range, allocates one
uniform lane system to pitches/categories, and sorts drum keys consistently.
Vertical rolls have a top keyboard/name rail and low-to-high pitch left-to-right;
horizontal rolls retain their left rail. Quiet-note geometry remains visible
in braille. Tests cover mixed lane widths, axis pixels and quiet compact bars.
`tests/test_examples --roll-preview` shows actual Bass fugue data without audio.
Live screenshots /tmp/tompouce-roll-live-{1,2}.png and corrected preview
/tmp/tompouce-roll-corrected-preview.png are temporary evidence, not build inputs.
Orientation audit: punchcard and wordfall use the same corrected roll renderer.
Fixed reversed-time onset markers (use the trailing screen edge when flipTime
is set), and clip note label text to its bar width. Raster regressions exercise
all 24 kind/vertical/flipTime/flipValues combinations, including label containment.
Scope/spectrum/spiral/pitchwheel retain fixed native orientations.

Empty Arpoon roll follow-up: read-only live MCP confirmed visual queries were
interrupting at the 50 ms budget, leaving the roll cache empty. The adapter now
fills 1/8-cycle tiles near the playhead first, stops starting queries after a
12 ms work slice, and reuses/refreshes bounded tiles (256 / 8,192 events). The
50 ms interruption guard is unchanged. Waveforms skip note queries entirely.
Held notes are deduplicated across tiles, preserving identical simultaneous
events. A maintained Arpoon visual-cache regression failed before the fix;
its first populated update now takes about 13 ms locally. Audio event baselines
remain unchanged. The user's running engine/source were left untouched; it
needs a normal restart to load this rebuilt runtime.

`include/visual.h`, `src/visual.c` and `src/visual_render.c` add punchcard,
spiral, pitchwheel, spectrum, wordfall and the tscope alias alongside scope and
pianoroll. `js/runtime.mjs` assigns bounded source-site metadata/options;
all note views use tagged final haps. The piano roll originally captured its
input, but the user reported a mismatch on Arpoon: its subsequent slow(4) was
absent from the display. It now follows the final audio timeline, deliberately
adapting browser capture semantics. Arpoon regression compares displayed
onsets, ends and fractional pitches to the actual scheduled piano branch.
Separate generator queries cache up to 4,096 events at 8 Hz/50 ms budget;
the foreground UI reads that cache and interpolates time. Do not execute JS
from rendering or FFT from audio. Spectrum uses the existing isolated 4096-frame
tap and foreground Hann FFT/log-frequency spectrogram history. Scope stays a
waveform with amplitude labels, per user preference. Fixed per-site UI history
is generation tagged. Kitty and braille share the renderer; browser-only options
report a console message. Both call spellings render inline; direct global
transforms also work. One visible widget per kind/source line, 32 sites and eight
Kitty placements remain the layout limits. Mini active-token colors/basic
markcss attributes travel through cached scheduling spans. See docs/javascript.md
for supported options and browser differences. Preserve all untracked modules
and bundled runtime files when transferring this working tree.

Validation: clean warning-as-error build, native suite including Csound and
all 31 published examples/1,207 reference events, all 49 PTY cases (~58 FPS
two-widget transmission), FFT/low-resolution raster and styling regressions.
Real Kitty visual preview of all seven views was inspected; temporary evidence
is /tmp/tompouce-feedback-preview.png. The owned preview window was closed.
No user's engine or source was restarted or changed. Native/browser visual
options are intentionally not complete replicas; documented unsupported options
log once. Rebuild/restart normally to use the new executable.

### Project rename (2026-10-18)

The executable and installer now use `tompouce`. Existing `strudelterm` user
directories migrate only when the corresponding `tompouce` root is absent;
native state and Python sound import use the same precedence.
Migration renames the root without replacing an existing destination; it never
continues writing to the legacy directory. Failures are reported. Tests use
temporary roots; leave the user's live process and config untouched.
The old bank environment variable remains a fallback for `TOMPOUCE_BANKS`. Make also reuses
an existing legacy Csound prefix. Upstream Strudel packages, musical fixtures,
licenses and the `strudel://code` resource URI retain their original identities.
Rename checks passed: native suite (including Csound and 1,207 example events),
34 PTY cases (including legacy vi settings/imports), Python installer/import/
bridge/catalog checks, companion DOM tests and a temporary-prefix installation
with --skip-deps --skip-banks. No live engine was restarted.

### Installer continuation (2026-10-18)

`./install.sh` bootstraps Python if necessary, then invokes the standard-library
`scripts/install.py`. Default user-local install under `~/.local` supports
Arch/Omarchy, Debian/Ubuntu and Fedora package plans; Linux x86_64 is required
by the pinned Csound 7 binary distribution. `--dry-run`, `--prefix`,
`--skip-deps`, `--skip-banks`, `--no-browser`, `--all-banks`, `--csound-prefix`
and bounded `--jobs` are available. Do not run package installation as a test on
the user's machine: tests use --skip-deps and temporary prefixes.

Builds happen in an isolated temporary source copy, preserving the working
tree and live executable. Explicit runtime-file allowlists retain companion
assets and third-party licenses, excluding account/config/cache directories.
Only distro packages use sudo. Private Csound and (when system tree-sitter is
older than 0.25) pinned checksum-verified tree-sitter live under the app's deps.
Make supports `TREE_SITTER_PREFIX` with its include/lib paths and rpath.

Bank revisions and attribution paths are pinned in `scripts/banks.lock.json`.
Default core set is Dirt, piano, drum machines and uzu (~450 MiB); --all-banks
adds VCSL/mrid (~3 GiB total). Only manifest-referenced audio is copied, retaining
array/pitch order and decoded directory names. SHA-256 receipts enable verified
resume and reject modified local files; atomic writes and symlink/path guards
confine installation. Eight VCSL paths need the pinned Membranophones prefix
on the remote side; local manifest names remain unchanged. Embedded uzu defaults
now prefer installed regular files under uzu-drumkit when its catalog exists.

Installer checks `tests/test_install_runtime` (grammar ABI and offline native
piano/909/uzu decode) and an installed MCP startup/catalog smoke test with
isolated user state. Fresh core install, repeat install with private Csound,
and the private tree-sitter fallback were tested under temporary prefixes.
Package-manager plans have tests; actual Debian/Fedora package installation
has not been run here. No live process was restarted or user configuration
changed. See README Install for the user-facing commands.

### Latest continuation: categorized sound browser (2026-10-18)

Kitty axis guides: graphics rolls reserve a left gutter for adaptive note-name/
octave labels and a black/white keyboard rail. Scope waveforms label signed
linear amplitude at their actual auto-scaled limits and zero. User explicitly
chose waveform/amplitude labels instead of a frequency spectrogram. A bounded
raster font adapts to short scope images; plot mapping excludes the gutter.
Pitch ranges clamp to MIDI 0..127; dense pitch views thin labels at musically
aligned intervals. Tests cover C4/C#4/C-1/G9 labels, gutter isolation, amplitude
label presence and retained overlapping-note/onset rendering. No audio changes.

Piano-roll accuracy follow-up: read-only inspection of the live Arpoon showed
correct piano event tags, but four-row graphics and overlapping same-pitch
rectangles made repeated notes look absent. Graphics rolls now use eight rows,
bounded interval lanes per pitch and a final onset-marker pass. Braille defaults
remain four rows; extras falling back to braille inside a graphics layout safely
use the taller layout. Editor geometry, scrolling and mouse placement share the
same row count. A failing-then-fixed raster regression retains three simultaneous
same-pitch notes. A published Arpoon test compares plain/scoped events over 16
cycles and verifies all 90 piano notes reach the roll with music unchanged.
Live source, playback and process were left untouched.

Kitty graphics follow-up: `src/terminal_graphics.c` renders bounded RGBA scope/
piano-roll rasters and sends direct base64 APC chunks (no files or dependencies).
TUI capability query must receive the matching OK reply before graphics enable;
tmux/screen use braille. `/graphics auto|braille` persists `kitty_graphics`.
Foreground-only placements are clipped to visible inline rows and cell aspect,
paced to 60 Hz, limited to eight visible widgets (extras stay braille), and
deleted by owned IDs on removal/exit. S-Lang refresh precedes graphics; cursor
save/restore and synchronized updates keep overlays and editing aligned.
Same scope mask/generation and NoteHist feeds as braille; audio is unchanged.
UI idle waits use the remaining frame interval after rendering when Kitty
widgets are visible, retaining 50 ms idle waits otherwise. PTY coverage measures
actual transmitted frames and rejects the former 10 FPS throttle or busy loops.
The framerate follow-up measured about 59 FPS for two inline widgets in a PTY;
this measures native transmission pacing, not the terminal GPU's presentation.
New raster/transport tests decode every base64 chunk and validate cropping;
PTY checks cover detection, fallback, resize, manual mode, hide/exit cleanup
and input after replies. Real Kitty raster and UI previews were captured under
/tmp/tompouce-kitty-preview.png and /tmp/tompouce-inline-kitty-check.png.
Validation: native suite including Csound and all 1,207 reference events,
39 PTY cases including Kitty protocol simulation, plus real Kitty visual QA.
`TOMPOUCE_TEST_FILTER=Kitty python3 tests/test_tui_pty.py` selects graphics cases.

Inline scope visibility follow-up: ws3 screenshot showed a near-flat trace on
Arpoon's sawtooth branch with cutoff 180 and an LP envelope. Braille scopes now
normalize their display to the finite window peak (noise floor 1e-5), label
auto scale and join adjacent columns. Audio and per-pattern taps are unchanged;
filtered/polyphonic output will not necessarily resemble a raw oscillator.
Editor regression covers a quiet saw spanning the display and gain invariance.
Screenshot evidence: /tmp/tompouce-ws3-scope.png (temporary, not a build input).

Later UI continuation: `src/tui.c` now draws inline braille widgets below
scope/pianoroll method calls, using whole-source tree-sitter detection cached
on editor changes. Scopes and piano rolls now isolate the attached pattern:
the JS adapter assigns same-length call aliases so mini source offsets stay
unchanged, tags hap context and passes `scopeMask` into native events/voices.
Audio taps are post-voice DSP/envelope/pan and pre-shared orbit/master effects;
Csound's combined renderer has no per-pattern tap. Fixed atomic snapshot rings
carry generation tags; note history carries the same mask/generation. Up to
32 sites. UI binds original byte offsets to compiled metadata, refusing stale
bindings after unapplied edits. Rebuild `make js-bundle` for runtime changes.
Geometry includes display-only rows in wrapping,
scrolling and mouse placement, while cursor movement skips them. `/inline
off|on` persists as `inline_visuals`; `/scope off|on` separately controls the
old bottom mini display. That footer is suppressed while inline widgets are
present. F12 is now a manual independent large-pane toggle; visual calls no
longer automatically open it. Editor, JS, callback and PTY tests cover the
inline geometry and isolated feeds.
Terminal Tab now inserts spaces to the next two-column stop or indents selected
lines as a block; Shift+Tab unindents. Ctrl+Space invokes the former Tab
completion. Indentation uses transactional editor replacement and undo.
F3 also clears any active text selection before entering vi normal mode;
otherwise its selection colors hide the moving note highlights. Cell-level
tests use queried JavaScript haps and verify both mode switching and note-time
advancement through the real play mask and renderer.
Terminal highlighting continuation: avoid drawing leading indentation before
delegating a whole JavaScript line to the statement renderer (it doubled the
indent and clipped code). Ordinary unclosed quotes recover at the next logical
line; templates and escaped newlines retain state. UTF-8 glyph writes use byte
spans (`SLsmg_write_nchars`), not padded field widths (`write_nstring`). The
blank-fill buffer is terminated. `tests/test_terminal_render.c` checks actual
S-Lang backing cells/colors and wrapped editor output, now in `make test`.
GCC 16 IPA CP mis-specialized `editor_row_end` to width 1 in this harness;
its localized `noipa` attribute preserves the dynamic layout argument.

F6 and the web companion now share `src/sounds.c`: samples, drum-machines,
synths, native wavetables, user maps/imports and import-sounds. F6 additionally
retains local-banks. Both insert snippets at the source selection without
evaluating. F6 Left/Right switches categories; an empty Backspace returns to
the category list. Import-sounds accepts a pasted folder path in F6 or a
folder chooser in Chromium. `scripts/sound_import.py` copies bounded audio
into config `tompouce/sounds/`, using an atomic manifest and content-addressed
assets. `src/sound_store.c` loads that manifest and lazily resolves through
the native sampler. Explicit `samples()` maps win, then imports, then defaults.
MCP `music_sounds` queries categories/search/pagination; `music_reload_sounds`
reloads only the fixed config storage. No arbitrary-path MCP import exists.
Native import runs a fixed-argv background helper and reloads on completion.
Checks: native suite, imported WAV decode, catalog reload/source preservation,
Python import/bridge tests, companion DOM/editor tests, and 16 PTY cases.
The rebuilt engine needs a user restart; relaunch the companion afterward.
Do not restart or alter the user's live process. Browser visual QA remains
pending because no browser UI surface is available in this environment.

## Cross-machine handoff (2026-10-18, start here)

Companion paste/highlighting follow-up: replaced its plain textarea with a
locally bundled CodeMirror 6 JavaScript editor (`web/editor.mjs`,
`web/dist/editor.js`, `web/dist/licenses.txt`). Keep the web package lock and
dist artifacts in handoffs; `web/node_modules` is disposable. Rebuild with
`make companion-bundle`; no Strudel package versions changed. The Python
bridge serves the bundle and a nonce-based style CSP. Restart only the bridge
with `/companion stop` then `/companion` for these route/header changes.
DOM tests cover real paste events, multiline strings/comments, escaped HTML,
highlight spans, nonce styles, unchanged-poll selection/history, and undo;
frontend tests also cover an in-flight source poll during unapplied edits.
No live engine/source or companion was restarted automatically.

Companion latency fix: voice snapshots now poll independently at up to 30 Hz;
canvas animation uses rAF, extrapolates sample positions and clears stale
snapshots after 250 ms. Slow metadata does not block voices. `mcp_wait_input`
wakes the foreground UI for MCP socket activity instead of unconditional
50 ms sleeps; it watches the listener only when no client is active, avoiding
busy-waiting behind an occupied session. Routine client-close logs are omitted
to avoid telemetry filling the console. Full tests, all 13 PTY cases, four
bridge cases and `node tests/test_companion_frontend.mjs` passed. The disposable
engine served a 20-snapshot burst in 12 ms. Reload the companion for JS changes;
restart the engine for native wakeups. No live session was changed.

Companion launcher follow-up: from the UI use F2, `/companion`, Enter;
`/companion stop` stops the owned helper. `src/companion.c` spawns the
exe-relative Python bridge with fixed argv, captures its URL/errors into the
console, avoids duplicate launches and stops it on UI shutdown. The bridge
watches its parent as an orphan guard. Normal UI launches enable an ephemeral
loopback MCP listener on demand; existing TCP UI sessions reuse their actual
port. Stdio MCP UI sessions report that TCP terminal launch is required.
Full C/Csound tests and all 13 PTY cases pass, including real helper launch
against a disposable engine with a Chromium stub, duplicate prevention,
stop/exit cleanup and source readback. All four bridge tests pass. The user's
already-restarted live engine was left alone; this launcher needs a subsequent
restart onto the newly built binary.

Gap-closure follow-up: the user authorized remaining-gap work, prioritized
time-stretch/convolution and chose a Chromium companion tab. The tree now has
`make compare-audio` (`docs/audio-comparison.md`), default installed VCSL/mrid
catalogs, confined local ES imports, waveform-aligned sample pitch compensation,
partitioned custom-IR convolution, and `scripts/companion.py` plus `web/`.
See `docs/javascript.md` for controls, limits and deliberate differences from
browser algorithms. These additions supersede the historical deferrals below.
No live song source, playback, or process was changed. Existing processes need
a user restart to load new native code. ZzFX/bytebeat/Kabelsalat remain open.
Final checks passed: full C/Csound suite (31 examples / 1,207 reference
events), 11 PTY cases, four companion bridge cases, syntax/whitespace checks,
and `make compare-audio`. Dry C4, dry Arpoon and custom-IR correlation gates
pass; generated reverb is diagnostic only. The companion bridge is running
at http://127.0.0.1:18990/ for the current session; its UI needs visual QA
because neither Chrome nor IAB computer-use surfaces are available here.

Destination follow-up (2026-10-18): installed the pinned Csound 7 package,
rebuilt cleanly, and passed the full suite (31 examples / 1,207 reference
events, native Csound and callback checks), all 11 PTY checks, and
`git diff --check`. The user launched a fresh MCP/UI process and loaded
Arpoon. Read-only MCP confirmed the gapless stereo piano lengths, and the
user reports the opening phrase "Sounds similar now" against strudel.cc.
The fresh-process listening check is complete. Historical pending-restart
statements below describe the earlier machine. Broader audio parity and a
maintained browser/native comparison harness remain open; no source,
playback, pitch, or effect changes were made during this verification.

Latest task was to prepare this handoff only. The implementation is built and
tested locally; the next music task is to verify Arpoon's opening piano phrase
with the gapless MP3 fix in a fresh process. The user's last description was
that it sounds different from the web player. No pitch/octave workaround was
applied. Full browser sound parity and the user's listening result remain open.

### What must travel

- Preserve the complete working tree, including untracked source files.
  **Cloning the current HEAD or copying only `git diff` loses major work.**
  `git status --short` currently includes untracked `js/`, `scripts/`, new
  `src/` and `include/` modules, tests and `tests/fixtures/`,
  `docs/javascript.md`, and vendored QuickJS, Strudel Csound presets, and
  tree-sitter JavaScript sources under `thirdparty/`. Keep their licenses.
  Include `js/dist/runtime.js`, `js/dist/licenses.txt`, `js/package.json`,
  and `js/package-lock.json`; `node_modules` is disposable.
- Keep the compositions: `arpoon.json`, `belldub.json`, `bassfugue.json`,
  `flatrave.json`, and `juxundtollerei.json` are currently untracked.
  `tompouce-song.json`, if wanted, is ignored. Arpoon's published source
  can also be recovered from `tests/fixtures/strudel-examples.json`.
- `assets/banks/` is ignored and must be transferred or reinstalled
  separately. Preserve regular files and manifest-relative directory layout:

  ```text
  <banks>/Dirt-Samples/strudel.json                  + its audio files
  <banks>/dough-samples/piano.json
  <banks>/dough-samples/piano/*.mp3
  <banks>/dough-samples/tidal-drum-machines.json
  <banks>/tidal-drum-machines/machines/**
  ```

  Use `assets/banks` or set `TOMPOUCE_BANKS` to the new absolute root.
  Local catalog loading rejects symlinks and `..`. Bare `s('piano')` and
  `.bank('RolandTR909')` need these installed catalogs to resolve exactly;
  the default piano/machine manifests are not automatically downloaded.
  `.piano()` can load its explicit map, but do not rewrite Arpoon to hide a
  missing installation. The old machine's 909 default names and sample IDs
  are observations of that installation/process, not portable invariants.
- Optional user state: copy the wanted files under
  `$XDG_CONFIG_HOME/tompouce` (default `~/.config/tompouce`):
  `config.json` for editor settings, `state.json` for UI restoration, and
  `sessions/` for named MCP sessions. Startup vi mode uses `"vi_mode": true`
  in `config.json`; F3 now saves the selected mode there. Review old paths
  in restored state.
  Let the new machine rebuild its bank index; do not transplant
  `~/.cache/tompouce/banks.ldb` across changed bank paths.
  Downloaded sample files under `~/.cache/tompouce/web` can optionally
  travel for offline playback; otherwise URL-backed samples (including uzu)
  need HTTPS access to refill that cache. Decoded PCM lives only in memory.
- `/tmp/tompouce-*` diagnostic programs, browser captures, and WAVs are
  temporary evidence, not build inputs. This handoff and the durable fixtures
  below must suffice without them. No commit, archive, or transfer was made
  as part of the documentation task.

### Build and verify on the destination

Install GCC, make, pkg-config, xxd, and development libraries for S-Lang,
PortAudio, curl, cJSON, LevelDB, ALSA, and **tree-sitter**. The JavaScript
grammar is vendored, but the tree-sitter library is a system dependency.
Python 3 is needed for the existing PTY tests; do not use Python to edit files.

For the current upstream Csound orchestra, install Csound 7 before building.
`bash scripts/install-csound7.sh` installs the pinned, checksum-verified
7.0.0-beta.18 Linux x86_64 distribution into
`~/.local/opt/tompouce-csound7`. Its script also needs curl, unzip, tar,
and sha256sum. Other architectures need an appropriate native Csound build.
Set `CSOUND_PREFIX` for a custom install. Csound 6 supports older orchestras
but is insufficient for the current upstream `livecode.orc` syntax.

Rebuild locally instead of carrying binaries/objects or old absolute rpaths:

```sh
make clean
make -j4
make test
python3 tests/test_tui_pty.py
```

The C build embeds the existing `js/dist/runtime.js` without needing Node.
If editing the JS
adapter or rebuilding its bundle, install Node/npm and run `make js-bundle`,
then rebuild C. Keep the pinned Strudel 1.2.6 dependencies and reviewed event
baselines; do not upgrade packages or regenerate reference events just to
make a failure disappear. `make` regenerates the embedded `.inc` files.

Last completed checks after the gapless fix: `make -j4 test` passed with
`-Wall -Wextra -Werror -pedantic`, including 31 published examples / 1,207
reference events; all 11 `tests/test_tui_pty.py` cases passed; `git diff --check`
passed. Some loader/Csound checks need HTTPS access. The example event corpus,
synthetic sampler fixtures, and direct audio callback checks run offline;
real-bank assertions are conditional on those files being installed.
Without Csound headers, its tests report SKIP; a green aggregate exit alone
does not verify orchestra support. Check that the intended checks actually ran.
PortAudio device-init errors can be expected without audio hardware.

### Resume the fidelity investigation

1. Have the user load `arpoon.json` in the newly built process, using the
   transferred banks. `./tompouce --mcp --ui` provides the UI and MCP;
   read the launch rules below for terminal versus piped stdin. Confirm
   the actual endpoint and code with read-only status/code/console/debug
   tools before diagnosing sound. Do not overwrite a user's live source.
2. Confirm full stereo piano recordings and manifest-ordered TR909 snares:
   slot 0 is `naredrum.wav`, 12,766 stereo frames; slot 2 is `sd02.wav`,
   7,858 mono frames. Gapless C4 is 713,786 frames at 44.1 kHz (Fs3 711,201;
   Ds4 697,571). Sample IDs vary per process. `tests/test_websamp` includes
   installed-bank checks and always runs synthetic catalog/MP3 fixtures.
3. Listen to the first piano phrase against strudel.cc. The old machine's
   live process had the catalog fix but had **not** restarted for the MP3
   fix. That process/cache cannot establish whether the fix resolves the
   complaint. A fresh destination process decodes with the new code.
4. If a difference remains, capture identical passages through the actual
   native callback and an independent browser renderer, then isolate
   sample selection, timing/envelopes, and effects with measured evidence.
   Build a maintained comparison harness; the prior manual capture is not
   a permanent regression test. See the recipe and evidence below.

Keep sound algorithms in C. Never run QuickJS, allocate, download, or perform
file I/O in `pa_callback`. Use editing tools, not Python edits. No song-specific
hacks, octave compensation without evidence, Q retuning off the WebAudio dB
curve, or rounding the 0.02/0.1-semitone pairs. Stretch, convolution, ZzFX,
bytebeat, Kabelsalat, and new import loaders are deferred backlog, not this
investigation's next implementation tasks.

The old machine's `127.0.0.1:18789` listener and live song are local state:
leave them untouched. Do not assume that service, its sample IDs, temporary
browser profile, or diagnostic files exist on the destination.

## Build / run
- Build: `make`
- Clean/rebuild: `make clean && make`
- Run interactive UI: `./tompouce`
- Run MCP server over stdio: `./tompouce --mcp`
- Run MCP server with terminal UI: `./tompouce --mcp --ui`
- Run MCP over TCP (local only): `./tompouce --mcp-port 18789`

Requirements (system packages): `slang`, `portaudio-2.0`, `libcurl`, `libcjson`, `leveldb`, `alsa`, `tree-sitter`; GCC, make, pkg-config, and xxd. See the portable build notes above for Csound and tests.

## Architecture
- `src/common.c`: common helpers, console ring buffer, timing.
- `src/audio.c`: PortAudio real-time audio, 128-voice pool, synth oscillators and drum voices. ALSA chatter silenced (shares the terminal with S-Lang).
- `src/harmony.c`: Strudel-style chords, scales, and scale degrees (`| scale`, `chord`, comma stacks).
- `src/pattern.c`: mini-notation lexer + forgiving recursive-descent parser
  (tokens with columns; partial results + `pattern_last_error()` on typos;
  deliberately not bison: LALR all-or-nothing errors are wrong for live
  coding) and pattern expansion. Operators include `!` `@` `/` `?`,
  postfix euclid, polymeter `{}`, and in-pattern `|` choice.
- `src/strudel.c`: hand-written statement form (`note("…").lpf(800)`,
  `setcpm`, `slider()`). Bison stays out; a typo still keeps a partial pattern.
- `src/slider.c`: live slider values. The TUI nudges them without rewriting the source.
- `src/track.c`: code parser (bpm, drum, notes, bass, lead, statement lines), turns source into `Track`s.
- `src/scheduler.c`: cycle-based scheduler; generator thread looks ahead, consumer triggers audio. Also song list (buffer snapshots, per-cycle advance, lazy bank resolution).
- `src/ai.c`: OpenAI-compatible chat client (env: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`). Note: when an API key is set, the prompt **and current code** are sent to that endpoint — document/limit egress; falls back to offline templates if curl init fails or no key is set.
- `src/wavetable.c`: procedural bandlimited wavetables (`inst wt:<name>`; tables sine tri saw square organ acid voice pluck pad glass).
- `src/banks.c`: background index of playable files under `assets/banks` (wav/sf2/sf3/sfz, symlinks skipped), persisted in LevelDB (`~/.cache/tompouce/banks.ldb`, per-dir mtime validation).
- `src/sampler.c`: one-shot sampler (WAV/MP3/FLAC via vendored `thirdparty/`
  minimp3 + dr_flac; `| bank NAME`; alias + slot mapping, 44.1k interleaved
  float cache). Stereo files keep both channels. Melodic banks pitch-match
  by filename note, fractional playback, duration gating.
- `src/dsp.c`: native Strudel effect chain (filters, FM, drive, chorus,
  phaser, compressor). `src/js_audio.c` turns queried haps into `AudioEvent`s.
- `src/javascript.c`: embedded QuickJS. Pattern code is upstream Strudel
  1.2.6 (`js/runtime.mjs` bundled to `js/dist/runtime.js`).
- `src/csound_host.c`: isolated Csound orchestra renderer.
- `src/midi.c`: ALSA sequencer MIDI out (`| midi CH`), own off-queue worker.
- `src/tui.c`: S-Lang user interface (editor, console, AI prompt, piano-roll/scope viz).
- `src/mcp.c`: MCP JSON-RPC server + UI orchestration.
- `src/songfile.c`: whole-song JSON save/load (v2 `tracks`+`active`; v1 fallback), shared by TUI pickers/`/save`/`/load` and MCP `music_session`.

## Code style
- Keep functions short and single-purpose; prefer `strtok_r` (not `strtok`) for reentrancy.
- All shared state protected by mutexes; audio callback locks `voice_lock`; tracks lock `tracks_lock`.
- Never block in the audio callback; use lock-held sections sparingly.
- Keep UI state in `tui.c`, audio state in `audio.c`, etc.; don’t reach across modules unexpectedly.

## Secure coding practices (mandatory)
- **Never** use unbounded copies: prefer `snprintf`, `strncpy` with explicit size + NUL terminator, or `memcpy` with verified lengths. Ban `strcpy`, `strcat`, `sprintf`, `gets`.
- Treat all external input (MCP JSON, AI responses, terminal input, env vars) as untrusted: validate types, lengths, and ranges before use.
- Always bounds-check array/string writes (`code_lines`, `token[64]`, `out[65536]`). Never trust a length from a peer.
- Check every allocation and return value; handle `NULL`/failure paths.
- Use `strtok_r`, not `strtok`, in reentrant/multi-layer code.
- Keep object-size limits in mind for `snprintf`/`strncat`: always pass the *remaining* size, and verify termination.
- Do not shell out, `exec`, or invoke `system()` on strings derived from agent/LLM/terminal input.
- Do not write files based on untrusted paths; if persistence is added, sanitize and confine to a config dir.
- Keep the audio callback and scheduler lock windows short; avoid blocking syscalls there.
- Do not add backdoors/debug hooks (e.g., external process injection, hidden control channels) to production paths.
- Prefer `sig_atomic_t`/atomics for signal-shared flags; avoid non-reentrant calls in signal handlers.
- Run `make` cleanly under `-Wall -Wextra`; treat new warnings as errors to review.
  (`thirdparty/` vendored decoders are pragma-exempt: not our code.)

## Coding instructions for agents
- Preserve build cleanliness (`-Wall -Wextra`). Avoid new warnings.
- Test changes with `bash -n`-style sanity: rebuild and run `./tompouce` briefly under a PTY if possible.
- Do not use `strtok` in multi-layer parsers; use `strtok_r`.
- Avoid static storage changes that break reentrancy/concurrency.
- When adding UI behaviour, always update `tui_frame()` and keep cursor logic intact.
- MCP tools should always return simple JSON text content; never print extra non-JSON to stdout in `--mcp` mode.
- Sensible defaults: don’t break headless mode, `/dev/tty` UI, or interactive keys.
- Keep source code ANSI C compatible (C11/C17); GCC is used.

## Common pitfalls
- `PortAudio` init errors are expected in constrained environments; tests can still pass after init.
- `SLang` and `SLsmg` calls must be called from the foreground/UI thread.
- `isatty(0)` is used to distinguish interactive launch from MCP agent pipe.
- `current_code` is a heap string (`current_code_set`). It is the canonical source for MCP tools. Do not free the static empty string.

## Extending
- Add operations in `track_load_from_code()` and update parser tokens carefully.
- Add UI keys in `tui_run()`.
- Add MCP tools in `mcp.c` `handle_request()` and mention them in tools/list.

## MCP interface (for agents/harnesses)

### Launch
- Headless: `./tompouce --mcp` (JSON-RPC 2.0 over stdio, newline-delimited).
- With UI: `./tompouce --mcp --ui`. UI displays on `/dev/tty`. When stdin is a pipe, JSON stays on that pipe. When stdin is a terminal, MCP listens on `127.0.0.1:18789` (override with `--mcp-port`). Loopback only, no auth.

### Protocol methods
- `initialize`
- `ping`
- `tools/list`
- `tools/call`
- `resources/list` / `resources/read` (`strudel://code`)

### Tools
| Tool | Arguments | Notes |
|---|---|---|
| `music_set_code` | `{code: string}` | Replace & evaluate source. |
| `music_evaluate` | `{}` | Re-evaluate current code. |
| `music_play` | `{}` | Start playback. |
| `music_stop` | `{}` | Stop playback. |
| `music_set_bpm` | `{bpm: number}` | Tempo. |
| `music_get_status` | `{}` | Returns bpm, tracks, playing flag, etc. |
| `music_get_code` | `{}` | Current source. |
| `music_get_console` | `{lines?: number}` | Recent logs. |
| `music_ai` | `{prompt: string}` | Generates code via LLM, loads/evaluates. |
| `music_song` | `{enabled?: bool}` | Song mode: play track buffers in sequence, one per cycle. |
| `music_track` | `{action: copy\|next\|prev\|hold\|list\|goto\|move\|remove, index?: number, to?: number}` | Song track list ops (copy snapshots playing code; goto family follows editor). |
| `music_session` | `{action: save\|load\|list\|delete, name?: string}` | Sanitized-name sessions in `~/.config` (no raw paths). |
| `music_health` | `{}` | Engine diagnostics line. |
| `music_debug` | `{limit?: number}` | Read-only snapshot. `limit` is 1..48 rows per section (default 16). Text lines: master `fx` (cutoff 1 means that LPF is off), active pool voices, recent triggers (newest first), and queued events (soonest first). Per row: token, freq, gain, envelope, `smp` id, `begin`/`end` buffer fractions, playhead `pos`/`len`/`step`, hap `cut`/`q`/`ft`, current `fc`/`hfc`/`bfc` Hz, orbit, delay send, shape. `ft=-1` is the legacy one-pole. No sample paths or URLs. `music_get_status` `voices=` is the track count; this tool's `pool=` is the audio voice pool. |

### Example flow
```
1. tools/call music_set_code {code: "bpm 130\ndrum bd~bd~\nnotes c4 e4 g4"}
2. tools/call music_play {}
3. tools/call music_get_status {}
```

### Implementation notes
- All tool handles should update `current_code` when code changes (`current_code_set`).
- Maintain console logs with `log_console()` so agents can observe state.
- Do not print non-JSON to stdout in `--mcp` mode.
- Keep MCP server alive until EOF (blocking loop).
- Do not add unrestricted-path save/load tools: use `music_session` for
  sanitized names under the session directory. Agents can transfer source
  via `music_get_code` / `music_set_code` when changing the source is authorized.

## Gap handoff

Read the cross-machine handoff above first. `TODO.md` "Cross-machine
continuation" is the immediate checklist; "Gap closure" records completed
work. `docs/javascript.md` is the user-facing coverage note. Process-specific
observations below are historical evidence from the old machine.

### Continue here (web-player fidelity)

Latest continuation: the user restarted onto the catalog fix, then reported
varying piano octaves and an off-key first phrase. They described the sound
as different from the web player. Do not compensate by shifting notes:

- Read-only MCP showed the 909-name map and proper stereo piano recordings.
  A separate headless Chromium loaded actual strudel.cc. Its pattern engine
  generated exactly the same piano notes as QuickJS across 128 cycles:
  720 onsets, pitches, and native sample playback rates matched. The C4/Fs5
  MP3 files match the browser CDN byte-for-byte. An actual callback render
  confirms C4's fundamental, so no octave correction was justified.
- A concrete decoder mismatch was found and fixed in `src/sampler.c`:
  minimp3 frame decoding retained the Xing/Info frame, encoder delay, and
  end padding. Piano onset was about 2,257 frames late at natural speed,
  with the delay changing when repitched. `mp3_gapless` now reads bounded
  metadata (following minimp3_ex's CC0 delay convention), skips the metadata
  frame, removes encoder delay plus 529 decoder frames, and caps output
  at the original length before resampling. Untagged MP3s are not trimmed
  heuristically. Decoder state remains local; no audio-thread I/O was added.
- Native lengths now match WebAudio: C4 713,786, Fs3 711,201, Ds4 697,571.
  New offline generated fixtures in `tests/fixtures/mp3-gapless.h` cover
  stereo MPEG-1, mono MPEG-2/resampling, absent tags, invalid frame counts,
  and truncated input. `tests/test_websamp.c` also checks C4 against the
  observed browser length when installed banks are present.
  `make -j4 test` passes with the gapless decoder and new fixtures.
- One-off offline renders are in `/tmp/tompouce-opening-piano.wav`
  (corrected native callback) and `/tmp/tompouce-web-opening-piano.wav`
  (actual browser WebAudio). The opening .6 seconds aligns at zero sample
  offset with normalized waveform correlation .976. This is a measured
  sample/onset improvement, not proof of identical effects or listening parity.
  Diagnostic sources and a temporary Makefile are `/tmp/tompouce-piano-*`
  and `/tmp/tompouce-opening-piano.c`; they are not production hooks.

The user has not restarted since the gapless decoder fix. Inspect/listen
again after their next restart; do not restart or change the live source
yourself. The older catalog audit and fix details follow.

The user loaded **Arpoon** after the Belldub checks. Its source matches
the published example (ignoring trailing whitespace). Before the catalog
fix, read-only MCP showed healthy playback/queue and no unsupported-control
warnings, but two sample-selection mismatches:

- `piano` is sample ID 6 in that process, 600 mono frames, for every
  pitch. The default table has no piano entry, so `resolve_sample`
  falls through `bank_find_dir("piano")` to the earlier indexed
  `akwf-free/AKWF--Elektron--Model Samples/piano` directory. Its first
  file is `pno 1.wav` (654 frames at 48 kHz -> 600 frames at 44.1 kHz).
  The proper installed `dough-samples/piano/C4v8.mp3` is stereo and
  about 16 seconds long. Bare `.s('piano')` needs the default pitch-keyed
  piano manifest, not the unrelated directory or a song-specific rewrite.
- `.bank('RolandTR909')` uses the filename-alias fallback because the
  drum-machine default catalog is absent. Kick (`Bassdrum-01.wav`,
  20,547 frames) and closed hat (`hh01.wav`, 6,888) match by coincidence.
  `sd:0` is `sd01.wav` (14,038 frames) but should be `naredrum.wav`
  (12,766 stereo frames). `sd:2` is `sd03.wav` (13,586) but should be
  `sd02.wav` (7,858). Register the manifest and preserve array order;
  do not add a special alias for the misspelled filename.
- Installed `assets/banks/dough-samples/piano.json` and
  `tidal-drum-machines.json` match the current upstream manifests.
  Native filter/duration/delay controls and fractional pitch pairs
  appear consistent with the code. The published-event regression
  does not check which sample the audio resolver chooses.

Catalog fix now implemented and tested in `src/websamp.c`:

- Default loading reads installed `dough-samples/piano.json` with audio
  rooted at `dough-samples/piano`, and `dough-samples/tidal-drum-machines.json`
  with audio rooted at `tidal-drum-machines/machines`. The existing root
  search policy (env/cwd/exe-relative) applies to each catalog.
- Default slots now share `WebSlot`/`WebName` and `select_slot` with user
  maps. Pitch-keyed objects/arrays preserve their declared MIDI roots;
  fractional pitches choose the nearest region before n rounds/wraps
  inside it. Drum-machine arrays retain manifest order. User maps win,
  remain separate, and uzu still overrides bare Dirt drum names.
- Limits: 2,048 default names, 1,024 slots per name, 16,384 total slots.
  The installed set has 909 names. The old limits (512 names / 64 slots)
  would truncate the drum-machine catalog, which has 683 names and
  includes a 108-slot bank.
- `tests/test_websamp.c` always runs an isolated offline fixture covering
  600+ names, 108 slots, pitch regions/arrays, negative/fractional indices,
  roots, array order, and user overrides. When real banks are installed,
  it also checks a full stereo C4 recording and exact RolandTR909 snare
  lengths/channels: slot 0 = 12,766/stereo, slot 2 = 7,858/mono.
  `make -j4 test` passes, including all published event references.

The user subsequently restarted onto this catalog resolver; MCP confirmed
the full piano recordings. Source and playback were left untouched.
The newer gapless decoder change still needs a restart, as described above.
The earlier Belldub findings follow.

Earlier goal (completed routing check): Belldub should use the same drums as
strudel.cc. The old UI subsequently switched to Arpoon. Do not `music_set_code`
on that live process. Do not kill the running
`./tompouce --mcp --ui` (loopback `127.0.0.1:18789`). A rebuilt binary
does nothing for that process until the user restarts it. Do not retune
low/high-pass Q off the WebAudio dB curve. Do not round the 0.02 and 0.1
semitone offsets. Do not start stretch, convolution, ZzFX, bytebeat, or
Kabelsalat.

What the live `music_debug` snapshot already matched: bass cutoff 200 Hz
with Q 20 and a 2-cent pair; chord cutoff in the perlin 400–3000 range
with Q 1 and 10-cent pairs; bell `begin` 0.05. What did not: `mt` / `lt` /
`ht` / `misc` were synth hats (`smp=-1`), and `hh` was a long local
Dirt-Samples file (~38458 frames), not uzu-drumkit.

Verified/fixed in the continuation (2026-10-17):

- The failing test printed `pos=5 step=1 begin=0.5 end=0.75 dsp=1`.
  Its strict `<5` assertion was wrong, but the callback also incorrectly
  hard-stopped DSP samples at `end`. Upstream `superdough/sampler.mjs`
  uses `(end-begin)*playbackDuration` for the default envelope gate,
  not a source-buffer boundary. Native samples now continue through the
  release tail, stopping at the envelope end or the natural buffer end.
  Explicit `clip`, `loop`, or `release` use the hap duration. Legacy
  non-DSP slices still hard-stop. Loops play the prefix before loopBegin
  and remain looping during release, as WebAudio buffer sources do.
- `trigger_voice` now initializes the right-channel DSP for stereo
  samples. Previously only supersaw initialized it, so sampled right
  channels bypassed their effects. `tests/test_audio.c` loads a real tiny
  stereo WAV and covers both effect channels, slice/release, explicit
  gating, reverse playback, loops, and the legacy hard end.
- Relative bank roots previously left relative paths for
  `smp_acquire_abs`, which rejects them. `local_audio` now canonicalizes
  regular files to absolute paths and rejects symlink subdirectories
  as well as leaf symlinks. `tests/test_websamp.c` has an isolated offline
  fixture for manifest order, relative roots, rejected paths, uzu
  precedence, and user-map separation/override. No uzu audio downloads
  are made by these tests. The embedded uzu JSON was also compared with
  the current upstream `strudel.json`: all 16 names and slots matched.
- `tests/test_javascript.c` passes the query
  `s("misc:2").delay(.5).delaytime(1/3).gain(.4)`: `s="misc"`, `n=2`,
  `delay=0.5`, `delaytime=1/3`. The delay path was left unchanged.
  `delay` is `registerControl(['delay','delaytime','delayfeedback'])` in
  `js/node_modules/@strudel/core/controls.mjs`. `s` is
  `registerControl(['s','n','gain'], 'sound')`, so `misc:2` is a name plus
  `n`, not a sound string that still contains the colon.
- `make -j4 test` passes, including 31 examples / 1,207 reference events
  and native Csound checks. `python3 tests/test_tui_pty.py` passes all
  11 cases, including mouse, clipboard, startup vi mode, and signal cleanup.

The user restarted the UI. Read-only MCP verification afterwards showed
Belldub playing at BPM 120, with samples `hh=0`, `mt=1`, `lt=2`, `misc=3`,
and `ht=5` (sample IDs are process-local). Their decoded lengths are
15,439 / 20,387 / 39,059 / 9,567 / 18,207 frames respectively; toms
retain two channels. `misc` now has `dly=0.500`, gain .4, and step 1.
Bass cutoff 200 / Q 20, chord Q 1 with varying cutoff, fractional pitch
pairs, and bell begin .05 remain present. No fetch errors appeared in
the console. Source and playback were left untouched.

Next fidelity step is comparing the restarted engine's audio with the
browser. That comparison has not been completed; the MCP checks establish
sample/control routing, not waveform or listening parity. Do not restart
the process yourself.

Default map (covered by tests): side table in
`src/websamp.c` (`def_names`, not `web_names`). `ensure_defaults` loads
`assets/banks/Dirt-Samples/strudel.json`, `dough-samples/piano.json`, and
`dough-samples/tidal-drum-machines.json` via `./assets/banks`,
`TOMPOUCE_BANKS`, or the exe-relative banks dir, without
`banks_ensure_default`. Local regular files are stored as absolute paths
(`smp_acquire_abs`, symlinks and `..` refused). Embedded `UZU_JSON` is
applied after that and replaces overlapping keys. `hh` index 0 is
`https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/hh/10_hh_switchangel.wav`.
`misc` index 2 is `.../misc/12_misc_switchangel_ludens.wav`. `bd` index 0
is the uzu kick, not Dirt-Samples. User `samples()` is still checked
first; a failed user download does not fall through. Failed default URLs
set `DefSlot.failed` and are not retried. `DEF_NAMES` 2048, `DEF_SLOT_CAP`
1024, `DEF_TOTAL_SLOTS` 16384. Piano and drum-machine prefixes now use
installed catalogs; vcsl and mridangam remain outside this table.
`bank_find_dir` now calls `banks_dir_named`: exact path component, first
index hit, 32-entry cache keyed by `n_entries`. `resolve_sample` strips a
numeric `:n` before that fallback. `smp_acquire` on a directory still uses
alias matching, so it is not the strudel.json order; the default table is
what makes `misc:2` the third uzu file.

### Already done

- Stereo samples: `decode_wav` / `decode_mp3` / `decode_flac` in
  `src/sampler.c` keep an interleaved pair. `pa_callback` in `src/audio.c`
  plays both sides when `smp_channels == 2`. Do not "fix" this by downmixing.
- Pitch-keyed arrays and negative/fractional `n`: `select_slot` and
  `split_token` in `src/websamp.c`. `tests/test_websamp.c` is the spec.
- Default drum names live beside the user map, not in it. `samples()`
  still replaces only `web_names` (`websamp_name_count` stays the user
  count, including 0 when a relative path has no base). On a miss,
  `websamp_acquire` uses the default table: local
  `assets/banks/Dirt-Samples/strudel.json` when that file exists
  (absolute paths, offline), then the embedded uzu-drumkit map so bare
  `hh`, `mt`, `lt`, `ht`, `misc:2`, `bd`, and `sd` match current
  strudel.cc (`packages/repl/prebake.mjs` on branch main; uzu is the
  last `samples()` and wins those keys). `misc:2` is
  `misc/12_misc_switchangel_ludens.wav`. `hh:0` is
  `hh/10_hh_switchangel.wav`. A user `samples({ bd: ... })` keeps that
  `bd` and does not fall through when the download fails. Failed
  default URLs are not retried every hap. `bank_find_dir` is the
  offline fallback: it matches a path component exactly
  (`banks_dir_named`), so `hh` is `Dirt-Samples/hh` and `akailinn-hh`
  is not. Piano and drum-machine prefixes use the installed manifests;
  vcsl and mridangam are not in this table. `tests/test_websamp.c` passes,
  asserts the uzu URLs without
  downloading them, and exercises offline manifest order/precedence.
- Numeric `.csound()` and `.csoundm()`: `js/runtime.mjs` sets `csound` and
  `_csoundMidi`. `csound_program_schedule` in `src/csound_host.c` accepts a
  number or a name. `tests/test_javascript.c` covers `note(60.5).csoundm(1.25)`.

### Editor size and wrapping

Editor lines are independent malloc'd strings in `src/tui.c`. `current_code`
is a heap pointer (`current_code_set` in `src/track.c`). Song snapshots,
JavaScript compile, song files, and MCP `music_get_code` copy that text at
its real length. A failed allocation leaves the previous buffer in place.
Byte offsets are `int`, so one buffer stops near 2 GiB (`SOURCE_MAX`).
Linux overcommit can still kill the process. Do not allocate editor text
in the audio callback. MCP `tools/call` keeps its reply buffer on the heap
and grows it to fit `music_get_code`.

DSP state stays fixed: 128 voices, 12 orbits, and the reverb and delay
tables. `WEB_JSON_MAX` (2 MiB) caps the sample-map parser, not the editor.
QuickJS stays at 128 MiB, two seconds per compile, and 200 ms per query.
AI still sends only the first 4000 characters upstream.

Long lines wrap by default. `/wrap off` (also `on`, or no argument to
toggle) sidescrolls so the cursor stays on screen, and writes `"wrap"`
into the editor `config.json`. A missing key means wrap on. A config file
that does not end at EOF is replaced the next time `/wrap` saves. F3's vi
toggle persists `vi_mode`; vi sessions start in normal mode. Wrap tokens come from tree-sitter JavaScript
(`thirdparty/tree-sitter-javascript`, revision in `REVISION`, the same
commit Neovim's installed parser uses, ABI 15). A method dot stays with
the property (`note` / `.gain`). A string or comment wider than the pane
breaks on spaces, because mini-notation inside quotes is one JavaScript
string. One column stays blank before the right border. Up/Down, vi `j`/`k`,
the wheel, and PgUp/PgDn move by wrapped row. The hardware cursor uses that
same visual cell. Geometry is `editor_place` and `editor_content_rows`;
`tests/test_editor.c` covers a line past the old 2048-byte grid, more than
2048 lines, and wrap placement. The S-Lang draw still needs
`python3 tests/test_tui_pty.py` and a PTY.

Undo and redo keep full snapshots until `malloc` fails (`editor_undo`,
`editor_redo`). Ctrl+Z undoes, Ctrl+Y redoes, and vi normal `u` undoes.
A typing run (insert, delete, Enter) is one step until another key or a
two-second pause. Paste, load, AI, and an external `music_set_code` are
their own steps. Song auto-follow does not record; the track list already
holds those snapshots. Startup load does not record either.

### Orbits and ducking

Twelve buses, `N_ORBITS` in `include/event.h`. JS events default to orbit 1
(`orbit` or `o`). Legacy events leave orbit 0. Each orbit has its own
Schroeder reverb and delay line (`native_reverb`, `orbit_delay` in
`src/audio.c`). `.delay()` / `.delaytime()` / `.delayfeedback()` retune only
the sending orbit. The TUI `fx delay` / `fx verb` chain is a separate master
processor and does not share those lines.

`duckorbit` or `duck` is a number or a colon list such as `"1:5"`.
`duckattack` (aliases `duckatt`, `datt`) is the time to return to unity.
`duckdepth` is 0..1. The drop is immediate at the note onset inside
`trigger_voice`, which holds `voice_lock`. The callback copies the gains
out under that same lock. A depth of 1 silences the target orbit, including
its delay tail, until the attack elapses. Feedback inside the delay memory
is not scaled again.

### Browser comparison

Not bit-identical with WebAudio, and not promised to be. What is checked:

- Low/high-pass Q uses the WebAudio dB convention (`tests/test_dsp.c`).
- `dsp_envelope` breakpoints match the linear ADSR used for oscillators.
- `dsp_oscillator` saw (polyBLEP, discontinuity at half-cycle) is closer to
  the truncated Fourier series than a naive saw, at 440 Hz.
- `tests/test_audio.c` renders the real callback: stereo samples and
  effects, an orbit delay of 32 samples, a full duck of orbit 2, and
  forward/reverse sample slices with release and looping. All pass.
  Low/high-pass Q stays on the WebAudio dB curve; do not retune it.

A manual headless browser capture was completed; a maintained harness is
still not wired up. The 1,207 stored event references use Node/V8 with the
same host adapter, so they do not independently establish browser audio parity.
Do not shell out to a browser from the audio thread.

Portable recipe for recreating the manual comparison (helpers were in `/tmp`):

- Use a separate Chromium profile and a loopback-only debugging endpoint.
  Load actual strudel.cc and wait for `getSound`, `getSampleInfo`, and
  `renderPatternAudio`. Evaluate the same Arpoon pattern there and in the
  embedded runtime. The bundled transpiler is exposed at
  `__strudelModules['@strudel/transpiler'].transpiler`; its output is an async
  function body. The browser already supplies the preloaded catalogs.
- Query `pattern.queryArc(c, c + 1, {_cps: 0.5})`, retain piano onset haps,
  and compare whole arcs, notes, roots, and playback rates over 128 cycles.
  Browser sample selection uses
  `getSampleInfo(value, getSound('piano').data.samples)`. All 720 piano events
  matched in the manual check; no permanent 720-event browser fixture exists.
- Render only the piano using
  `renderPatternAudio(pattern.filterValues(v => v.s === 'piano'),
  0.5, 0, 4, 44100, 128, false, 'pitch-comparison')`. Capture the resulting
  WAV Blob in that scratch browser. Render the same four cycles (8 seconds)
  with the native callback, following `tests/test_audio.c`: query/convert
  haps, resolve samples/roots, trigger at the due frame, and run
  `pa_callback` without opening a PortAudio device. Preserve the song's
  explicit envelope, clip, filters, delay, and reverb parameters.
- Compare onset, duration, pitch and waveform alignment before judging
  effects. After the fix, the first 0.6 seconds had normalized waveform
  correlation about 0.976 at zero lag. This narrow observation is evidence,
  not a universal tolerance or proof of full-song/listening parity.

### Deferred feature backlog (after fidelity verification; separate scope)

1. Sample `stretch`: change duration without the same pitch change.
   `speed` already repitches. A stretcher has to run in the voice loop in
   `pa_callback`, with no allocation. Overlap-add state belongs on `Voice`.
2. Convolution: short impulse, partitioned or one FFT, also inside the
   callback. There is no impulse-response loader yet. Keep files inside the
   existing sample URL policy if you add one.
3. ZzFX, bytebeat, Kabelsalat. These are programs, not one control number.
   Compile them when the pattern is scheduled (`js_audio_schedule`), and
   play a buffer or a tiny opcode list in the callback. Do not call QuickJS
   from `pa_callback`. Kabelsalat's compiler is under
   `js/node_modules/@kabelsalat/` and is not in the audio bundle.
4. Imports. `globalThis.__import` in `js/runtime.mjs` only returns keys of
   the `modules` object (`@strudel/core`, `mini`, `tonal`, `transpiler`,
   `csound`). Adding a bundled package means editing that table and running
   `make js-bundle` (Node). Do not fetch remote JavaScript. A local loader,
   if added, has to stay under the config directory, reject `..`, and cap
   the file size. Rebuild `js/dist/runtime.inc` after any `runtime.mjs` edit;
   `src/javascript.o` depends on it.

### Build note

`make test` covers the C behavior. `python3 tests/test_tui_pty.py` needs a
PTY. PortAudio init may fail in a sandbox; `tests/test_audio` calls
`pa_callback` directly and does not need a device. The orbit reverb and
delay tables are large BSS (about 15 MB). That is intentional and static.
