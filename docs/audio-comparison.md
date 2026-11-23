# Browser/native audio comparison

The [V8 suitability assessment](v8-assessment.md) uses the same event-comparison
harness with a diagnostic V8 renderer. It matches all 41 musical Bakery cases,
including the two native QuickJS timeouts; this does not change the production
backend or establish audio/host-bridge parity.

## Reference bootstrap and historical legato notes

The Node live-control runner retains an independent JavaScript signal path:
without `__hostSignal`, native signal installation leaves upstream prototypes
and signal objects untouched. Regression checks include triangle/inverted and
bipolar queries across negative and fractional arcs, followed by 135 live-control
comparisons and 395 exact clock-fraction conversions.

The deployed REPL confirms that the `Random bells` bass plays D2 in query
cycles 0 and 1, and G2 in cycle 16. The historical pinned fixture records F2
for those first two notes because its legato rotation also rotated values.
The fixture remains unchanged. The example test asserts those two original F2
values before comparing against the exact current D2 contract; it requires
exactly two such differences, with all other event fields and ordering checked
against the historical fixture. `scripts/controls-reference-qa.mjs` independently
gates the three bass queries against strudel.cc without audio playback. The
production structure-only legato adapter needed no change.

## Bakery composition corpus

### Composition-query profiling

`node scripts/profile-pattern-query.mjs [before-binary] [after-binary]` runs
fresh isolated reference workers, alternating binaries for each repetition.
`TOMPOUCE_PROFILE_RUNS` selects 1–20 repetitions (default 5). It checks the
original Bakery source hashes and measures one `queryArc(0,1,{_cps:.5})`
inside the existing compile budget with `Date.now()`. This is a wall-time
probe, not a passing 200 ms query gate; it does not raise either budget.

Five paired runs measured median 287→268 ms for 13587 and 282→268 ms for
78179, all with 539 haps. Baseline ranges were 281–293 / 280–284 ms; optimized
ranges were 267–271 / 266–271 ms. The retained fast paths fuse ordinary pure
cycle-query construction and enumerate common control-record keys once rather
than rebuilding the right-side key array for each left-side key. They preserve
fresh objects, key order and callback behavior, with bounded native work and
fallbacks for custom prototypes, accessors and changed builtins. Pinned package
files and locks remain unchanged; bundle generation fails if either guarded
source hook no longer matches. The fused application/intersection prototype
showed no consistent gain and was discarded. Coarse instrumented timings put
context merging near 11 ms, value transformations near 91 ms (including nested
callbacks), and span transformations near 26 ms; those instrumented timings
are investigative evidence, not exclusive CPU attribution.

The fresh Bakery comparison still matches 39 musical compositions and the empty
entry. Both heavy songs still fail at the unchanged 200 ms limit. The bounded
optimizations therefore improve work by about 5–7% in these paired probes but
do not resolve the budget issue or establish full-song PCM parity.

### Where the remaining query time goes

The two failing compositions were re-instrumented to decide what to optimize
next. All numbers are compile-stage wall-clock probes on the same host, not
gates.

- **Scaling is per hap.** Querying the same pattern over growing spans gives
  40 ms/77 haps, 87/186, 163/364, 273/539 and 593/1248: about 0.47–0.49 ms per
  hap plus roughly 5 ms of fixed work. Any fix has to reduce per-hap cost.
- **Hap allocation is not the cost.** Replacing the three copy operations with
  in-place mutation (incorrect output, timing only) leaves the query at
  264–272 ms versus 267–270 ms baseline. Rebuilding the same copies through
  `Object.create` instead of `new Hap` is slower (289 ms). Freed copies buy
  nothing.
- **Fusing consecutive value transforms is not the lever.** Composing adjacent
  `withValue` layers into one hap produced byte-identical values but ran
  277–284 ms, slower than the 270 ms baseline, because the layers are not
  adjacent in this graph.
- **The dominant operations are dispatch, not arithmetic.** Per hap the query
  performs about 38 value transformations, 21 span transformations, 58 context
  copies and 18 context merges. Instrumented per-call totals overstate them,
  since ~45,000 timed calls add about 88 ms of `Date.now` overhead; allocation
  and arithmetic inside those calls are cheap, so the cost is JavaScript call
  depth through the pinned combinator graph.
- **Host crossings are minor.** One query makes about 17,000 native fraction
  creations (240 ns each), 8,300 pure-query calls, 4,100 common-key scans
  (520 ns) and 456 triangle batches: roughly 10 ms in total.

With a worker backend both heavy songs finish the compile-stage probe well
under the budget, and the full Bakery comparison matches **all 42
compositions**, including 398/398 events for 13587 and 78179, where QuickJS
times out. The Node diagnostic backend measured 128/129 ms medians and the
native V8 worker 139/140 ms; with no backend env the native worker is preferred
when installed. See the [V8 assessment](v8-assessment.md) for details. No
threshold or baseline was changed, and `TOMPOUCE_QUERY_BACKEND=quickjs` keeps
the original behavior.

These event gates do not measure playback continuity. "Computer says no" is a
runtime stress case: an isolated daemon playing IDs 13587 and 78179 reaches
**124–128/128 active voices** on QuickJS, Node and native V8 alike, with the
same near-cap peaks on every backend. `music_health` now reports PipeWire xrun
gaps; over 48 s runs the gap counts varied from 59 to 330 and max gaps were
about 7–10 ms, with no consistent backend difference and only one late-preparation
skip. Changing `--block` from 128 to 256 frames did not reliably reduce gaps
(81/330 versus 118/218 in repeated Node runs). An isolated experiment doubling
`MAX_VOICES` from 128 to 256 also did not help — 48-second Node runs gave 263,
497 and 832 gaps versus roughly 81–330 at the default limit, with voices still
peaking around 156. So this is not a simple buffer-size or voice-count problem:
the single real-time callback and its per-block render cost are the bottleneck,
whereas the browser's WebAudio graph has look-ahead scheduling and a different
voice/buffer architecture. Voice-pool policy and per-block render profiling for
this composition remain open diagnostics. A compile-time
`-DTOMPOUCE_RENDER_PROFILE` build exposes render/section/DSP/hot-path counters
through `music_health`. Measured over a 48-second Node-backed run: average
callback render ~1.5–1.7 ms with peaks ~3.9–4.2 ms against a 2.9 ms block
deadline; the voice loop is roughly 85% of render time, while `dsp_process` and
`dsp_envelope` are negligible. Of the hot path, `route_voice` costs about 91
cycles per call versus ~29 for `source_value`, and is called roughly nine times
more often, so routing/mixing is the first target for a parity-preserving
optimization. A follow-up change froze pan/send constants into preallocated
`Voice` fields at note-on and replayed the original multiplication order;
`ctest` (46/46), the nested-FX gates and the Bakery corpus still pass, but
repeated 48-second runs still average ~1.63–1.69 ms render with hundreds of
over-budget callbacks, so this refactor did not materially change xrun gaps.
Further gains need a different hot-path strategy, not just hoisting constants.
One such step removed the per-sample routing cache validation: every routing
input is frozen at note-on, so the callback only needs a `route_cached` check.
On a 15-second "Computer says no" PipeWire run the profile build moved from
~1.63 ms to **1.40 ms** average render, from ~29% to **8.4%** of callbacks over
2 ms, and from ~5.1 ms to **3.6 ms** maximum. A follow-up two-lane SSE2 path
computes the adjacent left/right pan, dry, delay and room sends together; it
keeps per-lane operation order and leaves scope/named-target voices scalar.
Measured route cost moved from ~62 to ~60 cycles per call and average render
to ~1.38 ms, so this is incremental rather than structural. Common un-panned
voices now skip exact `*1` side/dry multiplies, and voices with DSP disabled
skip the `dsp_process` calls entirely. Over a 35-second stress run the maximum
callback is still about 3.0 ms against the 2.9 ms PipeWire deadline, so rare
xruns remain; eliminating them needs the block/SoA voice renderer rather than
more scalar micro-optimizations. The callback now has a four-frame voice/effect
split and can stage routing through `src/render_block.c`; the staged route is
PCM-exact but measured neutral, so it is a foundation for blocking source and
DSP work, not yet a performance win. Release LTO is a separate, measured
build-level improvement: on the same 35-second profile run it reduced average
callback render from ~1.63 ms to ~1.59 ms and over-3-ms callbacks to one.
`-DWITH_LTO=OFF` restores the previous build. The four-voice sample SoA
experiments were reverted; see `TODO.md` for their measurements.

The from-allocation SoA bank now owns most of this composition. Qualifying
sample and sine/saw/square/triangle voices copy a compact lane once, at the
next four-frame block boundary after onset, and the lane then owns source
position, ADSR, oscillator phase, the single active biquad (low-, high- or
band-pass, including LPE/LFO coefficient updates) and routing constants.
Voices with no active DSP use an identity biquad, which reproduces the scalar
bypass and clamp exactly. Pattern scopes are accumulated from the staged
routing taps, so scope voices no longer disable block staging. The bank is
sized to the full primary pool (128 lanes), so dense passages no longer deny
qualifying voices. On the 128-frame PipeWire stress run about **86 lanes per
block** are active and no primary triangle or sample voice remains scalar;
repeated 35-second runs measure **~1.19-1.20 ms average** callback render with
a **2.30-2.86 ms maximum**, no callbacks over 3 ms and **zero PipeWire gaps**.
An earlier 53 ms outlier was not callback work: the maximum `voice_lock` wait
stayed at 8-22 us across the same runs. Retiring a bank voice copies its
current lane state first, and the browser retirement envelope/cut suite (62
cases) plus the vowel impulse suite still pass.

