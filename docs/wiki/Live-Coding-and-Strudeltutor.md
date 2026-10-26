# Live coding and Strudeltutor

## Editing and transport

| Key | Action |
| --- | --- |
| F1 | Keybinding reference |
| F2 | Prompt; enter slash commands such as `/tutor` or `/save` |
| F3 | Toggle vi editing |
| F4 | Engine and editor diagnostics |
| F5 | Evaluate source, commit it to the current song buffer, and start playback |
| Ctrl+Enter / Alt+Enter | Evaluate and start, matching the Strudel REPL. Plain Enter still inserts a newline |
| Ctrl+. / Alt+. | Stop playback, matching the Strudel REPL |
| Alt+W / Alt+Q | Next / previous `$` label, wrapping, as in the web editor |
| Ctrl+P | Play/stop |
| Alt+Up / Alt+Down | Raise/lower output volume by 1% from any pane |
| Tab / Ctrl+Space | Context-aware completion; leading whitespace/selection retains indentation |
| Shift+Tab | Previous completion or unindent |
| Ctrl+Z / Ctrl+Y | Undo/redo; vi normal mode also uses `u` / Ctrl+R |
| Ctrl+S / Ctrl+O | Save/load the whole song |
| Ctrl+N / Ctrl+B | Next/previous song buffer |
| Ctrl+T | Duplicate the current buffer into a new song buffer |
| F9 | Song buffer list |
| Ctrl+Q | Quit and save recovery state |

F5 evaluates; it does not save the named project file. A failed evaluation retains the last playable pattern. Ctrl+Q recovery state is also separate from a deliberate project save.

F4 includes a Volume slider for the final output. Drag it, use its mouse wheel,
or click it and use arrows for 1% steps; Escape returns editor focus. F2
`/volume .5` sets half amplitude and `/volume 1` restores unity. This changes
held notes and effect tails immediately, without editing or evaluating source.
The session's level survives song changes and reevaluation; restart restores unity.

In vi normal mode, `:` opens local commands, including `:w`, `:e`, `:q`, `:help`, `:tutor` and `:set wrap`. Tab completes commands, settings and JSON paths. Unsaved-change guards protect commands such as `:e` and `:q`; use `!` only when you intend to discard edits. No shell commands run through this prompt.

## Reusable setup

For reusable helpers shared across songs, see [Prebake setup files](https://github.com/ekollof/tompouce/wiki/Prebakes).
That guide covers automatic startup setup and the optional Switch Angel starter.

## Work through the tutorial

F2 `/tutor` opens a working copy of the 22 bundled lessons. In vi normal mode, `:tutor` does the same. The copy lives at `$XDG_CONFIG_HOME/tompouce/strudeltutor.json`, normally `~/.config/tompouce/strudeltutor.json`.

Existing copies are reused so progress survives updates. `/tutor!` discards unsaved editor changes, but does not reset the saved tutorial. Save progress with Ctrl+S, vi-normal `:w`, or F2 `/save`. To keep another copy, use `/save ~/strudeltutor-work.json`.

Lesson 01 starts silently: change its `gain(0)` to `gain(0.5)` and press F5. Keep song mode off with `/song off`, press F5 before switching lessons, and use Ctrl+N/Ctrl+B to navigate. Ctrl+S saves all lesson buffers. The lessons cover rhythm, scales, layering, envelopes, filters, signals, arithmetic, effects and recovery.

## Pattern essentials

A cycle is a repeating time container. `setcpm(108 / 4)` gives 27 cycles per minute; with four beats per cycle this is 108 BPM. Adding more notes divides the same cycle into more slots.

```js
note("c4 ~ [e4 g4] <c5 b4>").s('triangle').gain(0.4)
```

Spaces sequence steps, `~` rests, `[]` subdivides a slot, `<>` alternates over cycles, and commas play patterns together. Add `$:` or named labels for concurrent branches:

```js
melody: note("c4 e4 g4 c5").s('triangle').gain(0.3)
bass: note("c2 g2").s('sine').slow(2).gain(0.4)
```

A leading or trailing underscore mutes a named label, such as `bass_:`. This is separate from song mode, which sequences whole buffers. F7 enables song mode; F8 holds the current buffer.

## Arithmetic and control timing

Apply `.add()` to raw mini-pattern values before wrapping them in a control:

```js
n("0 2 4".add("<0 3 4 0>"))
  .scale('C:major').s('triangle').gain(0.4)
```

These additions shift scale degrees. Use `.transpose(7)` after `.scale(...)` to shift resolved pitches by seven semitones. Bare `n(...).add(7)` is rejected as arithmetic on a control pattern.

Each control string spans its own cycle. To alternate gain on four notes, use four gain values:

```js
note("c4 e4 g4 c5").s('triangle')
  .gain("0.6 0.15 0.6 0.15").pan("0 1 0 1")
```

`.every(4, x => x.rev())` transforms cycles 0, 4, 8, and so on in the pinned runtime. It starts on the first cycle of each group, rather than waiting until the fourth.

Continue with [Learn Strudel](https://github.com/ekollof/tompouce/wiki/Learn-Strudel) and the [full key reference](https://github.com/ekollof/tompouce/blob/master/README.md#terminal-ui).
