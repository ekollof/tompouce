# JavaScript Strudel and native audio

Tompouce now embeds QuickJS and the upstream Strudel 1.2.6 `core`, `mini`,
`tonal`, and `transpiler` packages. JavaScript builds and queries patterns;
the native audio callback synthesizes and processes sound in C. Node.js is a
build tool for updating the bundled libraries, not a runtime dependency.

## Writing code

```js
setcpm(128 / 4)
const melody = n("0 2 4 <6 7>").scale('D:minor')
const brighten = p => p.hpf(180).lpf(sine.range(500, 5000).slow(4))
$: brighten(melody.s('supersaw')).fm(1.5).room(.3).pianoroll()
$: s("bd sd, hh*8").sometimes(x => x.ply(2)).crush("<16 8>")
```

The upstream implementation supplies pattern constructors, time transforms,
conditionals, arithmetic, alignment, scales, voicings, and signals. Variables,
closures, functions, arrays, loops, and async functions execute as JavaScript.
Double quoted strings and backticks use Strudel mini-notation; single quoted
strings remain ordinary strings. `$:` plays concurrent patterns. Named labels
also work; a leading or trailing underscore mutes a label. `all` and `each`
apply transforms to the collected patterns. Several statements can share a
line, and statements and strings can span editor lines.

`slider(value,min,max,step)` supplies a live control pattern. Register it with
F5 once, then use Ctrl+Left/Right or drag its chip. Changes update upcoming
note parameters without reevaluating source, resetting cycle phase, retriggering
held notes or applying unrelated editor changes. The first numeric argument
updates in the editor, so saving the song saves the performed value too.
Parsed source locations keep the controls attached after edits; comments and
strings cannot register controls. New controls require F5 before they are live.
Each successfully loaded program owns its bindings, so a failed evaluation
cannot replace the playing program's controls. F5 reads the source values again,
as in the web REPL. Direct calls create live widgets. Aliases such as
`const control=slider; control(.5)` and member calls return a fixed `pure()`
pattern and emit the web fallback warning.
For live arithmetic, use pattern operations such as `slider(700,200,3000,100).mul(2)`;
ordinary JavaScript numeric coercion of a control Pattern is not supported.
Top-level `setcpm`/`setcps` read numeric or Pattern arguments once at evaluation;
moving a slider inside `setcpm(slider(30))` requires F5 to change tempo. Use
`.cps(slider(...))` for query-time global tempo changes. A Pattern-valued
`.cpm(...)` transform uses current scheduler CPS; numeric `.cpm(...)` keeps
evaluation-time scaling, matching the web helper.

JavaScript playback uses the web scheduler's 50 ms query windows,
100 ms timer and overlap, and 100 ms output latency. Already submitted notes
keep their values; the next query reads the new control. This applies equally
to native audio, MIDI, OSC and Csound. External tempo changes pivot at the next
unqueried fractional cycle, and evaluation while playing preserves that clock and the
pending notes. Held native voices and effect tails continue. Csound evaluations
share one Csound renderer, preserving sounding instances and loaded definitions
across changed or omitted orchestra loads. Inline loads execute again on each
evaluation; URL loads are cached after compilation, including `github:` aliases.
Orchestra calls apply in order. Like the browser, an orchestra syntax error is
reported in the console without rejecting an otherwise valid musical pattern.
JavaScript exceptions, failed fetches and host failures still reject evaluation.

Event `.cps()` follows the current live web scheduler: tempo controls are sorted
before simultaneous notes, the change takes effect at its onset, and the rest
of the query window is recomputed at the new tempo. Zero tempo pauses cycle
advancement while sounding notes continue. As in the web REPL, a slider cannot
resume its own zero-tempo pattern because there are no advancing queries;
use an external BPM change or evaluate a positive `setcps`/`setcpm` value.
There is no hidden 20–400 BPM clamp. Negative or non-finite tempos are rejected.

Shared delay uses the web limit of one second and default feedback of .5.
Zero delay time or feedback suspends new sends and parameter updates while
existing shared tails continue. Parameter decisions use the web's current
Float32 AudioParam value at preparation time; a future return value can be
skipped when it already matches that current value. Accepted delay automation
survives source removal or voice stealing, including zero-gain events.
JavaScript delay taps interpolate fractional samples, including sub-sample
times. Feedback repeats include the web graph's additional 128-frame cycle
delay. Legacy direct-event delay keeps its original timing.

Stop ends new scheduling and resets the next start to cycle zero. Already
submitted notes and sounding envelopes/effect tails finish, as in the web REPL.
F5, Ctrl+R in insert mode and `:eval` evaluate the current editor source and start a stopped
transport only when evaluation succeeds. Ctrl+P and `/play` from stopped also
evaluate pending editor text before starting, matching web Play. Committed
notes survive Stop/evaluate/start; failed evaluation retains the prior pattern
and transport state. Companion Apply, Evaluate and Play from stopped use the
companion's current source with the same success-only activation. MCP
`music_set_code` and `music_evaluate` expose optional `autostart: true`; their
default remains false. Cold sound
preparation that misses its onset is skipped rather than bursting late notes
into the performance.