The two-lane stereo biquad kernel is byte-exact against C and 1.21-1.28x
faster in a 200,000-frame batch. A persistent per-voice layout now keeps
coefficients and state across samples, so stereo samples the SoA bank cannot
admit (loops and fallback voices) and supersaw use it in the callback. A
256-block benchmark measured 1.02-1.04x over the scalar path; this is a
fallback-path gain, not a whole-callback speedup. Forced `TOMPOUCE_SIMD=c`
and `avx2` runs pass the audio gates.
A stereo sample-interpolation AVX2 kernel was measured and rejected: it is
byte-exact against C but 5,000,000 interpolations took 19.3 ms versus 14.7 ms
for C on this box. The scalar sample lookup remains in the audio path.
Supersaw voices now
use from-allocation lanes that point at the voice's oscillator state and reuse
the AVX2 kernel; the source/envelope clock and stereo biquad state stay in the
lane. The procedural HH drum (~1.3 voices/block) remains scalar after a
measurement: its callback costs ~4.85 us per voice per 256 frames, or about
0.1 us per four-frame block at that density, while lane admission would have
to preserve a shared `rand()`/high-pass stream in slot order. Vowel, phaser,
chorus and distortion are not used by this composition and still render
scalar.

The audio/reference gates and the supersaw exactness test pass; full ctest
includes the previously timing-sensitive `tui_pty` vi startup check.
The earlier OpenAL **zero-underrun** reading was a blind spot: it only counted
an empty app queue. A source-state check now also counts OpenAL stopping and
restarting while buffers are still queued. With the dense "Computer says no"
source at 128 voices, `-DTOMPOUCE_RENDER_PROFILE` reports average callback
render ~1.4–3.2 ms and peaks of 4.2–6.6 ms, so the 128-frame queues (2.9 ms per
buffer) are too shallow: 128×2/×3/×4 stopped the source 21–59 times in eight
seconds. The 256-frame queues cover the spikes; 256×2, 256×3 and 512×2 measured
**zero source stops**. The default is 256×2 (about 11.6 ms total), where
PipeWire's profiler showed zero xruns for the engine, sink and driver over an
18-second run. An app-specific `alsoft.conf` sets `period_size = 64`,
`periods = 2` and `channels = stereo`; the resulting PipeWire node reports
64/48000 latency. OpenAL remains **opt-in** via
`TOMPOUCE_AUDIO_BACKEND=OpenAL` (PipeWire stays the automatic default).
`TOMPOUCE_OPENAL_FRAMES` (64–1024), `TOMPOUCE_OPENAL_BUFFERS` (2–8) and the TUI
`Ctrl+-`/`Ctrl+=` presets tune the queue; F4 shows the queue, `xrun=` and
whether the render thread has real-time scheduling. `music_health` reports the
full `calls/gaps/empty/max/minq/rt` breakdown. The OpenAL feeder asks RTKit for
priority 10 (direct `SCHED_FIFO` fallback); `ps -L` then shows
`tompouce-audio` as `FF 10`. PipeWire runs the stream callback on its
`data-loop.0` thread, which is already real-time (`SCHED_FIFO`, priority 83)
with `SCHED_RESET_ON_FORK`; F4 masks that flag and reports `rt=on`.

Real-time priority alone does not make OpenAL match the native callback. With
the full TUI and the 128-voice stress source, the OpenAL audio clock advanced
0.91× wall before RT and 0.93× after, while the PipeWire callback advanced
**1.00× wall with zero gaps**. OpenAL's feeder blocks inside its own
device/queue, so CPU priority cannot recover the lost time. PipeWire is the
recommended backend for CPU-heavy songs; OpenAL remains useful for its
independent queue on systems where the native path xruns.
`TOMPOUCE_AUDIO_BACKEND=PipeWire` remains the direct path.

Strudel's WebAudio output has no master limiter, only orbit gain 1 and the
destination. Our live path previously used a tanh/clamp master stage; both
colored dense mixes. A new diagnostic `bass-fugue-full-mix` case renders **all**
events (empty sound filter) on both sides. It matches the deployed REPL at
correlation **0.999997**, RMS ratio **1.000014**, with native and browser peaks
both at 1.0. Strudel therefore also reaches the destination ceiling on this
mix; the old clamp was not the source of the perceived distortion. Live output
now uses the same unity-gain reference path as the gates, and
`music_health` reports `ref=`, `peak=` and backend underruns. This is the first
full-mix level gate; it does not establish PCM parity for every Bakery song.

Reaching the unchanged 200 ms budget on QuickJS therefore needs about 25% less
per-hap JavaScript work, not cheaper haps. The options are batching the hot
`Hap`/`Pattern` operations into a native applicator that still invokes the
JavaScript callbacks in order, reducing combinator depth only where semantics
allow it, or the scoped V8 migration. The earlier fused application prototype
and this allocation work show that local micro-optimizations inside the
existing call structure do not move the number.

`cmake --build build --target profile-bakery-query` runs this compile-stage
probe reproducibly against the built `test_javascript` (set
`TOMPOUCE_PROFILE_RUNS` for the run count). It prints median wall milliseconds
and hap counts for both heavy IDs and is explicitly not a 200 ms gate.

A build-time prototype replaced the pinned `withValue` map loop with a native
host function that walks the array and invokes the same JavaScript callback per
hap in order. Median probes went from 267/267 ms to 271/270 ms, so the hook was
reverted; QuickJS's built-in `Array.map` is already the faster loop. A second
instrumentation found that `Pattern.query` is an own-property closure created by
each combinator, with no repeated top-level `queryArc` call to cache. A working
native applicator would therefore have to restructure how combinators register
their stages in the pinned runtime, not merely wrap their map calls.

A CPU-affinity check ruled out scheduling noise: three-run medians were
277/275 ms unpinned and 280/276 ms pinned to a single CPU with `taskset -c 0`.
The 200 ms gap is genuine CPU work, not renderer contention.

A further owned instrumented bundle counted inner pattern queries: 14,090 calls
with 6,764 distinct (pattern id, begin, end) keys and 2,521 duplicates (up to
20 repeats). A bounded result cache removed 1,058 calls but raised medians to
300–337 ms, because string-key and map bookkeeping costs more than the saved
calls. Both prototypes were reverted; the bundle is byte-identical to the
committed one.

### Corpus and earlier measurements

`tests/fixtures/strudel-bakery.json` preserves the 42 compositions shown on the
first Featured and Last Creations pages of <https://strudel.cc/bakery/> on
2026-10-27 (21 entries each). Original source, authorship comments, share URLs,
Bakery IDs and SHA-256 hashes are retained. Similarly titled versions remain
separate; this is a dated snapshot, not the Bakery's entire historical archive.
Composition rights remain with the original authors.

Use the existing comparison harness to query all branches for four cycles:

```sh
cmake --build build --target render_audio
TOMPOUCE_COMPARE_FILTER=bakery- node scripts/compare-audio.mjs /tmp/tompouce-bakery
```

These cases use the renderer's `eventsOnly` mode, which skips audio/sample
preparation and writes events and effective tempo instead of PCM. The browser
uses the deployed REPL with a stopped, owned offline clock, including its label,
stack and tempo behavior. Neither side opens an audio device. Original sources
are not rewritten, branches are not filtered, and execution budgets are unchanged.
The native process uses isolated configuration/cache roots. Browser sample-map
imports still require network access.

The report distinguishes event matches, differences, empty patterns and native
or browser failures; a failed case does not prevent later compositions from
running. Any difference/failure produces a nonzero exit. Empty patterns are
reported separately from musical coverage. Event values use the existing
example suite's relative numeric tolerance of 1e-7. Existing PCM cases retain
their thresholds and behavior. This corpus establishes no full-song PCM claim.

The initial run recorded 29 event matches, nine differences, three native
failures and one comments-only empty pattern. Differences: IDs 13794, 13763,
13622, 13366, 13294, 13267, 105781, 78185 and 32267. IDs 13587 and 78179 hit
the existing 200 ms query CPU budget; ID 112818 requires `initHydra`. No runtime
fixes or budget changes were made as part of adding the corpus. Captures and
errors are recorded per case in the requested output directory, including a
complete `report.json`; results can change with the hosted REPL.

A later native pass matched those saved browser events for all 39 compositions
that captured them, including the nine former differences. Coastline (78185)
now returns the same 10 events at 0.75 cps. Scope masking, structure-only
legato, root/above voicing and the native fraction clock are in that renderer.
IDs 13587 and 78179 still stop on the first cycle at the unchanged 200 ms CPU
budget. ID 112818 still needs `initHydra`. This pass did not reload strudel.cc.

The fraction dispatcher now uses native results only on successful integer
operations and falls back to fraction.js on overflow or invalid arithmetic.
Regression checks cover division/modulo by zero, results beyond int64 and LCM
with shared numerator factors. Voicing reset also resets the adapter's default
dictionary. `voicing-reset-default` independently compares the five restored
C-major notes with the deployed REPL after selecting and resetting `lefthand`.

Triangle and inverted-triangle fastcat controls, including numeric fast, slow,
early, late, and range, are now sampled in C. Uniform saw, sine, square, and
noise signals stay in the pattern engine; sampling those one value at a time
in C made coastline miss the same deadline. Against the saved 2026-10-27
browser events, all 39 captured compositions still match, including coastline's
10 events at 0.75 cps. This check did not reload strudel.cc. One-cycle
`queryArc` during compilation, outside the 200 ms query entry, measured about
480–580 ms and 539 haps for both IDs. Their fourth patterns alone measured
about 430–570 ms, both still 456 haps. Neither finishes inside the unchanged
query budget. Coastline remains close to that wall deadline: on a machine
with load average about 3, some four-cycle renders were interrupted at 200 ms
while the query thread's CPU time stayed under it. ID 112818 still needs
`initHydra`.

