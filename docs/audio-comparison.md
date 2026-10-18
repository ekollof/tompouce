# Browser/native audio comparison

Run from the repository root after `npm ci --ignore-scripts` in `js/`:

```sh
make compare-audio
# Or choose an artifact directory:
node scripts/compare-audio.mjs /tmp/my-comparison
```

Requires Chromium, Node with its standard APIs, installed piano catalogs and
recordings, and HTTPS access to strudel.cc and its sample hosts. Set `CHROMIUM`
to an alternative Chromium executable. This is an explicit diagnostic, not a
network-dependent part of `make test`. It creates a disposable browser profile
and uses Chromium's debugging pipe; it does not connect to an existing browser
or the running Tompouce MCP/UI session.
The harness checks `assets/banks` and the standard user-local installation for
catalogs. Set `TOMPOUCE_BANKS` for another installation root.

`tests/render_audio` builds the real native callback and event/sample resolver
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

Twenty regression gates compare at zero lag (no capture shifts or gain
normalization). The original piano windows remain 0.6 seconds; the new synth
and generated-room cases check the full two-second capture:

| Case | Minimum normalized correlation | Native/browser RMS ratio |
| --- | --- | --- |
| Dry C4 piano | 0.99 | 0.85–1.15 |
| Arpoon piano with room disabled | 0.99 | 0.85–1.15 |
| Piano with custom TR909 snare convolution IR | 0.99 | 0.85–1.15 |
| Sine: default, ADSR, low-pass and explicit center pan (4 cases) | 0.99 | 0.85–1.15 |
| Saw, square and triangle at 440 Hz (3 cases) | 0.99 | 0.85–1.15 |
| ZzFX sine and bytebeat (2 cases) | 0.99 | 0.85–1.15 |
| Piano with seeded generated convolution room | 0.99 | 0.85–1.15 |
| Stretched piano: half/fractional speed, upward shift, reverse, loop, envelope/filter (6 cases) | 0.99 | 0.85–1.15 |
| Downward-stretched shared PCM fixture | 0.99 | 0.85–1.15 |

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

Original wet Arpoon and downward-stretched MP3 piano are also captured as diagnostics with
no pass threshold. Ordinary wet Arpoon has independent random impulses and
observed opening correlation around 0.95. The stretched-piano capture compared
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

Downward MP3 piano remains diagnostic (correlation about 0.97). Feeding the
browser-decoded piano to the native renderer gave 0.9999, isolating the remaining
difference to decoded input: the reference's final downward peak region reads
intermediate upper-half real-FFT bins and amplifies small decoding differences.
The maintained downward DSP gate therefore uses identical generated PCM16 WAV
bytes in both players. Its source hash is recorded in the report, and the WAV
and confined native catalog are retained under the artifact directory's
`fixture-banks`. The browser uses that exact WAV in a disposable Blob sample.
The original independently decoded MP3 capture remains visible; this controlled
gate does not establish MP3 decoder or full-song parity.

Use `TOMPOUCE_COMPARE_FILTER=stretch` to select the stretch cases while iterating;
omit it to run the full matrix.

Native implementations and bounds are described in [JavaScript/native audio](javascript.md).