Native voices are prepared ahead of playback and wait for their scheduled
timestamp. Their source position and envelope remain frozen until onset;
cut groups, ducking and orbit delay settings take effect at that timestamp.
Offline callback tests check the exact first sample and chronological cut
ordering independently of voice-slot order.

The live pattern uses upstream `ref()`, matching the web REPL's slider
implementation. `live_controls_reference` compares 135 queries against the
pinned upstream implementation, including gain, pan, filter, arithmetic, masks,
speed, effects and long notes. It also compares the native scheduler's event
times, durations, pitches and gains against upstream Cyclist through live
slider, tempo and pattern changes. A test-only snapshot of the current web
scheduler additionally compares 30 finite native note schedules and 11 clock
states through simultaneous tempo controls, pause/resume and stop/start.
`cmake --build build --target test-controls-reference-web` verifies the snapshot
against the deployed strudel.cc scheduler in a disposable browser, including
failed evaluation and source lifetime on Stop. It never produces real audio.
The pattern library and its historical event baselines stay pinned.
`cmake --build build --target test-csound-reference-web` checks Csound lifecycle
against the deployed loaders and actual Csound WASM in private offline contexts,
including held PCM, redefined instruments, globals, syntax errors and URL caching.
Native tests also check exact note onset, idle-session retention and confinement
of the compiler thread. No orchestra compilation runs in the native audio callback.
The default step matches the web widget's
range divided by 1,000. These gates establish the tested control behavior;
they do not establish complete browser audio, device or synthesis parity.
Ordinary sliders do not continuously modulate already sounding voices in the
web REPL; adding that behavior here would require a separate feature.

Imports resolve the bundled modules:

```js
import { note as tone } from '@strudel/core'
const { scale } = await import('@strudel/tonal')
tone("c4 e4 g4").slow(2)
```

`@strudel/transpiler` is also available as a bundled module. Arbitrary npm
packages and remote module URLs are not resolved. Local ES modules can live in
`$XDG_CONFIG_HOME/tompouce/modules` (default
`~/.config/tompouce/modules`):

```js
import { melody } from './melody.mjs'
const { rhythm } = await import('local:rhythm.mjs')
stack(melody(), rhythm())
```

Local modules use ordinary JavaScript module syntax and can import bundled
packages or sibling `./` modules. Parent traversal, absolute paths, symlinks,
and files over 256 KiB are refused. Load modules during top-level evaluation;
new local module loading is disabled during pattern queries. Modules are
isolated to each compiled program and refreshed on reevaluation.
This is an embedded ECMAScript host, not Node.js or a browser: there
is no filesystem/process API, DOM, WebAudio, Hydra, or browser input-device API.
Additional JavaScript modules require an explicit addition to the bundle.

JavaScript Strudel is the only source language. The historical C mini/statement
parser and `bpm` / `drum` / `notes` / `| transform` line dialect have been removed.
Use `setcpm(BPM / 4)`, `s("bd sd")`, `note("c4 e4")` and JavaScript method chains.
Retired syntax reports an evaluation error and retains the last playable program.
Quoted mini-notation remains supported by the upstream Strudel runtime.

## Native audio coverage

The following resolved Strudel controls are consumed by the C backend; their
arguments can be patterns or signals because Strudel resolves them per event.
Aliases registered by upstream Strudel map to the same controls.

| Area | Implemented in C |
| --- | --- |
| Sources | sine, triangle, sawtooth, square/pulse, supersaw/unison/stereo spread, white/pink/brown noise, procedural `wt:` tables, local and remote samples, ZzFX, bytebeat, bounded Kabelsalat graphs |
| Harmonics | `partials`, `phases` (32 harmonics, with a Nyquist cutoff) |
| Amplitude | gain, velocity, amp, busgain, overgain, ADSR, clip/legato, duration, postgain, pan, dry |
| Filters | low/high/band-pass, their Q values and ADSR/depth envelopes, fanchor, 12 dB / 24 dB filters and nonlinear low-pass ladder, vowel/formant filtering |
| Filter LFOs | lp/hp/bp rate, sync, depth, depthfrequency, shape, dc, skew |
| Modulation | pitch ADSR/anchor/curve, vibrato, pulse-width rate/sweep, tremolo/sync/depth/skew/phase/shape |
| FM | eight operators with index, harmonicity, ADSR, envelope type, waveform, and operator-to-operator routing |
| Color/dynamics | coarse, crush, shape/shapevol, distortion/type/volume, compressor threshold/ratio/knee/attack/release |
| Spatial | chorus, phaser rate/depth/center/sweep, delay/send/time/sync/feedback, reverb/send/roomsize |
| Samples | sound/bank/index selection, pitch matching, forward/reverse speed, begin/end, looping/loop bounds, cut groups, unit `c`, upstream chop/slice/fit/loopAt transforms |
| MIDI | note events and midichan through the existing ALSA output |
| OSC | timestamped UDP `/dirt/play` through `.osc()` |