Further clock optimization adds native fraction cloning, cycle helpers,
TimeSpan intersection/splitting and callback-based transformations, and Hap
value/span transformations. Triangle clock splitting and Hap construction are
now fused into one bounded native query instead of crossing the host boundary
once per raw sample batch and then assembling the fragments in JavaScript.
Numeric parsing, general pattern functions and uniform waveforms stay in JS.
Native triangle output is checked directly against the JS fallback, including
negative times, fractional rates, shifts and signed zero. Callback mutations,
exceptions, point intersections, reversed spans and wide-integer fallback have
focused regression checks.

Isolated full-cycle compile-stage probes for 13587 measured roughly 375–425 ms
before these changes and 241–279 ms afterward, with the same 539 haps. These
probes use the existing compilation budget, not a raised query budget, and are
profiling evidence only. Both original compositions still fail the unchanged
200 ms query gate. The remaining pattern application work includes 20,791 Hap
value transformations, 11,370 span transformations and 9,490 context merges per
first cycle; native arithmetic alone has not resolved the query deadline.
The fresh 301-case strudel.cc comparison kept all 256 gated PCM cases passing,
all 39 musical Bakery compositions that finish querying matching, the empty
entry empty, and the five-event voicing-reset check matching. The two query
timeouts remain failures. The two ungated PCM diagnostics remain ungated
(fractional room switch correlation 0.989538; Arpoon opening 0.945504).

An audio-only `initHydra` placeholder now accepts the original Hydra Bakery
composition (112818) without rewriting its source. Its four-cycle event
comparison matches all 112 events from the deployed REPL. Visual graphs,
buffer capture and visual callbacks are inert; a once-per-evaluation console
message makes that limitation explicit. This establishes musical event
compatibility for this composition, not Hydra rendering support.

Thirteen `fm-matrix-` gates check selected sine-modulator chains, parallel and
cross sends, explicit carrier sends, disconnected operators, operator 8,
negative indices/harmonicity, numbered envelopes and self/two-node feedback.
The first six previously failed with correlations 0.073–0.981. Matrix sends
now enter frequency inputs, numbered operators chain into the preceding stage,
and only carrier-connected operators render. Fixed voice-owned 128-frame buffers
preserve dependency ordering, feedback history and Float32 gain stages. The
thirteen corrected comparisons exceed 0.9999991 correlation with RMS ratios
1.00105–1.00128 at zero lag, under unchanged gates.

Nine `fm-adsr-` gates cover attack/decay/sustain/release defaults, explicit
linear/exponential curves, short attack/decay gates and zero-length segments.
FM now shares the existing AudioParam endpoint scheduler with pitch automation,
preserving exponential zero endpoints and arithmetic clipping at short gates.
Missing segments use the REPL's minima and decay-dependent sustain default;
the FM curve defaults to exponential. Correlations exceed 0.9999996 with RMS
ratios 1.00072–1.00113 at zero lag, under unchanged gates. The former decay-only
render failed at 0.9192 correlation. Selected operator-matrix behavior is covered
separately above.

Five `fm-frequency-` gates cover basic harmonicity, static detune, carrier
pitch envelopes, vibrato and fractional onset. Initial correlations ranged
from -0.016 to 0.865; corrected renders exceed 0.999998 with RMS ratios
1.00106–1.00135 at zero lag. FM depth scales by modulator frequency, default
modulators have no amplitude envelope, carrier pitch automation does not change
their frequency, and their first a-rate block follows oscillator onset
alignment. Explicit FM ADSR and selected matrix routing are covered separately
above.

Eight `fm-wave-` gates cover triangle, square, sawtooth, a numbered sawtooth
operator, a square envelope and white/pink/brown modulators. Triangle, square
and sawtooth use the polyBLEP oscillator. Noise modulators loop the same
two-second buffers as source noise and ignore frequency sends. Correlations are
0.99074–0.9999996 with RMS ratios 0.94389–1.00847 at zero lag. Square is the
low end of that range.

Twelve `fm-noise-onset-` gates combine white/pink/brown operators with explicit
FM envelopes in direct and chained routing, including aligned, half-quantum
and fractional starts. Noise BufferSources stay out of the oscillator-only
silent-head phase correction; their sample cursors are consumed by the regular
quantum. Their envelope retains the complete onset quantum, including the
GainNode's default gain before its first scheduled event. These twelve
cases exceed 0.9999987 correlation with RMS ratios 1.00095–1.00126 at zero lag
under unchanged 0.99/0.85–1.15 gates. The original white and pink fractional
cases failed at 0.97602 and 0.90991. These selected cases do not establish
arbitrary noise-matrix or full-song parity.

Six `fm-onset-` gates combine FM envelopes with late onsets, including a
numbered operator, a constant-gain chain, a quantum-aligned envelope and carrier
pitch automation. An enveloped operator that feeds another operator is pulled
through the silent head of the onset quantum at its default gain of 1; a direct
carrier envelope is not. Correlations are 0.99015–0.9999996 with RMS ratios
1.00108–1.01167. The numbered late case is the low end.

Four `fm-cycle-` gates add a three-operator loop, self-feedback, a signed mutual
loop and an eighth-operator cycle. Correlations exceed 0.9999996 with RMS
ratios 1.00103–1.00106.

Five `fm-range-` gates cover harmonicity 64, a high note, index 24, harmonicity
−48 and a low note. Sine operators and the carrier follow OscillatorNode's
band-limited table: full level below Nyquist/2^(1/3), then 3·log2(Nyquist/|f|),
and silence at ±Nyquist, while phase advances at the clamped frequency.
Correlations exceed 0.999992 with RMS ratios 1.00102–1.00111. All of these
retain the existing 0.99 correlation and 0.85–1.15 RMS gates.

Five `static-detune-` cases additionally cover four basic oscillators and a
shared-PCM sample: the REPL does not apply static detune as transposition.
Correlations exceed 0.9990 with RMS ratios 0.9951–1.0017. Unison spread remains
a separate source control. All ten retain the existing 0.99/RMS thresholds.

The six `generated-pitch-` cases independently check pitch envelopes, vibrato
and detune on `z_sine` and custom bytebeat. The deployed player ignores these
controls for these generated sources. Removing native-only detune transposition
raises the detune correlations from approximately zero to 0.999999 (ZzFX) and
0.999320 (bytebeat). All six pass unchanged 0.99 correlation and RMS gates at
zero lag; measured RMS ratios are 1.00152 and 1.00220 respectively. These checks
do not establish all-preset or arbitrary-frequency equivalence.

Run from the repository root after `npm ci --ignore-scripts` in `js/`:

```sh
cmake --preset default
cmake --build build --target compare-audio
# Or choose an artifact directory:
node scripts/compare-audio.mjs /tmp/my-comparison
```

Requires Chromium, Node with its standard APIs, installed piano catalogs and
recordings, and HTTPS access to strudel.cc and its sample hosts. Set `CHROMIUM`
to an alternative Chromium executable. This is an explicit diagnostic, not a
network-dependent part of `ctest --preset default`. It creates a disposable browser profile
and uses Chromium's debugging pipe; it does not connect to an existing browser
or the running Tompouce MCP/UI session.
The harness checks `assets/banks` and the standard user-local installation for
catalogs. Set `TOMPOUCE_BANKS` for another installation root.

`build/tests/render_audio` builds the real native callback and event/sample resolver
into a separate executable. It queries the pinned QuickJS runtime one cycle at
a time, resolves samples and prepares shared delay/room connections before rendering, and
triggers each event at its due frame. Delay decisions use preparation-time
state like the live commit path, matching the web's preparation-time parameter
guards. It opens no PortAudio device. Its output is interleaved stereo float32
PCM at 44.1 kHz on little-endian machines. Reference mode bypasses the legacy
DC/drive/soft-limit stage but still clamps output to [-1,1]. The comparison
cases use levels below clipping. The browser offline render maps hap onsets
through the requested render rate and ignores event-level cps pivots there;
tempo timing is covered by the live engine harness below instead.

The browser loads actual strudel.cc and uses its `evaluate` and
`renderPatternAudio` APIs. Only the source transpiler comes from our pinned
1.2.6 package; pattern evaluation, sample selection/decoding and WebAudio
rendering come from the site. Each case gets a separate page. The renderer's
eager scheduling path is selected because its suspend/resume path can race
completion of short offline renders in current Chromium. This affects only
the disposable diagnostic page.

Artifacts include each input, native PCM, browser WAV and queried events, and
`report.json`. The report records Chromium's version, native bundle hash,
browser script URLs/hashes, native executable/package versions, input and
capture hashes, capture time, metrics and thresholds. Capture frame counts
must also match the requested duration.
The hosted web player can change; a failure is evidence to inspect, not a reason
to update packages or loosen a threshold automatically.

Regression gates compare at zero lag (no capture shifts or gain
normalization). The original piano windows remain 0.6 seconds; the new synth,
generated-room and live-change cases check the full two-second capture:

