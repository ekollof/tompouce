# Browser/native audio comparison

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
a time, resolves samples and shared delay automation before rendering, and
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

Forty regression gates compare at zero lag (no capture shifts or gain
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

`cmake --build build --target profile-prep-backend` measures real backend
underruns under the same preparation load, using `scripts/prep-backend-qa.mjs`
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
establish remote-download deadline behavior.

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
advancing through stops exactly as the production audio clock does. Four cases
gate at zero lag (0.99 correlation, 0.85–1.15 RMS): source re-evaluation,
pattern-level tempo pivots (which both engines anchor at the pivoting hap,
unlike external tempo changes), transport Stop with fractional tails, and
transport restart. Restart re-fires early-cycle haps from cycle zero on both
sides: the web resets its query cursor and re-anchors at the restart tick,
matching the native cursor reset and restart-plus-latency anchor, so the
re-fired onsets agree sample-exactly. An earlier harness froze mock time
during stops, which made post-restart catch-up windows look older than they
were and skipped the re-fired hap as too late; advancing time fixed it with
no native change. An isolated run observed correlation 1.0 in all four.
External tempo pivot anchoring diverges by design and stays a documented gap
rather than a gate: the web anchors external pivots at its scheduling
frontier while native anchors at its phase frontier.

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
fails that gate.
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

Native implementations and bounds are described in [JavaScript/native audio](javascript.md).
