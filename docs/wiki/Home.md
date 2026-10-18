# Tompouce documentation

Tompouce is a terminal live-coding environment for music. It embeds Strudel 1.2.6 in QuickJS, schedules patterns locally, and renders audio in C. The Notcurses editor, native audio engine, optional Chromium companion and MCP interface work with the same session.

Start with [Getting started](Getting-Started.md), then try the bundled 22-lesson Strudeltutor. A simple synth pattern works without sample banks or a browser:

```js
setcpm(108 / 4)
note("c4 e4 g4 c5").s('triangle').gain(0.4)
```

Press F5 to evaluate and Ctrl+P to play or stop.

## Guides

| Guide | What you will find |
| --- | --- |
| [Getting started](Getting-Started.md) | Installation, first sound and updates |
| [Live coding and Strudeltutor](Live-Coding-and-Strudeltutor.md) | Editing, lessons, songs, saves and pattern fundamentals |
| [Sounds and samples](Sounds-and-Samples.md) | F6, drum machines, GM synths, wavetables and imports |
| [Visuals and themes](Visuals-and-Themes.md) | Inline views, Kitty graphics and live recoloring |
| [Companion and integrations](Companion-and-Integrations.md) | Browser controls, MCP, MIDI and OSC |
| [Development](Development.md) | CMake/Ninja, tests, architecture and contributions |
| [Troubleshooting](Troubleshooting.md) | Silence, failed evaluation, missing sounds and tutorial updates |
| [Learn Strudel](Learn-Strudel.md) | Official workshops, reference pages and practice ideas |

## Project and community

- [Source and README](https://github.com/ekollof/tompouce)
- [Discussions](https://github.com/ekollof/tompouce/discussions): questions, musical experiments, ideas and showcases.
- [Issues](https://github.com/ekollof/tompouce/issues): reproducible bugs and actionable reports.
- [JavaScript and native audio reference](https://github.com/ekollof/tompouce/blob/master/docs/javascript.md)
- [License and third-party notices](https://github.com/ekollof/tompouce/blob/master/NOTICE)

Original project code is AGPL-3.0-or-later. Dependencies and sound banks retain their own licenses and attribution. Repository and documentation access follow the project's GitHub visibility settings.

Tompouce uses native audio rather than the browser's WebAudio engine. Upstream Strudel examples are useful for learning patterns, but browser-only integrations, every audio parameter and exact browser waveforms are not all reproduced. Consult the native audio reference when adapting a pattern.