Native WAV, MP3 and FLAC sampling preserves stereo channels and recorded
levels. Pitch-keyed maps accept arrays within each region; sample indices
round and wrap with JavaScript semantics, including negative indices.
Region selection preserves fractional pitches. Arrays allow up to 1,024
slots per name, with an overall 8,192-slot map guard. Decoded samples have a
120-second duration guard and a 256 MiB/256-sample cache guard; exceeding a
decode/cache guard reports an error rather than silently truncating audio.
MP3 decoding honors Xing/Info encoder delay and end padding before
resampling, keeping note attacks aligned when samples are repitched.
Files without gapless metadata retain their decoded duration.

Sample starts preserve fractional source position and envelope time at the
first render frame, including fractional playback rates. Reverse stereo follows
the deployed web REPL's buffer-copy behavior: the right channel has a leading
zero and is displaced by one source frame. This applies before stretching too.

Native oscillators now use direct ADSR automation without the legacy
envelope smoothing stage. Explicit panning follows equal-power mono and stereo
folding conventions; omitted panning preserves unity upmix. Stock saw/square
levels match the browser periodic-wave normalization. Saw and pulse discontinuities
use polyBLEP corrections, and triangle starts at the sine phase. The maintained
browser comparison covers representative waveforms and effects; broader parameter
and full-song parity remains unverified.

Zero-gain events remain silent but still prepare their sources and apply shared
effect controls. Sample `cut` groups fade the previous sampled sources over
10 ms before their effects, retaining their original graph lifetimes. As in the
web REPL, oscillator sources ignore `cut`.

This is not complete WebAudio/SuperDirt sound parity. ZzFX, bytebeat and a bounded
Kabelsalat graph host are implemented natively (see below).
Sample pitch compensation and custom convolution impulses are now native
(see below). Separate orbits and
ducking are implemented. The native DSP algorithms are not promised to produce
bit-identical WebAudio output. `tests/test_dsp.c` compares the bandlimited saw
with its Fourier series and checks ADSR breakpoints. `tests/test_audio.c`
checks orbit delay time and duck depth.
It also checks sample release tails, explicit duration gating, reverse
slices, looping through release, and effects on both stereo channels.
### ZzFX, bytebeat and Kabelsalat

```js
note("c3 e3 g3").s('z_sawtooth').zrand(0).slide(.1).zdelay(.05)
note("c3 e3").s('bytebeat').bbexpr('t*(t>>15^t>>66)').bbst(0)
note("c3 e3 g3").K(sine(sFreq).mul(sGate.adsr(.001,.1,.5,.1)).out())
note(60).s('saw').K(audioin().fold(.5).out())
```

ZzFX supports `zzfx`, `z_sine`, `z_triangle`, `z_sawtooth`, `z_square`, `z_tan`
and `z_noise`, the 20-value `.zzfx([...])` parameter array and the corresponding
named controls. Its internal envelope is applied once; ordinary native effects
remain available. Random frequency variation is chosen during voice preparation.
Sounds are limited to 30 seconds, with at most two seconds of internal delay.

Bytebeat provides the 15 upstream presets (`s('bytebeat:N')` or `.n(N)`), custom
`bbexpr`/`byteBeatExpression` and `bbst`/`byteBeatStartTime`. Arithmetic, bitwise
operators, comparisons, logical/conditional expressions and standard numeric
Math functions compile to native operations with JavaScript 32-bit bitwise
semantics. Its startup follows the reference worklet's 128-frame quantum.
Assignments, loops, arrays, object access and arbitrary JavaScript functions
inside a bytebeat expression are rejected; expressions are at most 4,096 bytes.

`K(...)` uses the pinned Kabelsalat graph constructors, including Strudel `S(...)`
inputs and mini-notation placeholders. `sFreq` resolves per event and `sGate`
tracks its gate. `audioin()` effects process each input channel independently,
preserving stereo sample polarity. Sources and effects support stereo output
channels 0/1, arithmetic/math, ranges, oscillator/noise nodes, ADSR, filters,
lag/slide/slew, fold/distortion, clocks, triggers, sequences, hold and delay.
Feedback requires a delay; device input, MIDI/mouse nodes, custom UGens and
raw JavaScript nodes are unavailable. Unsupported nodes are rejected during
compilation. This is a native adaptation rather than a complete browser host.
Graph sources with ADSR nodes keep their own envelope, including release/delay
time, without a second outer ADSR. Sources without graph ADSR use the native
default envelope. Effects run after the source envelope; their lifetime is
currently bounded by the source voice, so browser `fxr` tail behavior is not
fully reproduced.

Programs have at most 256 operations and 16 inputs per operation. The cache
holds 256 programs and evicts unreferenced entries; queued events and sounding
voices keep their programs alive. Graph delay lines allow at most two seconds
per node and 262,144 total samples per voice. Native voice state is allocated
before taking the voice lock and has a shared 64 MiB budget. Neither preparation,
allocation, cache eviction nor JavaScript execution occurs in the callback.

### OSC output

```js
note("c4 e4 g4").s('superpiano').osc()
s("bd sd").oschost('127.0.0.1').oscport(57120).osc()
```

