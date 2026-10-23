# Troubleshooting

## No sound

1. Check that transport is playing; Ctrl+P toggles it. Tutorial lesson 01 intentionally starts with `gain(0)`.
2. Press F5 after a musical edit. Read the console for evaluation or missing-source errors.
3. Open F4 and inspect backend, stream, voices and queue. A healthy output stream alone does not mean transport is playing.
4. Try `note("c4").s('sine').gain(0.4)` to separate basic output from sample-bank problems.

Startup tries PipeWire, PulseAudio, PortAudio JACK, then PortAudio ALSA. Failed attempts clean up before fallback. With no usable backend, editing and MCP remain available; external MIDI/OSC may still schedule. Runtime disconnects report an inactive backend rather than silently restarting playback.

## CPU optimizations

The convolution engine automatically chooses AVX2, AVX, SSE2 or portable C at
startup. An older CPU does not need AVX2 to run Tompouce. To compare against the
portable implementation, start a new session with:

```sh
TOMPOUCE_SIMD=c tompouce
```

`TOMPOUCE_SIMD=sse2`, `avx` and `avx2` request a specific kernel; unavailable
requests fall back to C with a console diagnostic. Invalid values also use C.
Changing the variable does not affect an already running process. When reporting
an audio regression, include the CPU model and whether forcing C changes it.
See [Development](Development.md#cpu-dispatch-and-benchmarks) for verification.

## Output volume and mute

If every song stays silent, check **F4 Volume** or use F2 `/volume 1`.
Output mute survives song changes and reevaluation within the current process.

## My edit has no effect

Music changes require F5; adding a supported visual call can activate immediately. Failed evaluation retains the previous playable pattern. Check the console and F4's pending-evaluation state. If you installed a new executable, close/reopen Tompouce normally: a running process keeps its previous executable.

Check `command -v tompouce` when a checkout build and installed copy behave differently. CMake produces `build/tompouce`; the default installed launcher is `~/.local/bin/tompouce`.

## Arithmetic warnings

Bare `n(...).add(7)` applies arithmetic to a control pattern and is rejected by the pinned runtime. Use raw-pattern arithmetic inside the control constructor:

```js
n("0 2 4".add("<0 3 4 0>")).scale('C:major').s('triangle')
```

For a semitone shift after scale resolution, use `.transpose(7)` instead.

## Missing sounds

F6 lists the loaded catalog. Banks skipped during installation are unavailable offline until installed. `gm_` sounds use the installed `gm-webaudiofont` packs (or fetch them on first play); only when packs are unreachable do they need FluidSynth and a usable GM soundfont. `wt_` banks require their installed AKWF catalog/audio; procedural `wt:` oscillators are separate.

Explicit `samples()` maps override defaults; a failed explicit map is not silently replaced with another instrument. Review the console, installed bank paths and [Sounds and samples](Sounds-and-Samples.md).

## Tutorial changes are missing

`/tutor` reuses your saved config copy to preserve progress. Updating the bundled master does not rewrite that copy, and `/tutor!` is not a reset command. Apply corrections to your working lesson, or load the bundled `examples/strudeltutor.json` from your installation and save it under a new filename. Back up existing progress before replacing any tutorial file.

## Colors, graphics and mouse

Enable `/theme omarchy`, `/theme pywal` or `/theme auto`; default `/theme off` does not watch files. Check the paths listed in [Visuals and themes](Visuals-and-Themes.md). Missing/invalid files keep the last valid palette.

Use `/graphics auto` in a Kitty-capable terminal; tmux/screen and unsupported terminals use braille. `/inline off` hides inline views independently of F12. Hold Shift for the terminal's native mouse selection; Escape returns pane focus to the editor. F1 documents active shortcuts.

## Report a problem

Use [Discussions](https://github.com/ekollof/tompouce/discussions) for help or [Issues](https://github.com/ekollof/tompouce/issues) for a reproducible bug. Include the commit/version, terminal, distro, selected backend from F4, minimal pattern, expected/actual behavior and relevant console error. Avoid sharing credentials or private musical source unnecessarily.