| Case | Minimum normalized correlation | Native/browser RMS ratio |
| --- | --- | --- |
| Dry C4 piano | 0.99 | 0.85–1.15 |
| Arpoon piano with room disabled | 0.99 | 0.85–1.15 |
| Piano with custom TR909 snare convolution IR | 0.99 | 0.85–1.15 |
| Sine: default, ADSR, low-pass and explicit center pan (4 cases) | 0.99 | 0.85–1.15 |
| Sine with fractional shared delay | 0.99 | 0.85–1.15 |
| Sine with shared delay automation: feedback/time/zero-gain changes and return to baseline (4 cases) | 0.99 | 0.85–1.15 |
| Saw, square and triangle at 440 Hz (3 cases) | 0.99 | 0.85–1.15 |
| Pulse/PWM/panning/onsets, harmonic counts/custom phases, noise blend and looping noise sources (36 cases) | 0.99 | 0.85–1.15 |
| ZzFX sine and bytebeat (2 cases) | 0.99 | 0.85–1.15 |
| Piano with seeded generated convolution room | 0.99 | 0.85–1.15 |
| Stretched piano: half/fractional speed, upward shift, reverse, loop, envelope/filter (6 cases) | 0.99 | 0.85–1.15 |
| Downward-stretched MP3 piano: factors, C4/Fs3/Ds4, fractional speed/onset (8 cases) | 0.99 | 0.85–1.15 |
| Downward-stretched shared PCM: integer/fractional onset (2 cases) | 0.99 | 0.85–1.15 |
| Shared PCM fractional sample onsets: natural/fractional/double/reverse speed (4 cases) | 0.99 | 0.85–1.15 |
| Shared PCM fractional onsets under stretch-variant and delay-automation changes (2 cases) | 0.99 | 0.85–1.15 |

The C4 capture lasts two seconds and Arpoon captures four cycles/eight seconds.
Only the stated window is gated; these checks do not establish full-song
parity. The observed dry correlations were 0.99999994 and 0.999986 respectively.
Any non-finite capture or silent window fails. Peak levels and full frame counts
are recorded too. No best-lag search, time shift, or gain
normalization hides timing/level differences.

The new comparisons exposed and fixed default mono panning attenuation,
stock periodic-wave normalization and bytebeat quantum startup. Measured
ZzFX/bytebeat correlations exceed 0.999999; saw/square/triangle exceed 0.999.

The `source-` cases exercise the complete callback against the deployed web
renderer, including ADSR, stereo routing and fractional onsets. Run
`TOMPOUCE_COMPARE_FILTER=source- node scripts/compare-audio.mjs /tmp/tompouce-source-parity`
after building `render_audio`. They cover pulse defaults, width and LFO defaults,
explicit panning, numeric harmonics from 1 through 2,047, custom coefficient
phases, `.noise()` dry/wet levels, and white/pink/brown sources. Noise cases seed
the actual web noise-buffer generator, including its two-second loop; comparison
does not depend on unrelated chain-ID random draws. They retain the existing
correlation and gain thresholds. These selected source gates do not establish
arbitrary modulation/effects or full-song parity.

`cmake --build build --target test-supersaw-reference-web` separately compares
the production supersaw source with the actual deployed AudioWorklet. It seeds
the worklet's random phases and checks stereo PCM for defaults, single/100
voices, fractional unison, zero/wide spread, detune and `n` fallback, and high
frequencies. The largest observed sample error across nine cases was below
`2e-7`. This is a raw-source gate, including the worklet's first silent render
quantum; it does not establish full ADSR, effects or native onset parity.
Reports and PCM go to `/tmp/tompouce-supersaw-reference` by default.

Supersaw uses independently seeded phases for each live voice, five oscillators
by default, spread `.6` and total detuning width `.18` semitones. Its `detune`
control (or `n` fallback) sets that width rather than transposing the note in
cents. Oscillators alternate equal-power stereo positions and use the web's
`1/sqrt(unison)` gain. Unison is bounded to the web's 100 voices; ratios and
phases are prepared before rendering.

Generated native rooms now use convolution with the reference decay, fade and
gradual low-pass algorithm. The generated-room gate uses an explicit LCG seed
0xc0ffee in both implementations; it observed correlation 0.9921 and RMS ratio
1.0311. The seed is part of the case/report, not an implicit replacement for
ordinary browser randomness. Native continues that stream across generations
(advancing past cache hits) so rooms vary per call deterministically instead
of reseeding to identical kernels; ordinary browser rooms remain randomized.
Multi-room sequences stay diagnostic: each superdough trigger also draws one
chain-ID random before any room draw, so exact seeded alignment would require
mirroring that incidental consumption, and the eager comparison renderer
bounds each hap's capture to its clipped duration (hard zeros past it), so no
convolver tail reaches a gate there. A CTest locks sequence variety (distinct
rooms differ) and cache identity. Custom IR correlation remains 0.99999997.

Original wet Arpoon is captured as a diagnostic with no pass threshold. It has
independent random impulses and observed opening correlation around 0.94–0.95.
The original stretched-piano capture compared
native overlap-add grains against the browser phase vocoder and observed
correlation about 0.08 before the native phase-vocoder replacement. It now
correlates above 0.9999 with RMS ratio 0.9999. It uses the matrix's existing
0.99 correlation threshold, gating the entire two-second capture including
onset and ending.
The stretched input starts at 0.25 seconds in both renderers because upstream
subtracts 40 ms for its worklet and rejects a resulting negative onset. Captures
are never shifted to conceal this difference.

Native preparation follows the reference's envelope-before-vocoder order,
2048/128 window/hop, peak regions, Float32 accumulation and quantum graph
disconnect. Early precursors before the musical onset are omitted. Fractional
speed, upward pitch, reverse, actual loop wraps and envelope/filter ordering
have separate gates. Native input lifetime is bounded to 90 seconds, with a
128 MiB/128-buffer cache. FFT and preparation run outside the callback.

`ctest --test-dir build -R '^prep_profile$' --output-on-failure` measures the
cold-cache, live-edit, burst and duplicate-key workloads used before choosing
preparation workers. Counters stay off unless that test enables them. It records
wall time, thread CPU time, and lock wait and hold for sample decode, stretch
overlap-add, sample-convolution FFTs and generated-IR synthesis. On one run,
decoding an 8-second stereo WAV took about 3.5 ms and held the sample cache lock
for about 1 µs; four concurrent decodes waited about 2 µs. A half-second stretch
miss did about 12.6 ms of overlap-add while holding the stretch cache lock for
about 61 µs; repeating that key did not run the vocoder. Changing pitch prepared
again and did not decode the sample. The 0.8-second generated room spent about
2.2 ms in synthesis and 3.0 ms in kernel FFTs while its cache lock was held for
under a microsecond. Eight stretch misses on one thread took about 31 ms with
about 32 µs of lock hold. Four threads preparing distinct stretches did about
38.7 ms of combined work in about 10.0 ms of wall time, with about 12 µs of
combined wait. Stretch and generated IR use different locks, so overlapped calls
finished in about 12.4 ms of wall time for about 19.1 ms of compute. Four
concurrent requests for one stretch, convolution or generated-IR key all
returned the single published entry; losing duplicates were freed. The live
room serial lane still completes generated requests one at a time. The same
profile derives its worker counts from process affinity and cgroup quota, and
reports the CPU budget and total pool threads (general workers plus the serial
room lane). A four-key pool benchmark took about 29.4 ms with one worker,
15.2 ms with two and 9.3 ms with four. A separate scheduler regression advances
the owned clock across a staged onset before finishing refresh; the original
onset remains and the replacement is discarded. The MP3 branch
runs when `assets/banks/dough-samples/piano/C4v8.mp3` is present. These timings
are one machine's profile, not a parity gate.

`cmake --build build --target profile-prep-deadlines` runs the next preparation
measurement, using `scripts/prep-deadline-qa.mjs` and `test_prep_deadlines`.
Override `TOMPOUCE_PREP_DEADLINE_BINARY` to use an isolated build. Each case gets
a fresh process, owned temporary sample store and real generator/consumer
threads. A paced, device-free callback renders into memory for two seconds.
Cold and live cases combine four 8-second stereo WAV decodes, a half-second
stretch source, a 0.25-second custom IR and a 0.8-second generated room. The live
case changes speed, custom-IR speed and room size through direct sliders without
reevaluation. The burst case adds sixteen distinct long-sample stretches. The
voice-burst case instead layers twelve preparation-free sine voices per
cycle with four distinctly-stretched sample voices: the sines always publish,
so the 128-voice pool saturates deterministically (peak retiring graphs prove
eviction happened) while the workers carry real stretch load. Its long
releases keep every accepted voice sounding past the run horizon. The sine
voices also duck a shared orbit, so control publication and timer dispatch
run under the same load; the report records published controls alongside
sources.
Worker counts 1/2/4 (bounded by the process budget) are measured serially with
128- and 4,096-frame callbacks; no other verification should run during a timing
capture. 4,096 frames is the output adapter's render-call cap, so both block
sizes are realizable configurations. The ordinary CTest runs one worker with
128-frame callbacks as a structural smoke test, not a zero-miss timing assertion.
Its burst case asserts job-level decode/stretch work; room commit counters are
reported by the QA matrix instead, since overload may shed the room sine
before commit.

The JSON report separates submitted/completed preparation jobs, queue-to-source
completion latency, published sources, actual voice reservations, preparation
skips, late publication/reservation, callback overruns and host wake delays.
The offline report also records peak concurrent voices and peak retiring
graphs per case. Counters are disabled by default and run outside the callback; callback timing
is measured by the harness around the real render function. Snapshots are taken
at the end of the paced interval, before shutdown drains outstanding jobs.
Queued future events and unfinished jobs explain why published/reserved counts
can differ; these counts are not full waveform-onset observations. The script
returns failure unless cold/live cases hold fully (no skips, late
publication/reservation, overruns, wake misses or unfinished jobs), every
case sheds cleanly (no late publication/reservation, overrun or wake miss),
and every voice-burst case shows retiring graphs (the saturation proof is
vacuous otherwise).
Local cold reads do not measure remote download latency. An offline callback
overrun is not a backend underrun. On a contended desktop host, sub-3 ms
128-frame callback budgets can trip on scheduling jitter even for pre-existing
workloads; the report preserves such violations instead of hiding them, and a
rerun on a quiet host is the tiebreaker rather than a wider budget.

An isolated 18-case run on a process budget of 16 CPUs observed:

