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
a time, resolves samples before rendering, and triggers each event at its due
frame. It opens no PortAudio device. Its output is interleaved stereo float32
PCM at 44.1 kHz on little-endian machines. Reference mode bypasses the legacy
DC/drive/soft-limit stage but still clamps output to [-1,1]. The comparison
cases use levels below clipping.

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

Thirty-four regression gates compare at zero lag (no capture shifts or gain
normalization). The original piano windows remain 0.6 seconds; the new synth
and generated-room cases check the full two-second capture:

| Case | Minimum normalized correlation | Native/browser RMS ratio |
| --- | --- | --- |
| Dry C4 piano | 0.99 | 0.85–1.15 |
| Arpoon piano with room disabled | 0.99 | 0.85–1.15 |
| Piano with custom TR909 snare convolution IR | 0.99 | 0.85–1.15 |
| Sine: default, ADSR, low-pass and explicit center pan (4 cases) | 0.99 | 0.85–1.15 |
| Sine with fractional shared delay | 0.99 | 0.85–1.15 |
| Saw, square and triangle at 440 Hz (3 cases) | 0.99 | 0.85–1.15 |
| ZzFX sine and bytebeat (2 cases) | 0.99 | 0.85–1.15 |
| Piano with seeded generated convolution room | 0.99 | 0.85–1.15 |
| Stretched piano: half/fractional speed, upward shift, reverse, loop, envelope/filter (6 cases) | 0.99 | 0.85–1.15 |
| Downward-stretched MP3 piano: factors, C4/Fs3/Ds4, fractional speed/onset (8 cases) | 0.99 | 0.85–1.15 |
| Downward-stretched shared PCM: integer/fractional onset (2 cases) | 0.99 | 0.85–1.15 |
| Shared PCM fractional sample onsets: natural/fractional/double/reverse speed (4 cases) | 0.99 | 0.85–1.15 |

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
ordinary browser randomness. Native rooms use this deterministic seed; ordinary
browser rooms remain randomized. Custom IR correlation remains 0.99999997.

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
Rest/missing-source eviction, cut interactions, short/coincident envelope
endpoints, per-voice effect tails after source stop and the remaining source
types need broader independent comparisons;
this does not establish complete pool or scheduling-overload parity.

Generated room preparation runs outside the scheduler and callback. The
`room` CTest holds the worker at a deterministic barrier while checking the
actual convolver output against a retained history, then verifies completion,
failure retention, initially silent wet output, bounded queue overflow and
worker shutdown/restart. `effects-reference-qa.mjs` checks actual web buffer
handoff, an intervening custom IR and failed zero-duration generation's retained
buffer and updated parameter guards. Static waveform gates deliberately prepare
completed IRs before rendering; they do not measure asynchronous completion
latency or prove source-side failure parity. Multiple generated requests use a
single FIFO worker; browser completion races remain open.

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

`duck-onset-reference-qa.mjs` adds 1,264 exact browser/native numeric
concatenations, comparing binary64 bits at decimal halfway, subnormal and
overflow boundaries, with long tails, radix forms and scientific time prefixes.
The native summary uses fixed-size integer arithmetic rather than the pinned
QuickJS decimal reader's deliberately limited precision. The `duck_onset`
CTest checks bounded cache exhaustion, deduplication and retained references;
the scheduler harness checks summary lifetime after JSON/event destruction.
Both browser scripts run through `test-duck-reference-web`.

Native implementations and bounds are described in [JavaScript/native audio](javascript.md).
