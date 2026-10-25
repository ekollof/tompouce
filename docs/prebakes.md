# Prebake setup files

Prebakes define reusable helpers and setup before a song is evaluated. They
run in the same bounded QuickJS sandbox as the song, with Strudel mini-notation
transpilation and top-level `await`. Helpers can use `window` (an alias for
the JavaScript global), `register`, `Pattern.prototype`, sample maps and tempo
settings. Setup expressions and labelled patterns do not start playing tracks.

## Switch Angel starter

Download the optional starter with:

```sh
tompouce --install-prebake switchangel
```

This fetches [Switch Angel's upstream prebake](https://github.com/switchangel/strudel-scripts/blob/706dd24814ac86193ceec6d0b263f432cba6e5ff/prebake.strudel)
at revision `706dd24814ac86193ceec6d0b263f432cba6e5ff`, preserving its source and
attribution under `$XDG_CONFIG_HOME/tompouce/prebakes/switchangel.strudel`
(normally `~/.config/tompouce/prebakes/switchangel.strudel`). Existing files are
never overwritten. The downloaded script is not redistributed in Tompouce's
repository; upstream currently declares no redistribution license.

Downloading installs the file without enabling it. Restart Tompouce after
installing a version that adds prebake support, then use the activation line
below and press F5.

Enable it for a song:

```js
await loadPrebake('switchangel.strudel')
note("c3 e3 g3").s('sawtooth').acidenv(.4).gain(.5)
```

The starter's browser **Volume** widget maps to Tompouce's **F4 Volume** slider.
Drag its bar, use the mouse wheel over its row, or click it and use Left/Right
or Ctrl+Left/Right for 1% steps. Escape returns focus to the editor. From the
F2 prompt, `/volume .5` sets the output to half amplitude, `/volume 0` mutes,
and `/volume 1` restores unity. `/volume` reports the current setting.
Alt+Up/Down raises/lowers output volume by 1% from any pane, even with F4 closed.

This is linear attenuation on Tompouce's final stereo output, after voices,
Csound, effects and limiting. It changes held notes and effect tails on the
next audio block across PipeWire, PulseAudio, JACK and ALSA. It uses the same
native slider rendering as editor controls but does not rewrite source or
reevaluate music. External MIDI devices are unaffected.

The level starts at unity and persists across evaluations and song changes
for the current process; restarting restores unity. Loading the prebake keeps
the current output level. The slider remains available in F4 without a
prebake. Other custom browser `UISlider` callbacks are not mapped automatically.

Its gain defaults and quadratic gain curve are active: `.gain(.5)` becomes
`.25` before velocity is applied. Enabling the starter can therefore change
levels in existing songs. The curve also follows the REPL for velocity,
postgain, shape/distortion volume, delay send and tremolo depth.
The output Volume control is downstream of that curve: `/volume .5` always
halves the final amplitude.

The full file loads in QuickJS. Maintained web checks cover orbit mute/solo,
cue selection, arrangements, fills, note/scale/arpeggio helpers, parameter
locks, pan, delay, accents, sample timing, text-to-synth (`stxt`), linear glide,
and seeded humanize/glitch controls, as well as the original filter helpers.
Native `TextEncoder` supplies UTF-8 encoding, including Unicode replacement
and `encodeInto`; it supplies no DOM APIs. The public
`getFrequencyFromValue` pitch helper follows the web's note/frequency fallbacks.
This does not establish compatibility for every helper: browser DOM controls,
external MIDI devices, unavailable samples/synths and effect APIs such as
`bmod`/`FX` still require their own support. Calling an unavailable API retains
its error; the native profile does not substitute a different sound for it.

## Your own files

Put a named setup file in `~/.config/tompouce/prebakes/` and load it with
`await loadPrebake('helpers.strudel')`. `.strudel`, `.js` and `.mjs` are accepted
as setup scripts; ES modules remain a separate facility under `modules/`.

For automatic setup, create `~/.config/tompouce/prebake.strudel`. For example:

```js
await loadPrebake('switchangel.strudel')
```

Or define helpers directly:

```js
window.lead = () => note("c3 e3 g3").s('supersaw')
register('doubletime', pattern => pattern.fast(2))
```

Then use `lead().doubletime()` in the editor. Ordinary local declarations stay
inside their setup closure; publish reusable names through `window` or
`register`. `strudelScope` is available for helpers that use that convention.
`setGainCurve`, `setDefault`, `setDefaultValue(s)`, `getDefaultValue` and the
default-reset helpers support native sound preparation. External MIDI, OSC
and Csound routing bypass these Superdough gain curves.

Each successful song evaluation gets a fresh runtime and reloads its setup.
Editing a file does not mutate an already accepted program. A setup failure
rejects the new evaluation and retains the previous playable program. Sample
maps and Csound loads continue through their existing guarded preparation paths.
Prebake mini-notation offsets never highlight unrelated song text; prebake
sliders have no editor chip because their source is outside the song.

File loading is allowed only during evaluation. Absolute paths, parent
traversal, symlinks and non-regular files are rejected. Files are limited to
256 KiB each, at most 16 named files and 1 MiB total per compiled program.
Repeated named loads run once per program; nested cycles do not rerun a file.
The existing runtime memory and execution budgets remain in force. The setup
loader grants no access to arbitrary files, processes, browser DOM or networking.

## Sharing, disabling and troubleshooting

Named song saves contain the song source, not the contents of local prebakes.
When sharing a song that calls `loadPrebake`, tell the recipient which file and
upstream revision to install. Keep attribution with any setup files you share.

To disable a per-song prebake, remove its `await loadPrebake(...)` line and
evaluate again. For automatic setup, remove the corresponding line from
`prebake.strudel`, or rename that startup file, then reevaluate. Accepted voices
continue under the existing playback rules; editing a setup file alone does
not replace the playing program.

| Symptom | What to check |
| --- | --- |
| `loadPrebake` is unknown | Restart the updated executable; check `command -v tompouce` if a checkout and installed command differ. |
| A named file is missing or rejected | Files belong under `$XDG_CONFIG_HOME/tompouce/prebakes/`; use a relative name, not a URL or absolute path. Symlinks and files over 256 KiB are refused. |
| The starter installer refuses an existing file | It never overwrites edits. Keep or back up the existing file before replacing it deliberately. |
| A helper is unknown | Await the loader before using it. In your own setup, export helper names through `window` or `register`. |
| A setup edit has no effect | Press F5 to create a fresh runtime; old accepted programs retain their original helpers. |
| Sound levels change after enabling Switch Angel | Its quadratic curve and gain defaults are intentional; adjust the song's explicit gain. |
| Everything stays silent after muting | Check F4 Volume or enter `/volume 1`. This level survives song changes and reevaluation. |
| `document`, `bmod` or `FX` is unavailable | The helper requires a browser or an unsupported audio API. Only the starter's known Volume widget is mapped; see the limitations above. |

## Verification

```sh
cmake --build build --target test-prebake-reference-web
cmake --build build --target test-security test-security-sanitize
TOMPOUCE_COMPARE_FILTER=gaincurve node scripts/compare-audio.mjs
TOMPOUCE_COMPARE_FILTER=custom-default node scripts/compare-audio.mjs
```

The web gate uses the deployed REPL and its actual transpiler, comparing 81
setup/helper/API cases with the native QuickJS file-loading path, including
multi-cycle arrangements and ten branch solo/identity/transform cases. It records the
upstream starter revision/hash, browser transpiler hash and native bundle hash.
Existing exact event checks remain exact. New text-synth/random/glide and frequency
checks allow at most 1e-12 absolute numeric error, with exact structure and
event counts. Helper-specific delay, velocity and postgain checks disable the
gain curve to inspect their raw controls. Gain shaping keeps its separate
browser audio gates. Three more audio gates cover glide, accent and delay
with the complete pinned starter, using the existing 0.99 correlation and
0.85–1.15 gain limits. Glide additionally exposed and fixed pitch-envelope
defaults, minimum segment durations and first-quantum oscillator automation.
Nine bare-REPL pitch/vibrato audio checks cover those changes independently.
The oneshot event check uses a stopped zero-time clock; live evaluation/grid
timing, nonlinear pitch curves, custom FX/bmod routing and external devices
remain separate checks rather than claims established by this matrix.
