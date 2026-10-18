# Tompouce example gallery

All snippets are copy-pasteable into the editor (`F5` evaluates) or via
MCP `music_set_code`. Verified against the parser in `src/track.c` /
`src/pattern.c`.

Syntax recap (what actually works today):
- One track per line: `drum|drums|sound|note|notes|n|chord|bass|lead|melody|noise|s <pattern> [| transforms]`
- `bpm N` line sets tempo (clamped 20–400, non-finite rejected).
- Pattern tokens are space-separated steps in one cycle (1 cycle = 4 beats).
- `[ a b ]` groups steps into a single step slot.
- `< a b >` picks one alternative per cycle (`cycle_index % n`).
- `tok*N` repeats a token inside its slot (clamped 1–64). A fractional
  `*` (`bd*1.5`) speeds the step without the integer repeat path.
- `tok!N` splices N copies into the parent (`c!3 e` is four steps).
  `tok@N` is temporal weight (`c@3 e` is 3/4 then 1/4). In `< >`, `@N`
  holds for N cycles.
- `tok/N` is Strudel slow: the pattern stretches across N cycles.
  `[c a f e]/2` plays `c a` on cycle 0 and `f e` on cycle 1.
  `| slow N` is different: the whole track sounds only every Nth cycle.
- `tok?` / `tok?0.2` drops events (degradeBy, default 0.5), seeded by
  the cycle so the lookahead matches playback.
- `a | b` inside the pattern chooses one side per cycle. `|` before a
  transform name is still the track bar, not a choice.
- `bd(3,8)` and `bd(3,8,rot)` are postfix Euclidean rhythms.
  `{bd sd hh, cp lt}` is a polymeter: later lists cycle at the first
  list's step size.
- `~`, `_`, `-`, `rest` are rests. `a,b` stacks both over the same span (a chord).
- Transforms after `|`: `rev`, `fast N` (1–64), `slow N` (1–64),
  `gain X` (0–2), `inst sine|saw|sqr|square|tri|triangle|noise`.
- One pattern per line may also be a call: `note("c e g").s("saw").lpf(800)`,
  `s("bd sd").gain(slider(0.5))`, `setcpm(45)` or `setcpm(120/4)`
  (numeric arguments do `+ - * / %` and parentheses; BPM = cycles/min × 4,
  so `120/4` is 30 cycles/min and 120 BPM). A `/` inside quotes is slow.
  `slider(v)` / `slider(v,min,max,step)` keeps the literal in the
  source; Ctrl+Left / Ctrl+Right nudge the live value.
- Drum tokens: `bd` kick, `sd`/`sn` snare, `hh`/`hat` hat (also the
  fallback for unknown tokens), `cp`/`clap` clap, `tom` tom.
- Note tokens: `c d e f g a b` with `#`/`b` accidentals plus octave
  (e.g. `c4`, `eb4`, `f#3`), or a raw MIDI number (e.g. `60`).
- Default instruments: `bass`→saw, `lead`→square, `notes`/`melody`→tri,
  `noise`→noise, everything else→sine.
- Limits: 32 voices in one buffer, song tracks grow until a safety
  ceiling of 8192. The editor source grows until memory runs out
  (offsets stop near 2 GiB). Long lines wrap on JavaScript tokens;
  arrows follow the wrapped rows. `/wrap off` scrolls
  with the cursor. The code pane
  draws a one-row roll of the cursor line and a one-row scope when
  the pane is tall enough. F12 is the full roll.

## 1. Hello beat

```text
bpm 120
drum bd sd
```

Two steps per cycle: kick, then snare.

## 2. Four on the floor

```text
bpm 124
drum bd bd bd bd
drum ~ sd ~ sd
drum hh hh hh hh | fast 2
bass c2 c2 eb2 g2
```

Classic house skeleton. `fast 2` doubles the hats inside the same cycle.

## 3. Break with fill

```text
bpm 132
drum bd ~ bd [bd bd]
drum ~ sd ~ [sd cp]
drum hh hh hh hh | fast 2
bass c2 ~ eb2 g2 | rev
lead eb4 g4 bb4 c5 | slow 2
```

`[bd bd]` squeezes two kicks into one slot; the lead only plays every
2nd cycle (`slow 2`); `rev` mirrors the bass pattern in time.

## 4. Alternation (`< >`)

```text
bpm 110
drum <bd sd>
bass <c2 eb2 g2 a2>
```

Cycle 0 plays the first alternative, cycle 1 the second, and so on.

## 5. Rests and space

```text
bpm 100
drum bd ~ ~ bd ~ ~ bd ~
bass c2 ~ ~ ~ g1 ~ ~ ~
```

`~` keeps the slot timing but emits nothing. Good for sparse grooves.

## 6. Sub bass + triad lead

```text
bpm 118
bass c2 e2 g2 c3 | inst sine | gain 0.9
lead c4 e4 g4 c5 | inst square | gain 0.5
```

