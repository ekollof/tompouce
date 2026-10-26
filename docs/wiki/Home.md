# Tompouce documentation

Tompouce is a terminal live-coding environment for music. Its aim is parity with the Strudel web REPL: it embeds Strudel 1.2.6 in QuickJS, schedules patterns locally, and renders audio in C. The Notcurses editor, native audio engine, optional Chromium companion and MCP interface work with the same session.

The performance keys match the web editor. Ctrl+Enter or Alt+Enter evaluates and starts playback. Ctrl+. or Alt+. stops. F5 and Ctrl+P do those same jobs. Alt+W and Alt+Q move between `$` labels.

Patterns can also leave the machine. ALSA carries MIDI notes, channels and controller input. `.osc()` sends timestamped SuperDirt bundles over UDP, by default to `127.0.0.1:57120`. See [Companion and integrations](https://github.com/ekollof/tompouce/wiki/Companion-and-Integrations).

Start with [Getting started](https://github.com/ekollof/tompouce/wiki/Getting-Started), then try the bundled 22-lesson Strudeltutor. A simple synth pattern works without sample banks or a browser:

```js
setcpm(108 / 4)
note("c4 e4 g4 c5").s('triangle').gain(0.4)
```

Press F5 to evaluate and Ctrl+P to play or stop.

## Guides

| Guide | What you will find |
| --- | --- |
| [Getting started](https://github.com/ekollof/tompouce/wiki/Getting-Started) | Installation, first sound and updates |
| [Live coding and Strudeltutor](https://github.com/ekollof/tompouce/wiki/Live-Coding-and-Strudeltutor) | Editing, lessons, songs, saves and pattern fundamentals |
| [Prebake setup files](https://github.com/ekollof/tompouce/wiki/Prebakes) | Reusable helpers, automatic setup and the Switch Angel starter |
| [Sounds and samples](https://github.com/ekollof/tompouce/wiki/Sounds-and-Samples) | F6, drum machines, GM synths, wavetables and imports |
| [Visuals and themes](https://github.com/ekollof/tompouce/wiki/Visuals-and-Themes) | Inline views, Kitty graphics and live recoloring |
| [Companion and integrations](https://github.com/ekollof/tompouce/wiki/Companion-and-Integrations) | Browser controls, MCP, MIDI and OSC |
| [Development](https://github.com/ekollof/tompouce/wiki/Development) | CMake/Ninja, tests, architecture and contributions |
| [Troubleshooting](https://github.com/ekollof/tompouce/wiki/Troubleshooting) | Silence, failed evaluation, missing sounds and tutorial updates |
| [Learn Strudel](https://github.com/ekollof/tompouce/wiki/Learn-Strudel) | Official workshops, reference pages and practice ideas |

## Project and community

- [Website](https://ekollof.github.io/tompouce/)
- [Source and README](https://github.com/ekollof/tompouce)
- [Discussions](https://github.com/ekollof/tompouce/discussions): questions, musical experiments, ideas and showcases.
- [Issues](https://github.com/ekollof/tompouce/issues): reproducible bugs and actionable reports.
- [JavaScript and native audio reference](https://github.com/ekollof/tompouce/blob/master/docs/javascript.md)
- [License and third-party notices](https://github.com/ekollof/tompouce/blob/master/NOTICE)

Original project code is AGPL-3.0-or-later. Dependencies and sound banks retain their own licenses and attribution. Repository and documentation access follow the project's GitHub visibility settings.

Tompouce uses native audio rather than the browser's WebAudio engine. Upstream Strudel examples are useful for learning patterns, but browser-only integrations, every audio parameter and exact browser waveforms are not all reproduced. Consult the native audio reference when adapting a pattern.