| Workers | Cold/live source latency maximum, 128 frames | Long-stretch burst late source skips, 128 frames |
| --- | --- | --- |
| 1 | 18.7 / 17.1 ms | 44 |
| 2 | 11.4 / 11.5 ms | 27 |
| 4 | 8.5 / 8.2 ms | 11 |

Cold/live cases at both block sizes had no observed preparation skips, late
publications or late voice reservations. All cases had zero late publications,
zero late voice reservations, zero callback overruns and zero host wake misses.
Burst job latency maxima were 490.3/246.7/128.6 ms; the sixteen-long-stretch
burst sheds that overload as specified skips rather than late bursts, so the
matrix passes its gate with cold/live 12/12 holding and 18/18 shed-clean.
These machine-specific timings establish the overload-shedding behavior, not a
new baseline or a full live-performance parity claim. The initial
exploratory harness also included ambient catalog discovery; the maintained
fixture now fixes the bank root and working directory to its own store so those
startup costs do not contaminate source-work comparisons.

`cmake --build build --target profile-prep-backend` measures stream continuity,
render deadlines and frame coverage under real backend preparation load, using `scripts/prep-backend-qa.mjs`
and the `backend` mode of `test_prep_deadlines`. It creates an owned null sink,
targets an inspected silence stream at it, verifies with `pw-dump` that the
stream has exactly two active links and both reach only that sink, then runs
cold/live workloads at 1/4 workers and the long-stretch burst and the
voice burst at 4 workers
through the real PipeWire backend at 256-frame blocks. Default routing is never
changed and the module is unloaded afterwards. An isolated run observed zero
stream drops, zero render overruns and no rendered-frame shortfall in all six
cases, with cold/live fully held, the long-stretch burst shedding only skips
(9 in this run) without late publication or reservation, and the voice burst
publishing all 209 sources with peak 128 concurrent voices and 21 retiring
graphs. Local fixture reads still do not
establish remote-download deadline behavior. Missed device cycles are counted
on the output thread itself: gaps between PipeWire process callbacks beyond
1.5x the delivered quantum (against the larger of the old and new quantum,
so recovery resizes cannot mask an underrun) report an underrun of the owned
stream, with empty callbacks counted separately. A one-shot injected stall
positive control proves the accounting fires. An isolated run observed zero
gaps in all six load cases; the injected run counted exactly one gap of
102.7 ms for a 100 ms stall. Backends other than PipeWire leave the counters
at zero, which the report treats as unmeasured rather than zero underruns.

`cmake --build build --target test-live-reference-web` compares fractional
sample onsets under live engine operations, using
`scripts/live-change-reference-qa.mjs` and the `--live-onset-reference` mode
of `test_live_sliders`. Both engines run deterministically without realtime
capture: the native live engine advances virtual time in exact 128-frame
blocks with scripted operations on integer sample boundaries, and the web
live REPL runs on fake time with the same block rule while scheduling real
pattern audio into a disposable offline context. The shared fixture sample
is generated once and read byte-identically by both sides; the mock interval
is honored so no scheduling pass runs after Stop, while mock time keeps
advancing through stops exactly as the production audio clock does. Nine cases
gate at zero lag (0.99 correlation, 0.85–1.15 RMS): source re-evaluation,
pattern-level tempo pivots (which both engines anchor at the pivoting hap),
transport Stop with fractional tails, transport restart, external tempo
pivots, chained double re-evaluation, sample slice/begin-end changes,
shared-effect automation swaps, and failed-evaluation retention (the failure
must report on both sides; invalid code evaluating cleanly fails the gate). Restart re-fires early-cycle haps from cycle zero on both
sides: the web resets its query cursor and re-anchors at the restart tick,
matching the native cursor reset and restart-plus-latency anchor, so the
re-fired onsets agree sample-exactly. An earlier harness froze mock time
during stops, which made post-restart catch-up windows look older than they
were and skipped the re-fired hap as too late; advancing time fixed it with
no native change. An isolated run observed correlation 1.0 in all nine.
External tempo pivots anchor at the scheduling frontier on both sides: the
web anchors the pivot cycle at its last queried end and the time at its next
callback phase, matching the native queried-cycle cursor and phase-frontier
plus-latency anchor, so post-pivot onsets agree sample-exactly. An earlier
harness ticked the web driver every render chunk instead of the production
100 ms driver cadence (which the native js_tick gate already follows),
scheduling one extra query window ahead and shifting web pivots by a window;
gating web ticks to the production cadence fixed it with no native change.

The CPU-budget fixture test covers nested v1/v2 membership, inherited minimum
quotas, controller lists, subtree mounts, escaped paths and malformed limits,
without changing the host's cgroups. Kernel membership and hierarchy semantics
are described in the [cgroup v2 documentation](https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html).

Downward MP3 piano now has eight gates. The native MP3 decoder previously
rounded its output to PCM16 before storing floats. Removing that quantization
raised the original case's correlation from 0.9722 to 0.9998. The reference's
last downward peak region reads upper-half real-FFT intermediate bins, making
this path unusually sensitive to small input differences. Synthetic stereo MP3
tests preserve sub-PCM16 decoder precision alongside the existing gapless checks.

Fractional onsets exposed a separate one-frame delay: the prepared PCM began at
floor(onset), while playback began at ceil(onset). Anchoring preparation to the
first playback frame fixed both independently decoded MP3 and shared-WAV cases.
No capture shift or gain adjustment is applied. The eight piano cases cover
`.stretch(-1/-2/-3)`, C4/Fs3/Ds4, fractional playback speed and fractional onsets;
their measured correlations range from 0.9982 to 0.9999, with RMS ratios
0.9992–1.0016 across the full two-second capture.

Two independent DSP gates also use identical generated PCM16 WAV bytes in both
players, with integer/fractional onsets. Their source hash is recorded in the
report, and the WAV and confined catalog remain under `fixture-banks`; the
browser uses those bytes through a disposable Blob sample. These selected
MP3/WAV gates do not establish complete decoder or full-song waveform parity.

Five additional shared-mono cases (`TOMPOUCE_COMPARE_FILTER=pcm-mono`) guard
the preparation fast path with upward/downward pitch compensation, reverse,
looping and ADSR at fractional onsets. Both sides receive the same mono PCM16
WAV and the report hashes that source independently from the stereo fixture.
The 0.99 correlation and 0.85–1.15 gain gates are unchanged.

Use `TOMPOUCE_COMPARE_FILTER=stretch` to select the stretch cases while iterating;
omit it to run the full matrix.

Fractional sample-onset cases use the shared WAV without stretching. Samples
retain their fractional source position and envelope age when the callback
starts on the next whole frame. The double-speed case improved from 0.9747 to
above 0.999999 correlation without capture shifts or changed thresholds.
Reverse playback also preserves the deployed `reverseBuffer` channel copy
offsets: its right channel has a leading zero and one-source-frame displacement.

`ctest --test-dir build -R '^audio_performance$' --output-on-failure` runs a
device-free scheduler/callback regression with 64 prepared notes and 64 duck
callbacks. It checks expected forward/reverse sample PCM at fractional onsets
for six playback rates, then requires bit-identical output across callback
sizes 1, 64, 128, 256, 4,096 and 8,192. The harness also runs through the
`test_audio_performance_sanitize` executable built by `test-sanitize`.
This verifies prepared-event rendering independently of callback size; real
backend scheduling under load, underruns and voice-pool saturation remain open.

`node scripts/voice-pool-reference-qa.mjs /tmp/voice-pool-reference` checks the
deployed web pool in a disposable browser with offline rendering. It fills the
128-source pool with reverse-ordered target times and differing gains, verifies
insertion FIFO, and captures eviction gain ramps and stops at the incoming
target time plus 250 ms. Rests and missing sounds also evict before source
selection; zero gain still creates a source. Native saturated allocation uses
insertion FIFO, with regression coverage for out-of-order targets and reused slots.
Incoming stretch events apply the web's 40 ms preroll before calculating the
eviction stop deadline; the deployed pool reference also gates this case.

`node scripts/voice-eviction-reference-qa.mjs /tmp/eviction-reference` captures
real registered sine/piano gain automation using private offline contexts. It
replaces only the input waveform with DC, keeping the actual source gain node,
pool registration and eviction calls. Ordinary oscillator eviction inserts a
ramp into the existing ADSR timeline; sampled-source eviction ramps a separate
unity cut gain from preparation time. Both stop at incoming target + 250 ms.
Native retiring graphs implement these selected behaviors without resetting
phase, DSP state or shared sends. The callback harness verifies expected sine
and sample PCM, shared delay output after retirement, surviving duck callbacks,
bit-identical 1/64/4,096/8,192-frame rendering, and bounded overflow diagnostics.
Native-state ownership is separately checked through retirement and cleanup.
Explicit rest and missing-source attempts now queue an eviction-only operation:
at capacity they remove the oldest source, retain its fading graph, and leave a
free current-source slot. They produce no MIDI or note-history entry and do not
prepare shared effects. A rejected duck timer still aborts before this operation.
The scheduler harness verifies the rest/silent/missing/silent/stretch sequence.

Drum voices fade like every other evicted source: a stolen drum scales by the
same retirement envelope through source stop at the incoming target plus
250 ms, then stops. The deployed pool ramps each evicted entry regardless of
source type (bd/sd/hh captures all show ramp+stop pairs in FIFO order). A
native A/B regression renders a retired TOM against its unevicted self and
requires exact fade scaling plus post-stop silence; a mixed
sine/kick/tom/sample saturation burst requires FIFO order, the same stop
deadline for every type and bit-identical 1/8,192-frame rendering.

