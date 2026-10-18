# Sounds and samples

## Browse with F6

F6 opens a search bar above category tabs: samples, drum-machines, synths, wavetables, user sounds, import-sounds, plus All/local-banks. Left/Right changes category while preserving the query.

Manifest arrays and pitch regions appear as one sound entry. Drum-machine components are grouped under their bank. Enter opens a bank or inserts a sound; Tab inserts a selected bank using its listed instruments. Standalone audio appears only when no loaded manifest references it. Insertion replaces the source selection and requires F5 to evaluate.

```js
s("bd sd, hh*8").gain(0.4)
s("bd sd").bank('RolandTR909').gain(0.4)
note("c4 e4 g4").s('piano').gain(0.4)
```

Array indices follow manifest order. `.n()` selects variants; pitched maps choose a recording region for the requested pitch. Explicit `samples()` maps take priority over imports and default banks.

## Built-in and GM synths

Built-in sources include sine, triangle, sawtooth, square, supersaw and noise, plus native ZzFX, bytebeat and a bounded Kabelsalat host. Basic synths need no sample downloads.

With optional FluidSynth and a GM soundfont, the synth category includes 125 `gm_` names:

```js
note("c4 e4 g4").s('gm_flute').gain(0.4)
```

GM playback is local; no external MIDI connection is required. The installer includes FluidR3 GM. `TOMPOUCE_SOUNDFONT` selects another supported SF2/SF3 file at startup. Native GM provides one soundfont voice per name; the browser's additional `.n()` soundfont variants are not replicated.

## Wavetables

The wavetable category contains 65 Strudel `wt_` banks with 4,358 variants, alongside procedural `wt:` oscillators:

```js
note("c4 e4 g4").s('wt_flute').n(3).gain(0.4)
```

Installed `wt_` banks work offline, including code using `samples('bubo:waveforms')`. Native playback loops single-cycle waveforms at the requested pitch. Browser wavetable morph/warp processing is not implemented. AKWF attribution is retained in the project notices.

## Import your own sounds

In F6 choose import-sounds, paste a folder path and press Enter. The companion offers a folder chooser. Supported WAV, MP3 and FLAC files are copied into Tompouce's config sound storage; each folder becomes a sound name with alphabetical file slots. Imports persist across sessions and appear under user sounds.

Import limits are 8 MiB per file, 256 MiB/2,048 files in total and 256 sound names. Reimporting the same filename replaces its slot. Imported content retains its original licensing obligations.

See the [native audio reference](https://github.com/ekollof/tompouce/blob/master/docs/javascript.md) for sample-map syntax, cache bounds, supported effects and decoder behavior.