Explicit `inst` overrides the keyword default; gains are clamped to 0–2.

## 7. MIDI numbers

```text
bpm 120
notes 60 62 64 67 | inst tri
bass 36 36 41 43 | inst saw
```

`60` = middle C. Same timing model as note names.

## 8. Ambient pad-ish

```text
bpm 72
notes c4 e4 g4 b4 | slow 2
notes e4 g4 b4 d5 | slow 4
bass c2 ~ ~ ~ | slow 2
```

Long cycle (4 beats at 72 BPM ≈ 3.3 s); `slow` staggers the layers.

## 9. Techno driver

```text
bpm 138
drum bd bd bd bd
drum ~ ~ sd ~
drum hh*4 hh*4 hh*4 hh*4
bass c2 c2 c2 g1 | inst saw
```

`hh*4` repeats the hat 4× inside its slot.

## 10. Noise texture

```text
bpm 90
noise ~ ~ ~ ~ | gain 0.3
drum hh hh hh hh | fast 2 | gain 0.4
bass c2 ~ g1 ~ | slow 2
```

`noise` tracks default to the noise oscillator.

## 11. Full mix template

```text
bpm 124
drum bd ~ bd [bd bd]
drum ~ sd ~ [sd cp]
drum hh hh hh hh | fast 2
bass c2 ~ eb2 g2 | rev
lead eb4 g4 bb4 c5 | slow 2
```

The smoke-test pattern used in CI-style MCP checks; expect
`Loaded 5 tracks.`

## 12. Wavetable voices

```text
bpm 118
drum bd bd ~ bd
drum ~ sd ~ sd
lead c4 e4 g4 b4 | inst wt:pad
bass c2 ~ g1 ~ | inst wt:acid
```

`inst wt:<name>` selects a bandlimited single-cycle table instead of a
naive oscillator. Tables: `sine tri saw square organ acid voice pluck
pad glass`; bare `inst wt` means `wt:saw`. Unknown names fall back to
`saw`.

## 13. Sampled drums

```text
bpm 130
drum bd sd hh cp | bank rolandtr909
```

`bank NAME` matches a directory under `assets/banks` (see F6 search)
and plays one-shot WAVs instead of synthesized drums. Banks with
cryptic file names (e.g. `Dirt-Samples/bd`) map tokens to fixed slots.
Unknown/missing banks fall back to synthesis with a console note.

## 14. Sampled instruments

```text
bpm 100
drum bd sd | bank 909
lead c4 eb4 g4 bb4 | bank Horn/mute
```

`bank` also works on note tracks: the closest recorded pitch is picked
by filename (`_A#1_`, `_D4_`, ...) and shifted to your note. Best with
note-named banks (VSCO2-CE, VCSL); cryptic banks map arbitrarily.

Sampled notes obey note length: each note stops at the next one plus
`rel` release (default 0.08s). Staccato vs legato from notation:

```text
bpm 100
lead c4 e4 g4 bb4 | bank Horn/mute | rel 0.05
lead c4 e4 g4 bb4 | bank Horn/mute | rel 0.6
```

Drum and `s` banks always ring fully.

## 15. Euclidean hats and backbeats

```text
bpm 128
drum euclid(3,8,bd)
drum every(2,sd)
drum euclid(5,8,hh,2)
```

## 16. Mix tools and master fx

```text
bpm 120
drum bd sd
drum hh hh hh hh | mute
bass c2 e2 | solo | att 0.01 rel 0.4
lead c4 e4 | midi 0
fx drive 0.3
fx delay 0.4
```

## 17. One-shot samples

```text
bpm 100
s Dirt-Samples/bd/BT0A0A7.wav Dirt-Samples/sd/rytm-00-hard.wav
```

`s` tokens are bank-relative sample paths (F6 shows them); missing
files are silent.

## 18. Strudel tonal line

```text
bpm 96
chord <Cm7 F7 Bb^7 Eb^7> | sound sine | adsr .01:.2:.5:.3
n 0 2 4 7 | scale C:minor | arp 0 1 2 1 | sound sawtooth
lead c4 e4 g4 | vib 5:.2 | lpf 1800
```

Comma chords (`g3,b3,e4`) and `[g3,b3,e4]` are the same stack.
`| scale C:minor` reads plain integers as degrees (root defaults to
octave 3, so `0` is C3). On a scaled track, write raw MIDI as a note
name (`c4`). `chord` uses ireal symbols: `C` is major, `C7` is
dominant, `C^7` is major-7, `C-7` is minor-7. On `notes`/`n`/`lead`,
`c7` stays the note and `cm7` is a chord. Voicings are close position.
`| arp` indexes that stack (`0 [0,2] 1`). `| lfo gain sine 2 .4` and
`| vib 4:.1` (sine pitch) are one LFO per track. Pink and brown select
the same noise as white. `| lpf` is a per-voice cutoff in Hz; `fx cutoff`
is still the master filter.