`.osc()` routes a pattern exclusively to native UDP OSC, defaulting to
127.0.0.1:57120. Each hap becomes a timestamped OSC bundle containing a
`/dirt/play` message with alternating control names and values, plus `cps`,
`cycle` and `delta`. Note-name strings become `midinote`; numeric `note` values
retain upstream OSC semantics. Bank names, room size and cycle-unit speed use
the upstream conversions. No browser/WebSocket bridge or Node process is needed.
Use another unmodified pattern branch if local audio is wanted too.

`oschost` accepts `localhost` or literal IPv4/IPv6 addresses; DNS names are
unavailable. Packets are bounded to 2,048 bytes, 64 primitive key/value pairs,
48-byte keys and 512-byte string values. Invalid addresses, complex values and
oversize packets are rejected with a console warning. Preparation runs on the
generator; nonblocking sends run on the consumer and follow transport stop/start.
With no local audio backend, a monotonic clock keeps external scheduling moving.

Unimplemented event controls are reported once per program in the console,
rather than silently claiming that the sound effect exists.

`samples(...)` arguments are evaluated by JavaScript, including variables and
computed maps, then handed to the existing C sample loader. Its URL checks,
cache, decoder limits, and lazy audio downloads still apply. Default sounds
use the installed Dirt-Samples `strudel.json` when available, overlaid with
the bundled uzu-drumkit manifest used by the current web player. Missing uzu
audio downloads lazily on first use; installed Dirt samples work offline.
Installed `dough-samples/piano.json` and `tidal-drum-machines.json` register
the default pitch-keyed piano and drum-machine banks too. Bare `.s('piano')`
selects the nearest recorded pitch, and `.bank('RolandTR909')` uses manifest
sample order. Explicit `samples()` maps take precedence. Without those
installed manifests, load the catalogs explicitly with `samples()` (the
`.piano()` helper also loads its map). VCSL and mridangam use the
installed `dough-samples/vcsl.json` and `mridangam.json`, with recordings under
`VCSL/` and `mrid/` at the bank root. These catalogs now register by default as
well; absent installed manifests still require explicit maps. The entire
browser catalog is not bundled.

## Pitch compensation and convolution

`stretch` follows the current upstream worklet's shift mapping: positive `x`
uses pitch factor `1+x`, negative `x` uses `max(0,1+x/4)`. For example,
`.speed(.5).stretch(1)` plays a sample at half speed while compensating its
pitch upward by an octave. Duration still follows sample transport and the
event envelope. Native samples now use the reference's 2048-frame phase vocoder,
128-frame hop, peak regions, phase correction and Float32 Hann overlap-add.
The sample envelope precedes the vocoder, and ordinary effects follow it.
Preparation accounts for the reference's 40 ms source anticipation and quantum
disconnect at the end; the musical event and highlight times stay unchanged.
Output begins at the musical onset, so earlier low-level precursors are omitted.

The generator prepares immutable stereo PCM with a 90-second input-lifetime
limit, a 128 MiB/128-buffer cache and eviction of unreferenced entries. Queued
events and voices retain their buffers. Preparation-limit failures report a
console error. New source/pitch variants require preparation; gain and ordinary
effect changes reuse the same PCM. No FFT, allocation, decoding or JavaScript
runs in the callback. The stretch cache lock is held across the overlap-add, so
two new stretch variants cannot prepare at once. A repeated source and pitch
reuses the cached PCM after a short lookup. A new pitch or stretch variant
misses and prepares again without decoding the sample a second time. Sample
decoding runs outside the sample cache lock. Reverse playback, slices, looping,
envelopes and source pitch modulation are included in preparation. Oscillators
retain their direct pitch-factor implementation rather than passing through a
vocoder. The preparation profile in
[audio comparison](audio-comparison.md) records cold-cache, live-edit and burst
timings for this path.

Maintained browser comparisons pass for half/fractional-speed compensation,
upward shifting, reverse playback, loops and envelope/filter ordering. Downward
shifting passes for shared PCM and eight independently decoded MP3 piano cases,
including three downward factors, C4/Fs3/Ds4 recordings, fractional speed and
fractional onsets. MP3 decoding retains float precision rather than rounding to
PCM16 before stretching. Prepared output begins on the first playback frame,
avoiding a one-frame delay at fractional onsets. The downward peak regions still
amplify small decoder differences; these selected gates establish neither full
decoder equivalence nor full-song waveform parity. See
[audio comparison](audio-comparison.md) for evidence and limits.

`ir` selects a registered sample as the orbit's convolution impulse:

```js
note(60).s('piano').ir("RolandTR909_sd").set({i:0}).room(.5).roomsize(.2)
```

`irbegin` sets the source offset and `irspeed` selects its signed playback
rate. Fractional positive rates play forward; negative rates play backward.
This correct signed-rate behavior differs from the current browser's
`adjustLength` bug for positive rates below one. `roomsize` limits the active
IR duration; the original buffer length participates in WebAudio-style RMS
normalization. The IR is prepared by the generator using existing sample
maps, URL policy and decoders. That preparation holds the sample-impulse cache
lock across kernel construction and its FFTs. The callback uses an immediate 128-tap head
plus 128-frame FFT partitions for its tail, preserving zero onset latency.
Stereo channels remain separate. Prepared impulse changes select the shared
convolver immediately and reset that orbit's tail on the next audio frame;
an earlier accepted note does not restore its old impulse at onset.