`cmake --build build --target test-voice-reference-web` also runs
`scripts/voice-edge-reference-qa.mjs`: 48 web/native short-envelope, coincident
endpoint and sample-cut cases, plus ring-out cases for low-pass, high-pass,
band-pass, phaser and coarse graphs (62 cases total).
An existing automation endpoint at the exact stealing deadline remains audible
until source stop. Sample cuts schedule their future unity set and 10 ms ramp
at preparation time, overriding an intervening stealing fade. Filter cases gate
post-stop PCM only; DC is injected after the sampled instrument's own ADSR, so
they do not establish full filtered attack/release waveform parity.
Downstream filters and native effect graphs now process silence after source
stop until the original cleanup deadline, retaining state and shared sends.
The retired phaser is bypassed there: the deployed player holds a retired
phaser at exactly zero post-stop while its low-pass rings, so the eviction
gain sits downstream of the phaser graph. The deployed player exposes no
chorus control, so chorus stays native-only: an impulse regression locks the
dry mix, the 20 ms LFO-centered wet tap and the tail silence. Vowel topology
is gated instead: `voice-vowel-reference-qa.mjs` compares one-second impulse
responses for all fifteen vowels against the deployed five-formant parallel
filter (per-formant gains, makeup gain 8, Q up to 140), worst case 1.2e-7,
through the real letter-to-params path. The old three-formant approximation
fails that gate. Chain placement was audited against the deployed chain and
matches (filter, vowel, coarse/crush/shape/distort, tremolo, compressor,
phaser); static pan commutes with the per-channel phaser, so mono pre-pan and
stereo post-pan filtering agree. Full-voice vowel PCM stays at IR level by
design: raw oscillator parity is separately ungated and would conflate.
Offline regressions check ring-out across 1/8,192-frame blocks for low-pass,
high-pass, band-pass, vowel, phaser, chorus and coarse graphs, plus native
effect-state lifetime; a cut-ordering regression checks same-group cuts with
first-cutter-wins, group isolation, equal-time onset-serial tiebreak and
oscillator exclusion. Remaining
source types and scheduling-overload parity still need independent checks.

Generated room preparation runs outside the scheduler and callback. The
`room` CTest holds the worker at a deterministic barrier while checking the
actual convolver output against a retained history, then verifies completion,
failure retention, initially silent wet output, bounded queue overflow and
worker shutdown/restart. `effects-reference-qa.mjs` checks actual web buffer
handoff, an intervening custom IR and failed zero-duration generation's retained
buffer and updated parameter guards. Static waveform gates deliberately prepare
completed IRs before rendering; they do not measure asynchronous completion
latency or prove source-side failure parity. Multiple generated requests use a
single FIFO lane in the shared preparation pool; browser completion races remain
open. The completion checks the scheduling generation and publishes the kernel
under the same generation guard, so a transition cannot make the check stale;
the room CTest holds that guard while a transition thread waits.

`scripts/duck-preparation-reference-qa.mjs` blocks an actual Superdough source
trigger and verifies that its target orbit's duck method has already run. Its
native counterpart blocks the preparation worker, consumes the independently
published control, advances the owned callback and verifies that accepted
ducking survives a generation transition while the source remains blocked.
It is included in `test-duck-reference-web`; it verifies this ordering rather
than claiming full live-source waveform parity.

`node scripts/duck-timing-reference-qa.mjs /tmp/duck-timing-reference` compares
129 real web timer-callback/gain-PCM cases with native control conversion and
automation. It covers selected unusual numeric strings, nested onset values,
past endpoints, late callbacks, interruption during downward/return ramps and
failed first/return ramps after cancellation. Timers are dispatched explicitly
at suspended offline render boundaries, then detached to prevent duplicate
natural callbacks. Native reanchoring uses the last rendered sample, matching
the web's AudioParam.value read. `test-duck-reference-web` builds and runs this
gate; `TOMPOUCE_LIVE_CONTROLS_BINARY` selects another native test executable.
Long zero/exponent/whitespace strings and embedded NUL controls are included.
This does not establish live timer-jitter/overload parity or timer-creation
failure side effects.

`duck-onset-reference-qa.mjs` adds 8,944 exact browser/native numeric
concatenations, comparing binary64 bits at decimal halfway, subnormal and
overflow boundaries, with long tails, radix forms, scientific time prefixes
and every Unicode WhiteSpace/LineTerminator plus confusable non-space code
points in every suffix position.
The native summary uses fixed-size integer arithmetic rather than the pinned
QuickJS decimal reader's deliberately limited precision. The `duck_onset`
CTest checks bounded cache exhaustion, deduplication and retained references;
the scheduler harness checks summary lifetime after JSON/event destruction.
Both browser scripts run through `test-duck-reference-web`.

`duck-timer-reference-qa.mjs` adds 54 actual Superdough/native preparation cases
at note times 5, 10 and 11 ms, with sine, rest and missing sources and ordered,
repeated or unavailable targets. It verifies source-orbit registration,
first-target rejection before gain cancellation, source/effect preparation and
voice eviction, and the accepted zero-stop boundary. Its timer callbacks are
detached before offline rendering; this checks construction, not callback
latency. The native callback harness separately verifies a rejected event
against a full voice pool, retained independent timers, late preparation and
Csound/OSC bypass. Valid rest/missing-source pool eviction remains a separate
open comparison. The timer gate also runs through `test-duck-reference-web`.

`duck-mapping-reference-qa.mjs` adds 59 web/native per-target mapping cases:
which array element (or scalar) reaches each ducked orbit. It covers scalar,
pair, short, long, empty and null-entry onset/attack/depth arrays across one
to three targets, repeated targets with per-entry controls, scalar targets
with array controls, nested singleton targets, string targets, invalid
targets (which still consume their forEach index), and missing control keys.
An empty control array selects undefined at the orbit, reaching the orbit
parameter defaults for onset and depth, while attack has already passed
through Math.max and arrives as NaN. Null and boolean selections are
compared through the same single-step coercions the timing gate proves at
ramp/PCM level, including seven new null/boolean scalar timing cases.

A native 150-note duck flood covers live callback execution under pool
saturation without a browser: overlapping 2 s voices across all 12 orbits
fill the 128-voice pool (peak retiring graphs required) while 150 timers
dispatch, including NaN-depth and NaN-attack rejections confined to orbits 10
and 11. Every accepted timer must dispatch, each orbit 0–9 must hold its last
valid duck exactly, the 25 rejections must land in the bounded mailbox in
dispatch order without overflow, and rendering must be bit-identical across
1/8,192-frame callbacks. This proves accepted callbacks survive voice
stealing; it does not establish live timer-jitter parity under preparation
load or timer-creation failure side effects.

## Shared sends and mono-IR tail width

Thirteen `shared-` cases compare eight delay-control scenarios (send toggles,
zero feedback, negative delay time, gain/postgain-zero notes, orbit switches,
stack order and fractional onsets) and five reverb scenarios using the exact
shared mono IR and stereo source PCM. The IR's hash is recorded separately.
The room cases include a single clipped source, room-send toggles, a silent
send, orbit changes and combined delay/reverb.

The first reverb captures failed at correlations 0.038..0.725. Their left tails
already matched; the right tail differed after source cleanup. Chromium's
[reverb channel matrix](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/platform/audio/reverb.cc)
runs only the left convolver for mono input + mono IR, leaving the right
history paused. The output then upmixes to both channels. Native kernels now
retain their IR channel count and independent history clocks; room preparation
retains the stereo send's cleanup deadline. No history allocation, freeing or
channel rebuilding occurs in the callback. Tests check a delayed impulse
across mono/stereo transitions, right-clock suspension and deep cloning under
each forced convolution backend.

The offline renderer now prepares room sends alongside delay automation,
matching the eager web renderer's preparation-time graph connections and IR
reuse. All 13 cases pass unchanged gates. Delay correlations exceed 0.99974;
room correlations exceed 0.999999 with RMS ratios 0.9930..0.9937. That small
remaining level difference is not corrected or normalized out.

Nine additional `shared-room-lifetime-` gates extend the shared-effect matrix
to 22 cases. Stereo sources without a pan stage cover natural completion,
fast/reverse playback, a buffer shorter than its explicit clip, pitch envelopes,
vibrato, slicing, looping and `fxr`. Source and mono IR use the exact shared PCM;
fast/reverse cases apply corresponding IR rates to avoid near-silent captures,
and all levels stay below clipping. The prior stereo deadline followed the
original hap, leaving the wrong right-channel tail after the source disconnected.
The valid initial captures failed at correlations -0.217..0.901. All nine now
correlate above 0.999996 at lag zero with the unchanged thresholds.

Two more `shared-room-source-` cases cover a panned source (`.pan(.7)`) and a
downward-stretched source (`.speed(.5)`) feeding the mono IR. Both pass at lag
zero: correlation 0.99999998 and 0.999705, RMS ratios 0.9931 and 1.0111.
Two `shared-room-kernel-` gates extend this to kernel-change automation via
`.roomsize(".3 .8 .3")` and `.size(".2 .6 .2")`: correlations 0.99999999 and
0.99999999, RMS ratios 0.9930 for both, with the same unchanged thresholds. Two
further `shared-*-graph-source` gates connect a Kabelsalat graph source to the
shared mono IR and to the shared delay: correlations 0.999999998 and 0.9999990,
RMS ratios 0.9930 and 1.0007. Fractional **IR** rates previously diverged: matched-source
`.irspeed(.5|.75)` measured correlations -0.78/-0.92. The cause is the web's
`adjustLength` index math: it multiplies the index by `abs(speed)`, wraps it
with `%`, then negates the index when speed is below unity before `Array.at()`.
Sub-unity rates therefore read backward from the end with truncation, not
interpolation. `convolution_resample_ir` now reproduces that exactly, including
zero speed, and `test_convolution_prepare` pins `.5/.75/2/-2/0/1`.
`shared-room-lifetime-rate-half` and `-rate-threequarter` now measure
correlations **0.99999999/0.99999998** and RMS ratios 0.9930/0.9931 at the
unchanged gates. A source-unity-speed variant remains diagnostic
(`correlation:null`): its room level is tiny and the residual is dominated by
a source-path level/phase difference, not the IR kernel. The earlier
interpolation and kernel-stretch attempts were removed.

