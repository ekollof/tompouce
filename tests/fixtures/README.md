These 31 examples were retrieved from https://strudel.cc/examples/ on
2026-10-17. `strudel-examples.json` preserves the code embedded in the page's
REPL links without modification, with titles and SHA-256 source hashes.
Original author and license notices remain in each example. Many are by
Felix Roos under CC BY-NC-SA 4.0; some carry different or no explicit notices.
No blanket license is asserted for the collection.

`strudel-example-events.json` records Node/V8 evaluation with the pinned
upstream Strudel libraries and the Tompouce host adapter. Each example
runs in a fresh process; cycles 0, 1, and 16 capture timing, pitches, sample
indices, gains, filters, probabilistic transforms, tempo, sample declarations,
visual requests, and orchestra declarations. Native QuickJS results must
match these stored events and metadata, with a small numeric tolerance.
This checks runtime/bridge regressions, not browser audio waveform parity.
The native suite additionally checks DSP output remains finite. Separate
sampler and Csound tests check sample roots/levels, filters, and rendered audio.
Native Csound tests fetch the current orchestra used by Lounge sponge;
sample-loader tests also need network access. The event corpus itself is offline.

Updating the fixtures is an explicit maintenance step, never part of `ctest --preset default`:

```sh
node js/example-corpus.mjs > tests/fixtures/strudel-examples.json
node js/example-reference.mjs > tests/fixtures/strudel-example-events.json
ctest --preset default
```

Review source, version, and event changes before accepting a new baseline.
Run `cmake --build build --target js-bundle` first if the adapter or pinned dependencies changed.

`mp3-gapless.h` contains synthetic half-second recordings (no third-party
music), encoded with ffmpeg/libmp3lame and converted to C arrays with `xxd -i`:

```sh
ffmpeg -f lavfi -i 'aevalsrc=0.25*sin(2*PI*440*t)|0.125*sin(2*PI*660*t):s=44100:d=0.5' -codec:a libmp3lame -b:a 128k stereo.mp3
ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=22050:duration=0.5' -codec:a libmp3lame -b:a 48k mono.mp3
```

The sampler tests check original duration, attack alignment, stereo,
resampling of MPEG-2 mono, absent gapless tags, corrupt frame counts,
and truncated input. These fixtures decode offline without ffmpeg at test time.