Limits: 139,264 active IR frames (about 3.16 seconds at 44.1 kHz) and 16 cached
IR/size/speed/offset combinations per process. Excess limits or failed IR
resolution reject the event and report a console warning; audio is not
silently truncated or replaced by another reverb. Kernels are immutable and
retained for the process lifetime.

The initial `roomsize` defaults to two seconds, matching the web REPL.
Omitted `roomsize`, `roomfade`, `roomlp` and `roomdim` controls retain the
last accepted settings while the orbit reuses its room, including across
evaluation and Stop/Play. Changing an explicit room parameter rebuilds the
impulse and restores defaults for omitted parameters, matching the web REPL.
Unchanged generated room settings reuse the impulse and preserve its tail.
Custom IRs also retain omitted speed and offset while reusing the same resolved
sample. Changing the sample, or an explicit room control, restores defaults for
omitted controls before preparing the replacement kernel.
An explicitly supplied IR speed or offset differs from an omitted one even
when its numeric value equals the default. Such rebuilds reset convolution
history even if the prepared PCM is already cached.

`room` without `ir` prepares a stereo convolution impulse on a dedicated worker,
with a bounded 32-request FIFO queue. Live scheduling does not wait for generation.
Generation holds the generated-IR cache lock across noise synthesis, the low-pass
and the kernel FFTs. That lock is separate from the stretch lock. The worker
still completes one generated request at a time, so several rooms do not overlap
with each other. The old convolver and its sounding tail remain active until a
completed kernel is published; that publication resets the old convolution
history on the next audio frame. An initial pending room has no wet output.
Accepted requests survive evaluation and Stop/Play. As on the web, an older
generated completion can replace an intervening custom IR. Multiple generated
requests complete in FIFO order; arbitrary browser worker completion races are
not reproduced.

The generated impulse follows the upstream
exponential decay, fade-in and gradual low-pass. `roomfade` defaults to .1,
`roomlp` to 15000 and `roomdim` to 1000. A fixed random seed makes native rooms
reproducible. These generated impulses have a separate 16-entry cache and fit
up to about 2.10 seconds of decay (the IR lasts 1.5 times the decay). Longer
rooms, zero size, exhausted caches, a full request queue or worker startup failure
retain the previous room and report a console diagnostic. Failed initial generation
leaves the wet output silent. Parameter equality guards retain the requested
settings even after failure; change a parameter to request another generation.
Explicit `ir` failures still
reject the event rather than silently substituting another impulse.

The browser capture harness checks dry piano, synths, envelopes, filters,
panning, ZzFX, bytebeat and seeded generated/custom IRs through the real callback;
see [audio comparison](audio-comparison.md). No-pan mono sources upmix at unity,
while explicit panning uses equal-power mono or stereo folding. Stock saw/square
levels follow the full-partial periodic-table normalization used by
[Chromium](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/blink/renderer/modules/webaudio/periodic_wave_handler.cc).

## Chromium companion

From the terminal UI, press F2, type `/companion`, and press Enter. This starts
the bridge, opens Chromium, and logs its URL. It reuses this engine's TCP MCP
port, or enables a private ephemeral loopback port for an ordinary UI launch.
Repeated commands reuse the running helper. `/companion stop` stops it, and
the helper also exits when the engine exits. A UI attached to stdio MCP needs
to be launched in a terminal TCP session instead.

The standalone bridge remains available:

```sh
python3 scripts/companion.py --open
# Optional fixed port or alternate MCP endpoint:
python3 scripts/companion.py --port 18990 --mcp-port 18789
```

Open the loopback URL printed by the command in Chromium. The companion
shows sounding voices, pitch, level and sample progress, the console and
engine health. It provides play/stop, tempo, reevaluation and a source editor.
The source editor uses locally bundled CodeMirror 6 with JavaScript syntax
highlighting, multiline paste, line numbers, bracket matching and undo.
Unchanged source polling preserves selection and history; in-flight polling
also preserves unapplied edits and their conflict-detection baseline. Strings
containing mini-notation are highlighted as JavaScript strings.
Voice telemetry targets 30 snapshots per second independently of the slower
status/console/source refresh. Canvas rendering uses animation frames and
extrapolates sample progress between snapshots; data older than 250 ms is
cleared. Hidden tabs reduce polling. The terminal's idle wait wakes directly
on MCP traffic, so telemetry does not wait for a 50 ms UI sleep per connection.
Edits stay in the tab until Apply; terminal-side changes prompt before
replacement, and polling never overwrites unapplied tab edits. Audio remains
in the native engine. This provides native session controls and activity
visuals, not Hydra or browser audio/input-device emulation.

The bridge binds only to 127.0.0.1, checks Host/Origin and a per-process token,
and exposes a fixed tool allowlist. It sends no data to external services.
Stop the bridge with Ctrl+C. It does not restart the engine. The source
replacement check detects changes observed before applying; it is not an
atomic compare-and-swap against simultaneous terminal edits.