Preparation calculates the natural buffer lifetime from source rate, offset
and the source's scheduled stop. Pitch/vibrato transport uses the same
quantum-rate envelope schedule as rendering, with at most 32,768 quanta of
work; unusually long modulation retains the conservative scheduled-stop
deadline. This performs no callback integration or allocation. Tests cover
constant rate, reverse playback, loop lifetime, a known pitch integral and
the bounded fallback. An `fxr` extension does not keep a disconnected sample
source stereo merely because its shared reverb continues sounding.

Panned/stretched/graph source connections, source eviction, live reconnections,
kernel changes and the graph/effect FX boundary now have independent checks.
The eviction/reconnection checks use `test_audio`: a retired source's shared
delay samples survive pool saturation and a same-orbit dry source; changing the
prepared delay time keeps the line and reads it at the new offset; a fallback
room tail survives source retirement. Pattern `FX(...)` controls replay on the
event, while callable arguments are ignored by the current site and retain only
the chain's default 0.8 gain. Pattern filters and pan are post-source stages
before source pan; source filter plus FX filter, multiple FX filter arguments
and source pan plus FX pan have `fx-overlap-` gates at 0.9999988+ and ratios
near 1.0003. `test_javascript`/`test_audio` cover the graph-source boundary and
a live room+filter+pan+coarse+shape+distort+chorus edit during an active delay
tail. The replay also covers crush, postgain, phaser, tremolo, compressor, FM
operator/envelope/matrix, IR/room and canonical numeric DSP controls. Pattern
`partials` arrays flatten to a numeric control and loop windows are ignored by
the site; both have parity gates. Pattern shape is a post-source waveshaper
(`fx-pattern-shape` 0.99978/0.932, `fx-overlap-source-shape` 0.99979/0.943);
pattern delay/IR sends are ignored, leaving source sends in force
(`fx-overlap-source-delay` 0.9999996/1.0005). FX room presence selects a
mono-left room feed plus the source R/L ratio and holds that ratio through the
tail; `fx-overlap-source-room` now measures 0.9999994/0.9938 and
`test_fx_room_routing` covers the routing/ratio. The pan .75 variant still
differs in level (0.9999991 correlation, 0.538 RMS ratio) and stays
diagnostic. Source distort/chorus remain ungated; the remaining controls above
are still outside.
Existing randomized room-switch diagnostics remain diagnostics.

## Long generated rooms

Generated rooms now accept the web REPL's maximum size 10, including the full
15-second IR (661,500 frames). Size 4 prepares a six-second IR (264,600 frames).
`test_room` checks both boundaries, rejection above 10, the full late tail,
history wrap, cache behavior and failure retention. Its sanitizer build also
instruments convolution, SIMD kernels and impulse generation.

```sh
cmake --build build --target test-room-reference-web
cmake --build build --target profile-long-room-backend
TOMPOUCE_SIMD=c node scripts/prep-backend-qa.mjs /tmp/tompouce-long-room-c --long-rooms
```

The web check waits for the deployed REPL to generate sizes 4 and 10, then
shares each actual stereo IR with the native convolver. It compares every output
sample through the full tail: unit-gain convolution uses a 3e-5 maximum error
gate, while normalized output uses the maintained audio matrix's .99 correlation
and .85–1.15 RMS-ratio gates. These are separate checks: the existing small
normalization-gain difference must not mask convolution precision errors.
Forced C and automatic SIMD must produce byte-identical PCM in both modes, and
late-tail energy must remain nonzero. This establishes capacity and convolution
behavior, not identical independent random room generation or full-song parity.
The captured size-4/size-10 run measured maximum unit-gain errors of 3.28e-7
and 4.77e-7, respectively, with normalized correlations above .999999999999 and
RMS ratios about .9929. All 43 existing browser audio gates also passed, with
the two existing randomized-room diagnostic cases retained as diagnostics.
The implementation pass also passed all 37 CTests, the full-size room
ASan/UBSan test and all four forced SIMD sanitizer paths. The installed
executable's owned PTY check confirms the new maximum in its diagnostics.

History is sized to the attached kernel: a maximum-size (10 s) kernel
allocates about 20.2 MiB for its orbit, while shorter kernels allocate
proportionally less and orbits without a convolution allocate nothing.
`convolution_state_reserve` runs on the room/IR preparation path. The existing
16-entry generated cache has a worst-case spectral bound of about 323 MiB at
size 10, and the 16-entry custom-IR cache about 216 MiB at its ten-second
input limit. These are ceilings, not startup resident-memory requirements. No
history allocation or kernel preparation is added to the audio callback.

The dedicated backend workload runs for 18 seconds, verifying the size-10
kernel is published and its complete history is filled before accepting a
result. It retains the existing zero-overrun/zero-missed-cycle gate and owned
silent-sink routing checks; a separate injected 100 ms stall proves the missed
cycle counter fires. Long IRs cost more CPU, so measurements remain scoped to
the selected backend, machine and workload rather than promising every orbit
can run the maximum room simultaneously.

On the Ryzen 5800X3D, the final loop-unrolled build completed the 18-second
workload with zero render/CPU overruns, late publications/reservations or missed
device cycles in both forced C and automatic AVX2. Mean/max callback wall times
were 1.228/2.625 ms for C and .939/2.370 ms for AVX2; corresponding mean/max CPU
times were 1.215/2.617 ms and .926/2.316 ms. Both stall controls recorded one
roughly 102.7 ms gap. Before loop unrolling, isolated C recorded small deadline
overruns, and a run overlapping compilation recorded seven missed cycles;
those failed reports are retained rather than counted as passes. Reproduction
artifacts for the final runs are `/tmp/tompouce-long-room-backend-c-unrolled`
and `/tmp/tompouce-long-room-backend-auto-unrolled`. They are evidence from one
machine, not permanent baselines or guarantees under arbitrary host contention.

## Prebake helpers and oscillator pitch envelopes

The prebake reference gate compares 81 setup/helper/API cases against the
deployed transpiler and native QuickJS. Multi-cycle cases cover arrangements,
cueing, sample timing and stateful glide. Ten branch cases check solo labels,
muting, replacement, query-time identity and `each`/`all` transforms against
the actual REPL. Existing exact checks remain exact;
new floating-point text-synth/random/glide/frequency cases allow 1e-12 absolute
numeric error with exact event counts and structure. Raw postgain, velocity
and delay helper checks disable the gain curve; separate audio checks retain
the actual starter's quadratic shaping. `sine-velocity-above-one`
compares `note(69).s('sine').gain(.4).velocity(2)` with the live player.
Correlation is 0.9999999887063752 and the RMS ratio is 1.000149005223337 at
zero lag, under the same gates. Velocity above 1 multiplies gain, up to the
same ceiling of 16 as the other amplitude stages. Values inside 0..1 are
unchanged.

Three `prebake-` PCM cases fetch the pinned Switch Angel starter into an owned
fixture config and compare glide, accent and delay against the real web player.
Nine `pitch-envelope-` cases independently cover native pitch ADSR defaults,
partial/zero segment settings, sustain-based anchors, short gates, late and
fractional onsets, and vibrato. These retain the existing 0.99 correlation and
0.85–1.15 RMS ratio gates, without shifting or rescaling captures. Metadata
queries use a separate fresh pattern for the stateful glide audio render so
previewing events does not prime the pattern's musical state.

The glide check exposed two native pitch problems: incorrect ADSR defaults and
segment minima, and initial oscillator automation timing. Chromium processes
a-rate oscillator automation from the quantum's array head even when the
audible destination begins later in that quantum. Native oscillators now
retain that alignment, including fractional starts, without shifting amplitude
envelopes or sample playback. See the [Chromium oscillator implementation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/modules/webaudio/oscillator_handler.cc).
Glide correlation improved from about 0.93 to over 0.999999.

Ten additional `pitch-exponential-` cases cover `.pcurve(1)` defaults, decay,
negative depths, sign crossings, exact-zero endpoints, nonzero sustain,
truncated attack/decay, release and fractional starts. The former squared
linear-envelope approximation failed all eight initial discovery cases.
Native pitch now follows the deployed `getPitchEnvelope`/`getParamADSR`
AudioParam event schedule in cents: exponential interpolation, .001-cent zero
range endpoints, linearly clipped endpoints for short gates, exact sustain
sets and WebAudio's hold behavior for sign-crossing ramps. Endpoint arithmetic
order is preserved because rounding through zero changes that behavior.
Sample-aligned clock error is removed before these discontinuities without
shifting fractional onsets. The 19 linear/exponential pitch/vibrato comparisons
pass unchanged gates; exponential correlations range from 0.99947 to above
0.999999. Run with `TOMPOUCE_COMPARE_FILTER=pitch-`.

Additional pitch/FM combinations, live oneshot/grid timing and full
helper/synthesis equivalence remain outside these established gates.

## Sample pitch automation

Eight `sample-pitch-` gates use the exact shared mono/stereo PCM fixture to
isolate BufferSource detune from decoding. They cover linear/exponential
envelopes, vibrato, release, fractional starts, stereo and reverse playback.
The initial four comparisons failed at correlations 0.3188..0.9736 because
native sample transport evaluated pitch every frame. Native transport now
reads a cached ratio once per 128-frame quantum, following the browser's
k-rate detune. For fractional starts, events older than the first active
automation evaluation are clamped to that quantum; their endpoint values and
later deadlines retain the original schedule. Sample onset and source-position
alignment are preserved. See Chromium's
[BufferSource playback-rate calculation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/modules/webaudio/audio_buffer_source_handler.cc)
and [AudioParam event clamping](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/modules/webaudio/audio_param_handler.cc).

