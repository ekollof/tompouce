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

Current regression gates compare the first 0.6 seconds at zero lag:

| Case | Minimum normalized correlation | Native/browser RMS ratio |
| --- | --- | --- |
| Dry C4 piano | 0.99 | 0.85–1.15 |
| Arpoon piano with room disabled | 0.99 | 0.85–1.15 |
| Piano with custom TR909 snare convolution IR | 0.99 | 0.85–1.15 |

The C4 capture lasts two seconds and Arpoon captures four cycles/eight seconds.
Only the stated window is gated; these checks do not establish full-song
parity. The observed dry correlations were 0.99999994 and 0.999986 respectively.
Any non-finite or silent window fails. No best-lag search, time shift, or gain
normalization hides timing/level differences.

The original wet Arpoon piano is captured too, as a diagnostic with no pass
threshold. Native Schroeder reverb and browser convolution reverb differ;
browser generated impulses also vary. Observed wet opening correlation was
approximately 0.93–0.95. A dry pass does not close this reverb gap. The custom
IR check observed correlation 0.99999997 and native/browser RMS ratio 0.9931.