The committed `web/dist/editor.js` and `web/dist/licenses.txt` let the
companion run without npm or CDN access. Rebuild them with
`cmake --build build --target companion-bundle` after editing `web/editor.mjs`; `web/package-lock.json`
pins the editor/build dependencies independently of the Strudel runtime.
Editor DOM regression checks run with `node tests/test_companion_editor.mjs`
after installing those development dependencies. Companion asset/CSP changes
require restarting only the bridge (`/companion stop`, then `/companion`).

## Live coding and visual feedback

Upstream event source locations feed the existing TUI sounding-token
highlighting, including locations in patterned control arguments. UTF-16
locations are converted to UTF-8 byte offsets before rendering. Highlight spans
are cached during scheduling, so drawing the editor does not execute user
JavaScript again. Adding, moving or changing a visualization preserves moving
note highlights: the generator maps the original literal offsets to the edited
source after checking that the musical syntax still matches. Comments and
whitespace can move those offsets; unapplied musical changes still suppress
highlighting. Patterned `.color()` changes active-token colors; `.markcss()`
supports underline, bold and italic terminal attributes (arbitrary browser CSS
is unavailable). Colors accept common names, hex, RGB(A) and HSL(A).

The native inline views are `scope` (also `tscope`), `pianoroll`, `punchcard`,
`spiral`, `pitchwheel`, `spectrum` and `wordfall`. Both `._name()` and `.name()`
render beneath the source line in the terminal. Kitty-capable terminals receive
pixel graphics; others receive the same geometry in braille, or ASCII without
UTF-8. Global transforms such as `all(punchcard)` also work.

Piano rolls, punchcard, wordfall, spiral and pitchwheel follow subsequent
transformations, including time changes after the visual call. Each view stays
isolated to its tagged branch. This intentionally adapts the browser's
capture-at-call piano roll to match terminal playback. Note views query held, recent
and upcoming logical haps, including silent patterns, rather than relying on
the audio-trigger history. The default window is four cycles with a centered
playhead. `cycles`, `playhead`, `vertical`, `labels`, `flipTime`, `flipValues`,
`fold`, `minMidi`, `maxMidi`, `autorange`, fill/stroke controls and colors are
supported. Wordfall defaults to vertical labelled notes. Drum sounds occupy
separate categorical rows. Overlapping same-pitch notes retain lanes and onset
markers where resolution permits.
Mixed pitched/percussion rolls use a single uniform lane layout; drum sample
indices do not affect the melodic auto-range. Horizontal rolls have note guides
on the left; vertical rolls have a keyboard/label rail across the top, with
low pitches on the left by default. Quiet notes keep their geometry in braille.
These orientation options apply to piano roll, punchcard and wordfall; scope,
spectrum, spiral and pitchwheel currently keep their fixed native geometry.

Spiral supports `stretch`, `steady`, `inset`, `padding`, `fade`, `logSpiral`,
size/thickness and playhead/color options. Pitchwheel preserves fractional
pitches and supports `edo`, `root` (frequency or note name), `circle`,
`hapcircles`, `hapRadius`, `margin` and `mode: 'flake'|'polygon'`.
Spectrum is a scrolling logarithmic-frequency spectrogram with native Hann FFT
analysis, 20 Hz–20 kHz guides, `min`/`max` dB range and `speed` (zero freezes).
At speed 1 its time span is four seconds. History fills continuously between
foreground frames; narrow low-frequency bands interpolate FFT magnitudes instead
of leaving empty stripes. Its default intensity gradient runs from the terminal's
blue through cyan, green and yellow to foreground, with quiet energy translucent
and silence transparent. Explicit `color`, `active`/`activeColor` or patterned
note colors retain a single hue. `thickness` sets a minimum history column width.
Scope uses the web renderer's fixed amplitude scale and thickness (`3`).
Inline `._scope()` defaults to `scale: 1` and `pos: 0.5`; `.scope()` defaults
to `scale: 0.25` and `pos: 0.75`, matching the web's distinct entry points.
Samples map to `(pos - sample * scale) * height`;
there is no automatic normalization. `scale: 0` draws a flat line and negative
scale inverts the trace. Alignment starts at the first falling crossing of
`-trigger`, preserving sample spacing rather than stretching the remaining
waveform. The default window is 4,096 samples (about 93 ms at 44.1 kHz).
Color and explicit scale, position, trigger and thickness options are supported.
The separate master/F12 and bottom status scopes retain their terminal layouts.
`node scripts/scope-reference-qa.mjs` checks coordinates captured from the
deployed web renderer in a disposable browser without opening audio devices.
Native raster checks cover the same trigger, scaling and spacing cases in
`test_terminal_graphics`; terminal rasterization remains an adaptation.

