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

Kabelsalat `audioin()` follows the web worklet: it reads the first input channel
into one graph, with `.out()` providing both outputs by default. Explicit
`.out(0)`/`.out(1)` calls select graph output channels. For these effects,
the graph runs after the source envelope and before event gain and native DSP.
Both graph outputs receive downstream effects independently. In addition,
`.fxr(seconds)` retains and fades tails after the source release (bounded to
60 seconds). This does not establish support for arbitrary nested `FX` chains.

The synth category includes 125 `gm_` names (General MIDI, no FluidSynth required):

```js
note("c4 e4 g4").s('gm_flute').gain(0.4)
```

`gm_` names play the same per-instrument sample packs as the Strudel web REPL
(MIT-licensed WebAudioFont data, preinstalled by the installer under
`assets/banks/gm-webaudiofont/`; fetched on first play when missing).
GM playback is local; no external MIDI connection is required. Without the
packs and without network access, `gm_` sounds fall back to FluidSynth when
available: the installer includes FluidR3 GM, and `TOMPOUCE_SOUNDFONT`
selects another supported SF2/SF3 file at startup. Native GM plays the web's
default (first) soundfont variant per name; the browser's additional `.n()`
soundfont variants are not replicated.

## Wavetables

The wavetable category contains 65 Strudel `wt_` banks with 4,358 variants, alongside procedural `wt:` oscillators:

```js
note("c4 e4 g4").s('wt_flute').n(3).gain(0.4)
```

Installed `wt_` banks work offline, including code using `samples('bubo:waveforms')`. Native playback loops single-cycle waveforms at the requested pitch. Browser wavetable morph/warp processing is not implemented. AKWF attribution is retained in the project notices.

## Import your own sounds

In F6 choose import-sounds, paste a folder path and press Enter. The companion offers a folder chooser. Supported WAV, MP3 and FLAC files are copied into Tompouce's config sound storage; each folder becomes a sound name with alphabetical file slots. Imports persist across sessions and appear under user sounds.

Import limits are 8 MiB per file, 256 MiB/2,048 files in total and 256 sound names. Reimporting the same filename replaces its slot. Imported content retains its original licensing obligations.

### Banks with strudel.json

A folder whose root holds a `strudel.json` manifest imports as a bank instead
of a flat file walk: sound names, array order and pitch regions follow the
manifest, like Strudel's `samples()` maps. Supported value forms are single
files, flat arrays and one-level pitch-keyed maps (`{"c2": "low.wav"}`), with
top-level and per-entry `_base` subdirectories; audio references stay inside
the folder (relative subdirectories allowed). Nested JSON objects, remote
bases/URLs, empty groups and unparseable pitch keys are rejected, and an
invalid manifest fails the import instead of falling back to the file walk.
Extension matching ignores case, unlike the browser.
Three deliberate divergences from upstream's `processSampleMap`, verified with
`scripts/samplemap-upstream-check.mjs`: a malformed value aborts the whole
upstream map (order-dependent partial registration) while native skips
just that entry; upstream registers `_`-prefixed keys (including `_base`
itself) as broken sounds while native drops them at import; both sides agree
on forms, base prefixing, nearest-key first-wins selection and `n` wrapping.

Manifest imports own the names they define: reimporting replaces the bank's
slots, drops names the manifest no longer defines, and collects unreferenced
audio. Loose file imports into manifest-owned names are refused. Manifests
are capped at 2 MiB / 8,192 references; the stored index stays schema
versioned (currently 2) so older builds ignore newer stores rather than
misreading them.

### GitHub banks for offline use

Typing `github:user/repo[/branch][/path]` (or `--github`) in the import box
fetches that repo's `strudel.json` plus every referenced file over HTTPS and
stores the bank exactly like a local manifest import, for offline playback.
Only `raw.githubusercontent.com` is ever contacted (HTTPS, pinned-host
redirects, 45 s timeout); specs mirror the web `samples('github:…')` form
with the same user/repo defaults. A local folder literally named `github:*`
is unreachable through folder import; rename it.

See the [native audio reference](https://github.com/ekollof/tompouce/blob/master/docs/javascript.md) for sample-map syntax, cache bounds, supported effects and decoder behavior.
