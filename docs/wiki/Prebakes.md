# Prebake setup files

Prebakes define reusable Strudel helpers and setup before a song is evaluated.
They support mini-notation, top-level `await`, `register`, `window` globals,
sample maps and tempo settings. Setup patterns do not start playing tracks.

## Start with Switch Angel

After updating Tompouce, download the optional starter:

```sh
tompouce --install-prebake switchangel
```

The command installs a pinned [Switch Angel prebake](https://github.com/switchangel/strudel-scripts/blob/706dd24814ac86193ceec6d0b263f432cba6e5ff/prebake.strudel)
under `~/.config/tompouce/prebakes/switchangel.strudel`, or the equivalent
`$XDG_CONFIG_HOME/tompouce/prebakes/` directory. Existing files are never
overwritten. Downloading does not enable the starter.

Restart the updated executable, then put this in a song and press F5:

```js
await loadPrebake('switchangel.strudel')
note("c3 e3 g3").s('sawtooth').acidenv(.4).gain(.5)
```

The starter's browser Volume widget maps to **F4 Volume**, a native slider for
Tompouce's final stereo output. Drag the bar, use the wheel over its row, or
click it and use Left/Right or Ctrl+Left/Right for 1% steps. Escape returns
focus to the editor. F2 `/volume .5` halves the output, `/volume 0` mutes,
`/volume 1` restores unity, and `/volume` reports the current value.
Alt+Up/Down raises/lowers output volume by 1% from any pane, even with F4 closed.

Output volume changes held notes, Csound and effect tails on the next audio
block across PipeWire, PulseAudio, JACK and ALSA. It does not edit source or
reevaluate music. The level starts at unity and survives evaluations and song
changes until the process exits. Loading the starter keeps the current level;
the F4 control is also available without a prebake. Other custom browser
`UISlider` callbacks remain unsupported.

Its quadratic gain curve is active: `.gain(.5)` becomes `.25`
before velocity is applied. Enabling it can change levels in existing songs.
The final output Volume control is linear and downstream of this curve.

Maintained comparisons cover 81 setup/helper/API cases, including branch
solo/identity/transforms, cueing,
arrangements, orbit mute/solo, parameter locks, scale/arpeggio helpers, sample
timing, text-to-synth, linear glide and seeded humanize/glitch controls.
Native UTF-8 `TextEncoder` and the web's `getFrequencyFromValue` helper are
available. Glide, accent and delay also have browser audio comparisons.
Linear/exponential oscillator pitch has 19 browser audio gates. Live
oneshot/grid timing and sample/worklet pitch automation need separate checks.
Other helpers may require unavailable
samples, external MIDI devices or browser/audio APIs such as `bmod` and `FX`.
An unavailable API retains its error; full helper compatibility is not claimed.

## Automatic setup and your own helpers

Create `~/.config/tompouce/prebake.strudel` to run setup before every song
evaluation. For example, its contents can be:

```js
await loadPrebake('switchangel.strudel')
```

Or define your own helpers in that file:

```js
window.lead = () => note("c3 e3 g3").s('supersaw')
register('doubletime', pattern => pattern.fast(2))
```

Then use `lead().doubletime()` in the editor. Local declarations stay in their
setup closure; publish reusable names through `window` or `register`.
`window` is a global alias and does not provide a browser DOM.

Additional named files belong under `~/.config/tompouce/prebakes/`; load them
with `await loadPrebake('helpers.strudel')`. `.strudel`, `.js` and `.mjs` are
accepted as setup scripts. Local ES modules use the separate `modules/` directory.
The paths above respect `XDG_CONFIG_HOME` when it is set.

Each evaluation reloads setup in a fresh runtime. Editing a file alone does
not mutate the playing program; press F5 to apply changes. A setup failure
retains the previous playable program. External-file mini-notation does not
highlight unrelated song text, and prebake sliders have no editor chip.

## Limits, sharing and disabling

Files are confined to the setup directories. Absolute paths, parent traversal,
symlinks and non-regular files are rejected. Limits are 256 KiB per file,
16 named files and 1 MiB total per program. Named files run once per program;
loading is disabled during pattern queries. Runtime memory and execution
budgets still apply.

Song saves contain the song source, not local prebake contents. Tell recipients
which files and upstream revision to install, and retain attribution.

To disable setup, remove the song's `await loadPrebake(...)` line or remove it
from the automatic startup file, then press F5. Renaming `prebake.strudel` also
disables automatic setup on the next evaluation.

If the loader is unknown, restart the updated executable and check
`command -v tompouce`. If a helper is unknown, check the file location and await
its loader before use. Installer refusal of an existing file preserves edits;
back it up before deliberately replacing it. See [Troubleshooting](https://github.com/ekollof/tompouce/wiki/Troubleshooting)
for evaluation and executable-version issues.