All eight cases correlate above 0.9999998 at lag zero and retain the existing
0.99 correlation / 0.85..1.15 RMS gates. The `pitch-` filter runs these alongside
the 19 oscillator checks. The callback regression checks pre-onset neutrality,
the initial clamped value, a shortened attack and a held within-quantum ratio.
This does not establish arbitrary sample, stretch or worklet pitch parity.

## Worklet pitch and supersaw startup

Twelve `worklet-pitch-` cases use the same full-player capture path: six
supersaw cases (baseline, linear/exponential pitch, release, fractional onset,
vibrato), five pulse cases and one Kabelsalat graph-source case. Supersaw phase
initialization is controlled with the same LCG seed in both renderers. The
native diagnostic permits that override only for a single supersaw event;
production randomness is unchanged. The browser worklet remains the deployed
code, with only its random input seeded; its original SHA-256 is recorded.

The five initial supersaw comparisons failed at correlations -0.41..-0.12:
native phases advanced before the browser's first active quantum. Native
supersaw now waits for the first quantum whose head exceeds its Float32 begin
parameter. It keeps phases still before startup and does not apply the
OscillatorNode partial-quantum automation offset to the worklet. Pulse and
Kabelsalat already passed their initial checks. Graph-source pattern pitch
controls affect the replaced oscillator, not the graph's own `sFreq`.

The initial corrected supersaw gates correlate above 0.9999999 at aligned
starts and 0.99941 at the fractional onset; pulse gates exceed 0.99966 and the
graph-source gate exceeds 0.9999999. All use the unchanged 0.99 correlation,
0.85..1.15 RMS, exact duration and zero-lag gates. The `pitch-` filter now runs
39 oscillator/sample/worklet comparisons. Callback tests verify that supersaw
stays silent without advancing phases until startup, including Float32 boundary
rounding and fractional onsets. These gates do not establish every source,
FM combination or full-song parity.

## Kabelsalat effect tails and input routing

Nested `FX(...)` chains remain outside the gated matrix. A probe with
`.FX(x=>x.lpf(800))`, `.FX(x=>x.room(.3))` and `.FX(x=>x.delay(.2))` over a
clipped sine gave correlation 0.99999997 in all three cases but a constant
RMS ratio of 1.2504, outside the unchanged 0.85–1.15 band. The waveform shape
matches while the level does not, so this is a gain-staging divergence rather
than a timing or synthesis mismatch; no threshold was changed and no case was
accepted. A control sine with the same gain and no `FX` passed at ratio 1.000,
and an FX case with gain 0.5 kept the same 1.2503 ratio, so the factor is
independent of the explicit gain and consistent with the web applying its 0.8
default gain to the FX chain. Native has no `FX` gain handling; adding a 0.8
factor would be tuning to this capture rather than deriving the web's full FX
gain staging, so it was not accepted. Two `fx-nested-` diagnostic cases in
`scripts/compare-audio.mjs` reproduce the 1.2504 ratio without a gate. The
browser's serialized events contain `FX:[{}]` because the pinned `FX(...)`
builder stores callable FX arguments as empty control objects, so the deployed
renderer ignores their controls and retains only the chain's default 0.8 gain.
Pattern arguments (`FX(lpf(800), ...)`) carry serializable controls; the JS
query layer materializes both forms into bounded `_fxSteps` (at most 8 chains,
32 steps each) and the native renderer replays pattern controls. Four new
`fx-pattern-` gain/lpf/pan/coarse gates pass at correlations 0.99999967+ and
RMS ratios near 1.0003. Four `fx-overlap-` gates cover source filter plus FX
filter, two FX filter arguments, source pan plus FX pan and source coarse plus
FX coarse at 0.9999988+ and ratios near 1.0003. `fx-pattern-partials`
(0.9990/0.9939) and `fx-pattern-loop` (0.99999994/1.0003) cover the site's
flattened partials and ignored FX loop windows. The two `fx-nested-` callable
cases pass unchanged at 0.99999997/1.0003. Remaining overlap is limited to
room/delay/shape/distort/chorus and the source-level controls above. No
threshold or baseline changed. Four `fx-` gates cover the graph/effect boundary: a `K()` graph
source replaces the oscillator or sample outside the FX chain, and the deployed
renderer does not apply the chain's default gain there. The native renderer now
skips that gain for graph sources while keeping it for `audioin()` worklet
effects and ordinary sources. `fx-graph-source-lpf` (correlation 0.999999966,
ratio 1.0003), `fx-graph-out-channels` (0.999999937, 1.0004),
`fx-worklet-effect-gain` (0.999999942, 1.0004) and
`fx-worklet-effect-tail` (0.999999932, 1.0004) pass at the unchanged
thresholds.

Eight `kabel-fxr-` gates compare actual browser worklet output with native PCM:
zero/short/extended cleanup times, feedback, clipped and naturally ending
stereo samples, and explicit output channels. Run them with
`TOMPOUCE_COMPARE_FILTER=kabel-fxr node scripts/compare-audio.mjs` after building
`render_audio`. They retain the existing 0.99 correlation and 0.85..1.15 RMS
ratio gates, with no capture shifts or gain correction. Observed correlations
range from 0.9986 to above 0.999999.

The native effect now drains zero input until the original hap's effects
cleanup deadline and applies the web's final linear fade when `fxr` exceeds
the source release. Natural sample termination does not move that deadline.
Like the browser worklet, a single graph reads the first input channel and
produces both outputs; the former independent right-input graph was removed.
Callback tests retain graph state across program release and pool retirement,
then check bounded cleanup without freeing graph state in rendering.

Eight additional `kabel-order-` gates check graph-before-gain/DSP ordering:
low/high-gain nonlinear folding, an additive offset, native waveshaping and
filtering, offset/filter tails, and distinct stereo graph outputs followed by
waveshaping. All pass the existing gates with correlation above 0.9999997.
The native graph now consumes the enveloped source before event gain, then
both graph outputs pass independently through native DSP, panning and sends.
This fixes additive offsets that previously bypassed gain and waveshaping
levels that changed because processing ran in the opposite order. Unit tests
check explicit stereo outputs and zero gain even with an additive graph.
`TOMPOUCE_COMPARE_FILTER=kabel-` runs the effect comparisons together with the
graph-source cases below.

Eight further gates cover `K(...)` sources: `fxr` at 0 and 0.5 seconds on a
feedback delay, an internal `sGate` ADSR with a longer `fxr`, then gain/shape,
low-pass and pan. One fractional onset uses `.late(.137)`. Correlations are
0.99961 for the extended delay tail and above 0.99996 for the others, with
RMS ratios inside 0.85..1.15. `kabel-fx-late` checks the same 128-frame
worklet startup on an `audioin()` delay and correlates at 0.99929. Graph
sources keep oscillating with the gate low until the hap end plus the longer
of the pattern release and `fxr`; the linear fade matches the web release
node. A fractional onset previously let the legacy oscillator envelope stop
the voice one sample before that tail. Nested `FX(...)` chains remain outside
these gates.

Six cleanup-clock regressions extend the `kabel-` matrix to 30 cases. Two
accelerated shared-PCM samples end naturally at .125 seconds but retain an
.8-second worklet delay, both with and without an explicit `fxr` extension.
The delayed output previously vanished because the fade followed the natural
sample's note-off; both now correlate above 0.9999999. Three graph-source cases
cover raw releases of 0, .005 and .01 seconds with `.fxr(.005)`; a fourth checks
an `audioin()` effect with zero release. These correlate above 0.9999999,
including the former .96819 failure. Thresholds, gain bounds and lag stay
unchanged. Artifacts: `/tmp/tompouce-fxr-clock-final/report.json`.

Graph gate, fade and cleanup anchors now retain the original hap and raw
release control. Natural sample envelopes keep their own transport gate, and
source ADSR minima do not change the outer fade. Effect graphs drain silence
after naturally ending samples even without an `fxr` extension, and cleanup
follows the generic processor's 128-frame quantum. Callback regressions check
the independent clocks and a half-fade with a sub-minimum raw release.

## MIDI channel routing and delivery

`node scripts/midi-reference-qa.mjs` compares 206 cases against the deployed
REPL using owned fake WebMidi transports and native scheduler dispatch. It
checks hap values, exact message bytes/order and note-on/off timestamp
differences. Channel-array coverage includes order, duplicate/empty lists,
all-channel selection, WebMidi conversion/filtering, explicit overrides,
options precedence, mapped CCs, NRPN, program changes, bend, touch, SysEx and
transport. The largest mapped case emits 32 CCs to each of 16 channels before
the notes. Native fan-out is capped at 16 raw channel entries; oversize lists
are rejected with a warning and tested separately from the web gate.

`test_midi` also tests the real producer/consumer path and owned ALSA delivery.
Its receiver subscribes only to the output client identified by this process's
PID and client/port names. It receives an ordered three-channel note fan-out,
including a duplicate channel, then all three 20 ms note-offs before 30 ms of
synthetic audio time. The callback is paced in 128-frame steps. Queue-pressure
tests prove that insufficient off capacity rejects the entire note fan-out,
while the remaining scalar slot remains usable. No external MIDI device or
audio device is opened by this loopback fixture. This establishes the local
ALSA boundary, not USB/DIN/device latency or behavior under arbitrary load.

Native implementations and bounds are described in [JavaScript/native audio](javascript.md).