Browser page-background placement, custom canvas contexts, font families,
spiral cap styles and pitchwheel degree/int-label options are unavailable;
unknown options are reported once in the console. Native typography and
low-resolution braille are adaptations, not browser-pixel replicas.
Each scope now shows only the native voices from its attached pattern, after
voice effects, envelope, gain and pan. The tap precedes shared orbit reverb/delay,
ducking and master effects; these shared tails are not attributed to a pattern.
Csound's combined orchestra output has no isolated tap yet. Spectrum uses the
same isolated tap. Up to 32 visualization call sites are supported per program,
with one visible widget of each kind per source line and eight simultaneous
Kitty placements (additional widgets use braille). Drawing queries run on the
generator at up to 8 Hz, with a 50 ms budget and 4,096 total cached events;
the UI interpolates time at its display frame rate and runs FFT outside audio.
Drawing queries fill a bounded tile cache incrementally, starting near the
playhead, rather than querying an entire complex score within one budget.
Views fill outward during startup; cached tiles refresh as playback continues.
The cache holds at most 256 tiles/8,192 events, deduplicating held notes across
tile boundaries while retaining simultaneous identical notes. Waveform scopes
use only the audio tap and do not consume pattern-query time.
Excessive queries report a console error and retain the previous view.
Adding, moving, removing or changing a visual call on a running branch takes
effect without F5. Loading a song activates its views automatically. The generator
parses display requests and binds them to the existing program; it does not rerun
the song, change tempo, clear queued notes or reset playback. Comments, whitespace
and edits elsewhere in the buffer do not detach an unchanged branch. Literal
visual options update immediately; options that execute JavaScript still require
F5. Musical edits also require F5. A new or changed musical branch that cannot be
matched to the playing program shows “waiting for playing pattern”.
Branches carry bounded observation tags from compilation, so scopes can read
already sounding voices as soon as they are attached. Existing visual sites and
these automatic branch feeds share 32 tap slots; reaching the limit can require
F5 to attach another branch explicitly. Generation tags reject stale waveform data.
These are live views, not static sample-file previews. Comments and strings do not
create widgets. Widgets scroll with code but are skipped by editing, selection,
mouse placement and keyboard movement. UTF-8 terminals use 2×4 braille dots per
cell, with an ASCII fallback. `/inline off` hides them and `/inline on` restores
them; the preference persists. The bottom mini scope is suppressed while inline
widgets are present. F12 independently toggles the larger visualization pane.

A failed JavaScript evaluation retains the last playable program and reports
the error. The editor keeps the failed source for correction; it does not
highlight the old program against the new source. MCP evaluation tools return
an error result for the failed evaluation. Empty or comment-only JavaScript
buffers evaluate successfully as silent patterns.

Compilation and sample-map loading happen outside the track lock. JavaScript
queries and sample resolution also run outside that lock, in the generator.
Program references protect an in-flight query during reloads; queue generation
numbers reject events produced by an obsolete program. The audio callback
executes neither JavaScript nor file/network operations.

Limits remain: the editor source and each line grow until malloc fails
(byte offsets stop near 2 GiB), 128 current source slots plus up to 128 retiring
graphs, 12 effect orbits,
128 MiB per JS runtime, two seconds per compilation, 200 ms per pattern
query, 4,096 haps/events per query, 4 MiB query JSON, and 8,192 queued
audio events. Long lines wrap on JavaScript tokens; `/wrap off` scrolls sideways. The audio
callback does not allocate. `orbit` / `o` selects the delay and reverb bus (default 1).
`duckorbit` / `duck` selects target buses by number or array. Canonical numeric
strings and singleton nested arrays follow the web's target-key conversion;
colon lists and leading-zero strings do not select numeric orbits. Targets
that do not yet exist are skipped at preparation time. `duckattack` is the return time, defaulting to
0.1 seconds with a 0.002-second minimum; `duckdepth` sets the minimum gain to
`max(0.01, 1-sqrt(depth))`. Exponential ramps begin 10 ms before the note,
reach their minimum at the note plus `duckonset`, then return to unity.
Arrays of onset, attack and depth values map to the target array, reusing the
first value for omitted entries. Depth zero still cancels the previous ramp.
Replacement callbacks anchor to the last rendered sample's gain, as returned
by WebAudio's AudioParam.value, before cancelling and replacing automation.
That anchor is retained if the first replacement ramp fails; failure of the
return ramp retains the accepted downward ramp and its target.
Empty arrays and arrays whose first entry is undefined use onset zero and
depth one, but their attack becomes NaN before the orbit defaults apply: the
downward ramp is retained and the return ramp is rejected. Nested arrays retain
JavaScript string/number conversion, including null, booleans and empty arrays.
Numeric NaN and infinities retain their duck conversion behavior across the
native bridge: invalid ramp endpoints reject, positive infinite depth reaches
the minimum gain, and negative infinite attack uses the 0.002-second minimum.
Repeated targets retain their order and per-entry controls; each target entry
queues its own callback, including repeats of the same orbit.
Timer creation validates the note time minus 10 ms before accepting controls,
preparing shared effects or allocating a source. A negative/non-finite stop
time rejects the event at the first existing target and reports a distinct
timer diagnostic. The source orbit remains registered, missing targets are
skipped, and a stop time of exactly zero is accepted. Rejection leaves existing
gain automation, accepted callbacks and voices intact; later haps still run.
String and singleton nested-array onsets follow JavaScript concatenation when
forming ramp times. Invalid depth holds the cancellation gain; an invalid
return time holds the target of an accepted downward ramp. Rejected ramps
record their orbit, failing ramp and callback execution time. The scheduler
reports these to the console outside the audio callback, including while
stopped. Bursts retain the first 64 errors between drains and report overflow.
Long normalized onset strings use immutable numeric summaries prepared outside
the callback, without increasing the size of queued controls. The callback
combines bounded significant digits with the actual (possibly late) callback
time and attack, preserving concatenation and binary64 rounding. A separate
256-entry process-lived summary cache bounds memory; exhaustion rejects the
new control with a console diagnostic and retains existing references. Malformed
syntax uses a shared invalid summary. Embedded NULs in duck numeric strings are
encoded as NaN before JSON serialization, avoiding native string truncation.
Accepted duck callbacks use an independent bounded timeline, so voice stealing,
source preparation failure, Stop and program replacement retain them. Rest and
missing-source events can still duck another bus. Missing sounds stay silent
and report an error. As in Superdough, `clip(0)` falls back to full duration.
Csound and OSC output bypass local orbit creation and ducking. Csound also
bypasses ALSA note output even when `midichan` is present. Native `midichan`
without either output keeps its existing local-audio-plus-ALSA behavior;
the web's dominant `.midi()` output and combined output callbacks remain open.

