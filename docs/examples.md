# Tompouce example gallery

These are complete JavaScript Strudel buffers for the pinned 1.2.6 runtime.
Paste one example into the editor and press F5; Ctrl+P toggles playback.
With MCP, `music_set_code` replaces and evaluates source; `music_play` starts
transport. `music_evaluate` reevaluates the current source.
F5 evaluates music; Ctrl+S saves the whole song.

Examples using `bd`, `sd`, `hh`, `cp`, `piano`, `RolandTR909` or `wt_` sounds
need the corresponding default banks from `./install.sh`. Built-in synth
examples work offline without banks. F6 shows the available sound catalog.

## Syntax to keep nearby

- `setcpm(120 / 4)` gives 30 cycles per minute, equivalent to 120 BPM with
  four beats per cycle. More steps divide the same cycle rather than extend it.
- `note("c4 e4 g4")` sequences pitches; `.s('triangle')` chooses the sound.
  MIDI note numbers also work. `s("bd sd")` sequences sample sounds.
- `$:` and named labels such as `bass:` play concurrent branches. Rename a
  label to `bass_:` to mute it. A song buffer can contain several branches.
- Spaces sequence steps, `~` rests, `[]` subdivides, `<>` alternates across
  cycles, and commas stack events. `*` repeats inside a slot; `!` adds copies
  at the same level; `@` changes relative duration.
- `.fast(2)` doubles a branch's speed; `.slow(2)` stretches its pattern over
  two cycles. It does not silence every other cycle.
- Controls have their own pattern timing: two gain values span half a cycle
  each. Use four values to alternate gain on a four-note sequence.
- `.every(4, x => x.rev())` transforms cycles 0, 4, 8, ... in this runtime.
- Use `.add()` on raw mini-pattern values inside `n(...)` or `note(...)`;
  use `.transpose()` to shift resolved notes. Bare `n(...).add(7)` is invalid
  control-pattern arithmetic.

The native example test compiles these Markdown snippets and queries their
events directly. See the [JavaScript/audio reference](javascript.md) for
supported controls and bounds, or the [handbook](wiki/Home.md) for setup.

## 1. Hello beat

```js
setcpm(120 / 4)
s("bd sd").gain(0.5)
```

Two equally spaced steps per cycle: kick, then snare. Requires default drums.

## 2. Four on the floor

```js
setcpm(124 / 4)
kick: s("bd*4").gain(0.5)
snare: s("~ sd ~ sd").gain(0.4)
hats: s("hh*8").gain(0.2)
bass: note("c2 c2 eb2 g2").s('sawtooth').lpf(600).gain(0.3)
```

Four kicks and eight hats share each cycle. Labels collect the four parts;
they do not create separate song buffers.

## 3. Break with fill

```js
setcpm(132 / 4)
kick: s("bd ~ bd [bd bd]").gain(0.5)
snare: s("~ sd ~ [sd cp]").gain(0.4)
hats: s("hh*8").gain(0.2)
bass: note("c2 ~ eb2 g2").rev().s('sawtooth').lpf(700).gain(0.3)
lead: note("eb4 g4 bb4 c5").slow(2).s('triangle').gain(0.3)
```

The final kick slot holds two kicks. The lead takes two cycles to finish its
four notes; the bass is reversed in time.

## 4. Alternation across cycles

```js
setcpm(110 / 4)
drums: s("<bd sd>").gain(0.5)
bass: note("<c2 eb2 g2 a2>").s('sawtooth').lpf(600).gain(0.3)
```

The drums alternate kick/snare each cycle; the bass follows a four-cycle loop.

## 5. Rests and space

```js
setcpm(100 / 4)
drums: s("bd ~ ~ bd ~ ~ bd ~").gain(0.5)
bass: note("c2 ~ ~ ~ g1 ~ ~ ~").s('sine').gain(0.4)
```

A rest occupies time without emitting a note. Removing a slot instead changes
how the remaining steps divide the cycle.

## 6. Sub bass and lead

```js
setcpm(118 / 4)
bass: note("c2 e2 g2 c3").s('sine').gain(0.4)
lead: note("c4 e4 g4 c5").s('square').lpf(2000).gain(0.2)
```

This example needs no banks. Set the instrument explicitly with `.s()` rather
than relying on historical line-dialect keywords.

## 7. MIDI note numbers

```js
setcpm(120 / 4)
melody: note("60 62 64 67").s('triangle').gain(0.4)
bass: note("36 36 41 43").s('sawtooth').lpf(600).gain(0.3)
```

MIDI 60 is C4. These numbers select local synth pitches; they do not route
events to an external MIDI device.

## 8. Ambient pads

```js
setcpm(72 / 4)
pad: note("[c4,e4,g4,b4] [e4,g4,b4,d5]").slow(4).s('triangle')
  .attack(0.5).release(0.8).room(0.2).gain(0.15)
bass: note("c2 ~ g1 ~").slow(2).s('sine').gain(0.3)
```

Commas make simultaneous notes. The pad's two chords span four cycles;
attack, release and reverb soften their edges.

## 9. Techno driver