## Building and testing

`.piano()` follows the upstream REPL helper: it selects the default piano
sample map, preserves explicit clipping, sets release to 0.1 seconds, and pans
by pitch. `loadCsound` (also `loadCSound` / `loadcsound`) accepts orchestra
strings and template literals; `loadOrc` accepts public HTTP(S) or `github:`
URLs. `.csound('Instrument')` and `.csound(1.25)` route named and numeric
instrument instances to the native Csound renderer. `.csoundm(...)` uses MIDI
key/velocity pfields and preserves fractional keys.
These functions can also be imported from `@strudel/csound`.

Csound rendering runs in an isolated Linux process with bounded memory and
execution time. After trusted engine initialization it cannot open files,
create processes, or access the network; `loadOrc` fetching happens in the
parent using the existing URL policy. The audio callback consumes buffered
stereo PCM without IPC, allocation, or Csound calls. Failed fetching or host
initialization keeps the previous playable program; orchestra syntax diagnostics
retain existing definitions while allowing the new musical pattern, as above.
The compiler thread has the same confinement as the renderer. Up to 16 loads
per evaluation, 2 MiB of combined source per update and 64 cached orchestra URLs
per Csound session are accepted. Named instruments receive frequency,
gain, and the slash-separated Strudel control map as p4/p5/p6, matching the
upstream bridge.

Current `livecode.orc` uses Csound 7 syntax. Install the pinned native engine
on Linux x86_64 with `bash scripts/install-csound7.sh`, then rebuild. It lives
under `~/.local/opt/tompouce-csound7`; pass `-DCSOUND_PREFIX=/path` when configuring
to use another installation. Csound 6 headers/libraries are also supported
for older orchestras. Without either dependency, ordinary Strudel playback
still builds and Csound evaluation reports the missing dependency.

`ctest --preset default` includes all 31 published examples, comparing 1,207 events across
three cycles with committed Node/V8 reference results. It also renders the
two Csound examples using the native backend. See
[fixture provenance and update instructions](../tests/fixtures/README.md).

Plain sample arrays use MIDI 36 as their playback root, matching Superdough;
explicit note-keyed maps retain their declared roots. Decoding preserves the
recorded levels rather than peak-normalizing each file. Sample amplitude ADSR
defaults differ from oscillator defaults, and note duration gates a sample only
when `clip`, `loop`, or `release` is specified. Lowpass/highpass resonance uses
WebAudio's decibel Q convention; bandpass Q is linear.
For ordinary samples, `begin` sets the playback offset and `end` sets the
default envelope gate. Playback continues through the release tail until
the envelope or buffer ends. Explicit clipping, looping, or release uses
the pattern event's duration for that gate.

Normal builds use the committed bundle and vendored QuickJS sources:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
python3 tests/test_tui_pty.py
```

Updating the JavaScript bundle requires Node.js/npm and package downloads:

```sh
cmake --build build --target js-bundle
cmake --preset default
cmake --build --preset default
```

`js/package-lock.json` pins dependencies. `js/runtime.mjs` contains the native
host adapter and `js/build.mjs` produces the bundle and third-party notices.
QuickJS 2026-06-04 is vendored under its MIT license. The upstream Strudel
packages use AGPL-3.0-or-later; see `thirdparty/STRUDEL-LICENSE` and
`js/dist/licenses.txt` for the bundled dependency notices and licenses.

References: [Strudel REPL pipeline](https://strudel.cc/technical-manual/repl/),
[audio effects](https://strudel.cc/learn/effects/),
[time modifiers](https://strudel.cc/learn/time-modifiers/), and
[QuickJS](https://bellard.org/quickjs/). Audio implementation references:
[upstream pitch-shift worklet](https://codeberg.org/uzu/strudel/raw/branch/main/packages/superdough/worklets.mjs),
[upstream custom IR handling](https://codeberg.org/uzu/strudel/raw/branch/main/packages/superdough/reverb.mjs),
and [WebAudio convolution normalization](https://www.w3.org/TR/webaudio-1.0/#impulse-response-normalization).