```js
setcpm(138 / 4)
kick: s("bd*4").gain(0.5)
snare: s("~ ~ sd ~").gain(0.4)
hats: s("hh*4 hh*4 hh*4 hh*4").gain(0.15)
bass: note("c2 c2 c2 g1").s('sawtooth')
  .lpf("400 800 400 1200").decay(0.15).sustain(0).gain(0.35)
```

Each `hh*4` repeats inside one of four outer slots, giving sixteen hats per cycle.

## 10. Noise texture

```js
setcpm(90 / 4)
texture: note("c4*4").s('pink').hpf(1000).lpf(5000)
  .attack(0.05).release(0.2).gain(0.15)
bass: note("c2 ~ g1 ~").slow(2).s('sine').gain(0.35)
```

The noise branch has actual events, so it is audible. Replace pink with white
or brown to compare the native noise sources. No banks are required.

## 11. Full mix with live controls

```js
setcpm(124 / 4)
kick: s("bd ~ bd [bd bd]").gain(0.5)
snare: s("~ sd ~ [sd cp]").gain(0.4)
hats: s("hh*8").gain(0.15)
bass: note("c2 ~ eb2 g2").s('sawtooth')
  .lpf(slider(700,200,3000,100)).gain(0.3)
lead: note("eb4 g4 bb4 c5").slow(2).s('triangle')
  .decay(0.2).sustain(0).delay(0.15).gain(0.3)._pianoroll()
```

F5 registers the slider; Ctrl+Left/Right adjusts its live value without
reevaluating source or restarting playback. Upcoming notes use the new value.
The inline roll follows the lead's final two-cycle timing.

## 12. Strudel wavetable banks

```js
setcpm(118 / 4)
lead: note("c4 e4 g4 b4").s('wt_flute').n("0 1 2 3").gain(0.3)
bass: note("c2 ~ g1 ~").s('wt_bw_saw').n(0).lpf(600).gain(0.3)
```

Requires the installed AKWF `wt_` banks. F6's wavetables category lists each
manifest bank once; `.n()` selects a waveform variant in manifest order. Native
playback loops each single-cycle waveform at the note pitch. Browser morph/warp
processing is not implemented.

## 13. Drum-machine manifests

```js
setcpm(130 / 4)
s("bd sd hh cp").bank('RolandTR909').gain(0.5)
```

Use the catalog's exact bank name. The installed drum-machine manifest controls
instrument selection and variant order; directory-name guesses are unnecessary.

## 14. Sampled piano and note gates

```js
setcpm(100 / 4)
short: note("c4 e4 g4 bb4").s('piano').release(0.05).gain(0.35)
long: note("c3 ~ g3 ~").s('piano').release(0.6).gain(0.3)
```

Requires the installed piano catalog. The sampler selects the nearest recorded
pitch region and repitches it to the requested note. Explicit `.release()` uses
the musical event duration as the gate, followed by the specified release tail;
an unlooped source can still end naturally before the envelope does.

## 15. Euclidean rhythms

```js
setcpm(128 / 4)
kick: s("bd(3,8)").gain(0.5)
snare: s("~ sd ~ sd").gain(0.4)
hats: s("hh(5,8,2)").gain(0.2)
```

`(pulses,steps,rotation)` distributes events over a grid. Rotation changes
their placement; it does not add pulses.

## 16. Mixing branches and effects

```js
setcpm(120 / 4)
drums: s("bd sd").gain(0.4)
hats_: s("hh*8").gain(0.15)
bass: note("c2 e2").s('sawtooth').lpf(700)
  .attack(0.01).release(0.4).shape(0.1).gain(0.3)
lead: note("c4 e4").s('triangle').delay(0.15).room(0.1).gain(0.3)
```

The trailing underscore mutes hats. Rename `hats_:` to `hats:` and press F5
to enable them. Gains and effects are expressed on the pattern branches;
shared orbit delay/reverb can affect other branches using the same orbit.

## 17. One-shots and reverse playback

```js
setcpm(100 / 4)
s("bd:0 ~ sd:0 ~").speed("<1 -1>").gain(0.5)
```

`:0` selects a manifest variant. Positive speed plays forward; negative speed
plays backward. The speed alternates across cycles. These one-shots use the
installed sample catalog, not hard-coded paths to filenames that may be absent.

For your own files, import a folder through F6's import-sounds category and
insert its generated sound name. Imports persist across sessions.

## 18. Scale arithmetic and semitone transposition

```js
setcpm(96 / 4)
degrees: n("0 2 4".add("<0 3 4 0>"))
  .scale('C:major').s('triangle').gain(0.3)
melody: note("c5 e5 g5").transpose("<0 5 7 0>").s('sine')
  .vib(5).vibmod(0.2).gain(0.3)
```

The first branch adds scale degrees before resolving pitches: C-E-G, F-A-C,
G-B-D, then C-E-G. The second transposes note pitches by semitones. Both
branches form a four-cycle progression; vibrato adds small pitch movement.

Continue with [Strudeltutor](wiki/Live-Coding-and-Strudeltutor.md) and the
[official Strudel learning resources](wiki/Learn-Strudel.md).
